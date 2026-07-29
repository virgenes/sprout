#!/usr/bin/env python3
"""rton_parser.py — RTON v1 binary format parser (serde_rton-compatible).

RTON is a binary serialization format used by PopCap games.
This parser implements the v1 standard format as defined by the serde_rton crate.

Format:
  Header: b"RTON" + u32 LE version (=1 for v1)
  Body: Token stream (root is implicit object, no leading 0x85)
  Footer: Optional b"DONE"

Token reference (from serde_rton tags.rs):
  0x00 BooleanFalse    0x01 BooleanTrue      0x02 StringAsterisk
  0x08 I8              0x09 I8Zero           0x0a U8           0x0b U8Zero
  0x10 I16             0x11 I16Zero          0x12 U16          0x13 U16Zero
  0x20 I32             0x21 I32Zero          0x26 U32          0x27 U32Zero
  0x40 I64             0x41 I64Zero          0x46 U64          0x47 U64Zero
  0x24 RawVarInt32     0x25 ZigZagVarInt32   0x28 UnsignedVarInt32
  0x44 RawVarInt64     0x45 ZigZagVarInt64   0x48 UnsignedVarInt64
  0x22 F32             0x23 F32Zero          0x42 F64          0x43 F64Zero
  0x81 StringLatin1Direct         0x82 StringUtf8Direct
  0x90 StringLatin1Definition     0x91 StringLatin1Reference
  0x92 StringUtf8Definition       0x93 StringUtf8Reference
  0x83 Rtid             0x84 RtidNull
  0x85 ObjectBegin      0x86 ArrayBegin       0x87 BinaryBlob
  0xFD ArrayCapacity    0xFE ArrayEnd         0xFF ObjectEnd

Usage:
  python rton_parser.py file.RTON              # Print as JSON
  python rton_parser.py file.RTON output.json  # Save to file
  python rton_parser.py --dir folder/           # Batch convert all .RTON files
"""

import argparse
import json
import struct
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


# === LEB128 varint reader ===

def _read_varint(data: bytes, pos: int) -> Tuple[int, int]:
    value = 0
    shift = 0
    start = pos
    while True:
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not (byte & 0x80):
            break
    return value, pos - start


def _read_signed_varint(data: bytes, pos: int) -> Tuple[int, int]:
    uval, n = _read_varint(data, pos)
    # ZigZag decode: (n >> 1) ^ -(n & 1)
    ival = (uval >> 1) ^ (-(uval & 1))
    return ival, n


# === Token decode table ===

# Simple fixed-size token -> size mapping (0 = dynamic/variable-length)
TOKEN_PAYLOAD_SIZE: Dict[int, int] = {
    # 0x00-0x01: booleans (0 bytes payload)
    0x00: 0, 0x01: 0,
    # 0x02: "*" string (0 bytes)
    0x02: 0,
    # Fixed-width ints
    0x08: 1, 0x09: 0, 0x0a: 1, 0x0b: 0,
    0x10: 2, 0x11: 0, 0x12: 2, 0x13: 0,
    0x20: 4, 0x21: 0, 0x26: 4, 0x27: 0,
    0x40: 8, 0x41: 0, 0x46: 8, 0x47: 0,
    # Floats
    0x22: 4, 0x23: 0, 0x42: 8, 0x43: 0,
}

TOKEN_NAMES: Dict[int, str] = {
    0x00: "BooleanFalse", 0x01: "BooleanTrue", 0x02: "StringAsterisk",
    0x08: "I8", 0x09: "I8Zero", 0x0a: "U8", 0x0b: "U8Zero",
    0x10: "I16", 0x11: "I16Zero", 0x12: "U16", 0x13: "U16Zero",
    0x20: "I32", 0x21: "I32Zero", 0x26: "U32", 0x27: "U32Zero",
    0x40: "I64", 0x41: "I64Zero", 0x46: "U64", 0x47: "U64Zero",
    0x24: "RawVarInt32", 0x25: "ZigZagVarInt32", 0x28: "UnsignedVarInt32",
    0x44: "RawVarInt64", 0x45: "ZigZagVarInt64", 0x48: "UnsignedVarInt64",
    0x22: "F32", 0x23: "F32Zero", 0x42: "F64", 0x43: "F64Zero",
    0x81: "StringLatin1Direct", 0x82: "StringUtf8Direct",
    0x90: "StringLatin1Definition", 0x91: "StringLatin1Reference",
    0x92: "StringUtf8Definition", 0x93: "StringUtf8Reference",
    0x83: "Rtid", 0x84: "RtidNull",
    0x85: "ObjectBegin", 0x86: "ArrayBegin", 0x87: "BinaryBlob",
    0xFD: "ArrayCapacity", 0xFE: "ArrayEnd", 0xFF: "ObjectEnd",
}

# Tags that start compound structures (push onto scope stack)
COMPOUND_BEGIN: set = {0x85, 0x86}
# Tags that end compound structures
COMPOUND_END: set = {0xFE, 0xFF}


class RtonParseError(Exception):
    pass


class RtonReader:
    """RTON v1 deserializer mirroring serde_rton logic."""

    def __init__(self, data: bytes):
        if len(data) < 8:
            raise RtonParseError("File too small for RTON header")
        if data[:4] != b"RTON":
            raise RtonParseError(f"Bad magic: {data[:4]!r}, expected RTON")
        self.data = data
        self.pos = 8  # Past header
        version = struct.unpack_from("<I", data, 4)[0]
        if version != 1:
            raise RtonParseError(f"Unsupported RTON version: {version}")

        # String caches (per serde_rton)
        self.latin1_strings: List[str] = []
        self.utf8_strings: List[str] = []

    def _remaining(self) -> int:
        return len(self.data) - self.pos

    def _peek(self) -> int:
        return self.data[self.pos]

    def _skip_footer(self):
        if self._remaining() >= 4 and self.data[self.pos:self.pos+4] == b"DONE":
            self.pos += 4

    # === Varint helpers ===

    def _read_varint(self) -> int:
        val, n = _read_varint(self.data, self.pos)
        self.pos += n
        return val

    def _read_zigzag_varint(self) -> int:
        ival, n = _read_signed_varint(self.data, self.pos)
        self.pos += n
        return ival

    # === String helpers ===

    def _read_latin1_chars(self, count: int) -> str:
        end = self.pos + count
        raw = self.data[self.pos:end]
        self.pos = end
        return "".join(chr(b) for b in raw)

    def _read_latin1_string_payload(self) -> str:
        char_count = self._read_varint()
        return self._read_latin1_chars(char_count)

    def _read_utf8_chars(self, count: int) -> str:
        result = []
        for _ in range(count):
            b = self.data[self.pos]
            self.pos += 1
            if b & 0x80 == 0:
                result.append(chr(b))
            elif b & 0xE0 == 0xC0:
                b2 = self.data[self.pos]; self.pos += 1
                result.append(chr(((b & 0x1F) << 6) | (b2 & 0x3F)))
            elif b & 0xF0 == 0xE0:
                b2 = self.data[self.pos]; self.pos += 1
                b3 = self.data[self.pos]; self.pos += 1
                result.append(chr(((b & 0x0F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F)))
            elif b & 0xF8 == 0xF0:
                b2 = self.data[self.pos]; self.pos += 1
                b3 = self.data[self.pos]; self.pos += 1
                b4 = self.data[self.pos]; self.pos += 1
                result.append(chr(((b & 0x07) << 18) | ((b2 & 0x3F) << 12) | ((b3 & 0x3F) << 6) | (b4 & 0x3F)))
            else:
                raise RtonParseError(f"Invalid UTF-8 start byte: 0x{b:02x}")
        return "".join(result)

    def _read_utf8_string_payload(self) -> str:
        char_count = self._read_varint()
        byte_len = self._read_varint()
        return self._read_utf8_chars(char_count)

    # === Main value reader ===

    def read_value(self, is_root: bool = False) -> Any:
        if is_root:
            return self._read_root()

        return self._read_any_value()

    def _read_root(self) -> Dict[str, Any]:
        """Root is an implicit object."""
        return self._read_object_body()

    def _read_any_value(self) -> Any:
        tag = self.data[self.pos]
        self.pos += 1

        # Booleans
        if tag == 0x00:
            return False
        if tag == 0x01:
            return True
        if tag == 0x02:
            return "*"

        # Zero-value fixed-width ints
        if tag == 0x09: return 0  # I8Zero
        if tag == 0x0b: return 0  # U8Zero
        if tag == 0x11: return 0  # I16Zero
        if tag == 0x13: return 0  # U16Zero
        if tag == 0x21: return 0  # I32Zero
        if tag == 0x27: return 0  # U32Zero
        if tag == 0x41: return 0  # I64Zero
        if tag == 0x47: return 0  # U64Zero
        if tag == 0x23: return 0.0  # F32Zero
        if tag == 0x43: return 0.0  # F64Zero

        # Fixed-width ints
        if tag == 0x08:  # I8
            val = struct.unpack_from("<b", self.data, self.pos)[0]
            self.pos += 1
            return val
        if tag == 0x0a:  # U8
            val = self.data[self.pos]
            self.pos += 1
            return val
        if tag == 0x10:  # I16
            val = struct.unpack_from("<h", self.data, self.pos)[0]
            self.pos += 2
            return val
        if tag == 0x12:  # U16
            val = struct.unpack_from("<H", self.data, self.pos)[0]
            self.pos += 2
            return val
        if tag == 0x20:  # I32
            val = struct.unpack_from("<i", self.data, self.pos)[0]
            self.pos += 4
            return val
        if tag == 0x26:  # U32
            val = struct.unpack_from("<I", self.data, self.pos)[0]
            self.pos += 4
            return val
        if tag == 0x40:  # I64
            val = struct.unpack_from("<q", self.data, self.pos)[0]
            self.pos += 8
            return val
        if tag == 0x46:  # U64
            val = struct.unpack_from("<Q", self.data, self.pos)[0]
            self.pos += 8
            return val

        # Varints
        if tag == 0x24:  # RawVarInt32
            return self._read_varint()
        if tag == 0x25:  # ZigZagVarInt32
            return self._read_zigzag_varint()
        if tag == 0x28:  # UnsignedVarInt32
            return self._read_varint()
        if tag == 0x44:  # RawVarInt64
            return self._read_varint()
        if tag == 0x45:  # ZigZagVarInt64
            return self._read_zigzag_varint()
        if tag == 0x48:  # UnsignedVarInt64
            return self._read_varint()

        # Floats
        if tag == 0x22:  # F32
            val = struct.unpack_from("<f", self.data, self.pos)[0]
            self.pos += 4
            return val
        if tag == 0x42:  # F64
            val = struct.unpack_from("<d", self.data, self.pos)[0]
            self.pos += 8
            return val

        # String types
        if tag == 0x81:  # StringLatin1Direct
            return self._read_latin1_string_payload()
        if tag == 0x90:  # StringLatin1Definition
            s = self._read_latin1_string_payload()
            self.latin1_strings.append(s)
            return s
        if tag == 0x91:  # StringLatin1Reference
            idx = self._read_varint()
            if idx >= len(self.latin1_strings):
                raise RtonParseError(f"Latin1 string ref {idx} out of bounds (have {len(self.latin1_strings)})")
            return self.latin1_strings[idx]

        if tag == 0x82:  # StringUtf8Direct
            return self._read_utf8_string_payload()
        if tag == 0x92:  # StringUtf8Definition
            s = self._read_utf8_string_payload()
            self.utf8_strings.append(s)
            return s
        if tag == 0x93:  # StringUtf8Reference
            idx = self._read_varint()
            if idx >= len(self.utf8_strings):
                raise RtonParseError(f"UTF-8 string ref {idx} out of bounds (have {len(self.utf8_strings)})")
            return self.utf8_strings[idx]

        # RTID
        if tag == 0x83:  # Rtid
            return self._read_rtid()
        if tag == 0x84:  # RtidNull
            return None

        # Object
        if tag == 0x85:  # ObjectBegin
            return self._read_object_body()

        # Array
        if tag == 0x86:  # ArrayBegin
            return self._read_array_body()

        # Binary blob
        if tag == 0x87:  # BinaryBlob
            marker = self.data[self.pos]; self.pos += 1  # sub-tag (usually 0x90)
            hex_str = self._read_latin1_string_payload()
            declared_len = self._read_varint()
            # Skip raw payload
            self.pos += declared_len
            try:
                raw = bytes.fromhex(hex_str)
                return raw
            except ValueError:
                return f"<binary hex:{hex_str}>"

        # Unexpected compound-end or capacity tag
        if tag in (0xFE, 0xFF):
            raise RtonParseError(f"Unexpected end marker 0x{tag:02x} at pos {self.pos-1}")
        if tag == 0xFD:
            # Capacity without ArrayBegin — read and skip
            self._read_varint()
            return self.read_value()

        raise RtonParseError(f"Unknown tag 0x{tag:02x} at pos {self.pos-1}")

    def _read_rtid(self) -> Any:
        sub_tag = self.data[self.pos]
        self.pos += 1
        if sub_tag == 0x00:
            return None
        if sub_tag == 0x01:  # UidWithoutName
            v2 = self._read_varint()
            v1 = self._read_varint()
            x = struct.unpack_from("<I", self.data, self.pos)[0]
            self.pos += 4
            return {"RTID": f"{v1:x}.{v2:x}.{x:08x}@"}
        if sub_tag == 0x02:  # UidWithName
            name = self._read_utf8_string_payload()
            v2 = self._read_varint()
            v1 = self._read_varint()
            x = struct.unpack_from("<I", self.data, self.pos)[0]
            self.pos += 4
            return {"RTID": f"{v1:x}.{v2:x}.{x:08x}@{name}"}
        if sub_tag == 0x03:  # RawString
            s1 = self._read_utf8_string_payload()
            s2 = self._read_utf8_string_payload()
            return {"RTID": f"{s1}@{s2}"}
        raise RtonParseError(f"Unknown RTID sub-tag 0x{sub_tag:02x}")

    def _read_array_body(self) -> List[Any]:
        """Array: 0x86 + 0xFD + varint(capacity) + elements... + 0xFE"""
        if self.data[self.pos] != 0xFD:
            raise RtonParseError(f"Expected ArrayCapacity (0xFD) after ArrayBegin, got 0x{self.data[self.pos]:02x}")
        self.pos += 1
        capacity = self._read_varint()
        arr = []
        for _ in range(capacity):
            elem = self._read_any_value()
            arr.append(elem)
        if self.data[self.pos] != 0xFE:
            raise RtonParseError(f"Expected ArrayEnd (0xFE) after array elements, got 0x{self.data[self.pos]:02x}")
        self.pos += 1
        return arr

    def _read_object_body(self) -> Dict[str, Any]:
        """Object: key-value pairs terminated by 0xFF."""
        obj = {}
        while self.pos < len(self.data):
            tag = self.data[self.pos]
            if tag == 0xFF:
                self.pos += 1
                break
            key = self._read_any_value()
            if not isinstance(key, str):
                key = str(key)
            val = self._read_any_value()
            obj[key] = val
        return obj


def parse_args():
    parser = argparse.ArgumentParser(description="RTON v1 parser/encoder (serde_rton-compatible)")
    parser.add_argument("input", type=Path, help="RTON file or directory (with --dir)")
    parser.add_argument("output", type=Path, nargs="?", help="Output JSON file")
    parser.add_argument("--dir", action="store_true",
                        help="Treat input as directory, parse all .RTON files")
    parser.add_argument("--pretty", action="store_true",
                        help="Pretty-print output")
    parser.add_argument("--extract-all", type=Path, default=None,
                        help="Extract all .RTON files from a directory tree to JSON")
    parser.add_argument("--to-rton", action="store_true",
                        help="Convert JSON input to RTON output")
    return parser.parse_args()


def main():
    args = parse_args()

    if args.to_rton:
        from rton_writer import RtonWriter
        if not args.input.exists():
            print(f"error: {args.input} not found", file=sys.stderr)
            sys.exit(1)
        with open(args.input, "r", encoding="utf-8") as f:
            root = json.load(f)
        writer = RtonWriter()
        rton_data = writer.encode(root)
        if args.output:
            args.output.write_bytes(rton_data)
            print(f"Written to {args.output}", file=sys.stderr)
        else:
            sys.stdout.buffer.write(rton_data)
        return

    if args.dir or args.extract_all:
        base = args.extract_all or args.input
        if not base.is_dir():
            print(f"error: {base} is not a directory", file=sys.stderr)
            sys.exit(1)

        output_base = args.output or base / "rton_json"
        output_base.mkdir(parents=True, exist_ok=True)

        rton_files = list(base.rglob("*.RTON")) + list(base.rglob("*.rton"))
        print(f"Found {len(rton_files)} RTON files in {base}/", file=sys.stderr)

        ok = fail = 0
        for rf in rton_files:
            rel = rf.relative_to(base)
            try:
                data = rf.read_bytes()
                reader = RtonReader(data)
                root = reader._read_root()
                reader._skip_footer()
                out_file = output_base / str(rel).replace(".RTON", ".json").replace(".rton", ".json")
                out_file.parent.mkdir(parents=True, exist_ok=True)
                with open(out_file, "w", encoding="utf-8") as f:
                    if args.pretty:
                        json.dump(root, f, indent=2, ensure_ascii=False)
                    else:
                        json.dump(root, f, ensure_ascii=False)
                print(f"  \u2713 {rel} -> {out_file}", file=sys.stderr)
                ok += 1
            except Exception as e:
                print(f"  \u2717 {rel}: {e}", file=sys.stderr)
                fail += 1

        print(f"\nDone: {ok} OK, {fail} failed. Output in {output_base}/", file=sys.stderr)
        return

    if not args.input.exists():
        print(f"error: {args.input} not found", file=sys.stderr)
        sys.exit(1)

    data = args.input.read_bytes()
    reader = RtonReader(data)
    root = reader._read_root()
    reader._skip_footer()

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            if args.pretty:
                json.dump(root, f, indent=2, ensure_ascii=False)
            else:
                json.dump(root, f, ensure_ascii=False)
        print(f"Written to {args.output}", file=sys.stderr)
    else:
        kwargs = {"indent": 2, "ensure_ascii": False} if args.pretty else {"ensure_ascii": False}
        json.dump(root, sys.stdout, **kwargs)
        print()


if __name__ == "__main__":
    main()
