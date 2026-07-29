#!/usr/bin/env python3
"""rton_writer.py — RTON v1 binary format encoder (serde_rton-compatible).

Encodes Python data structures back into the RTON v1 binary format.
Complements rton_parser.py to enable full round-trip modding.

Usage:
  python rton_writer.py input.json output.RTON       # Convert JSON to RTON
  python rton_writer.py --dir folder/                # Batch convert directory
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional


def _write_varint(val: int) -> bytes:
    """Encode a value as LEB128 unsigned varint."""
    buf = bytearray()
    while True:
        byte = val & 0x7F
        val >>= 7
        if val:
            byte |= 0x80
        buf.append(byte)
        if not val:
            break
    return bytes(buf)


def _write_signed_varint(val: int) -> bytes:
    """Encode a signed int as ZigZag LEB128 varint."""
    uval = (val << 1) ^ (val >> 63)
    return _write_varint(uval)


# Pattern for RTID strings: hex.hex.hex@name, hex.hex.hex@, or s1@s2
_RTID_FULL = re.compile(r'^([0-9a-fA-F]+)\.([0-9a-fA-F]+)\.([0-9a-fA-F]{8})@(.+)$')
_RTID_NO_NAME = re.compile(r'^([0-9a-fA-F]+)\.([0-9a-fA-F]+)\.([0-9a-fA-F]{8})@$')
_RTID_RAW = re.compile(r'^([^@]+)@(.+)$')


class RtonWriter:
    """RTON v1 encoder with string caching."""

    def __init__(self):
        self.latin1_cache: Dict[str, int] = {}
        self.utf8_cache: Dict[str, int] = {}
        self.latin1_list: List[str] = []
        self.utf8_list: List[str] = []

    def _is_latin1(self, s: str) -> bool:
        return all(ord(c) < 256 for c in s)

    def _encode_latin1_string_payload(self, s: str) -> bytes:
        if not self._is_latin1(s):
            raise ValueError(f"Cannot encode as Latin-1: {s!r}")
        return _write_varint(len(s)) + s.encode("latin1")

    def _encode_utf8_string_payload(self, s: str) -> bytes:
        encoded = s.encode("utf-8")
        char_count = len(s)
        byte_len = len(encoded)
        return _write_varint(char_count) + _write_varint(byte_len) + encoded

    def _write_string(self, s: str) -> bytes:
        if s == "*":
            return bytes([0x02])
        if self._is_latin1(s):
            if s in self.latin1_cache:
                idx = self.latin1_cache[s]
                return bytes([0x91]) + _write_varint(idx)
            idx = len(self.latin1_list)
            self.latin1_cache[s] = idx
            self.latin1_list.append(s)
            return bytes([0x90]) + self._encode_latin1_string_payload(s)
        else:
            if s in self.utf8_cache:
                idx = self.utf8_cache[s]
                return bytes([0x93]) + _write_varint(idx)
            idx = len(self.utf8_list)
            self.utf8_cache[s] = idx
            self.utf8_list.append(s)
            return bytes([0x92]) + self._encode_utf8_string_payload(s)

    def _write_int(self, val: int) -> bytes:
        if val == 0:
            return bytes([0x21])  # I32Zero (safe default)
        if val < 0:
            if val >= -128:
                return struct.pack("<Bb", 0x08, val)
            if val >= -32768:
                return struct.pack("<Bh", 0x10, val)
            if val >= -2147483648:
                return struct.pack("<Bi", 0x20, val)
            return struct.pack("<Bq", 0x40, val)
        else:
            if val <= 0xFF:
                return struct.pack("<BB", 0x0a, val)
            if val <= 0xFFFF:
                return struct.pack("<BH", 0x12, val)
            if val <= 0xFFFFFFFF:
                return struct.pack("<BI", 0x26, val)
            return struct.pack("<BQ", 0x46, val)

    def _write_float(self, val: float) -> bytes:
        if val == 0.0:
            return bytes([0x43])  # F64Zero
        return struct.pack("<Bd", 0x42, val)

    def _write_rtid(self, s: str) -> bytes:
        # hex.hex.hex@name → UidWithName (sub_tag 0x02)
        m = _RTID_FULL.match(s)
        if m:
            v1 = int(m.group(1), 16)
            v2 = int(m.group(2), 16)
            x = int(m.group(3), 16)
            name = m.group(4)
            name_bytes = self._encode_utf8_string_payload(name)
            payload = bytes([0x02]) + name_bytes + _write_varint(v2) + _write_varint(v1) + struct.pack("<I", x)
            return bytes([0x83]) + payload
        # hex.hex.hex@ → UidWithoutName (sub_tag 0x01)
        m = _RTID_NO_NAME.match(s)
        if m:
            v1 = int(m.group(1), 16)
            v2 = int(m.group(2), 16)
            x = int(m.group(3), 16)
            payload = bytes([0x01]) + _write_varint(v2) + _write_varint(v1) + struct.pack("<I", x)
            return bytes([0x83]) + payload
        # s1@s2 → RawString (sub_tag 0x03) — most PvZ2 RTIDs use this
        m = _RTID_RAW.match(s)
        if m:
            s1, s2 = m.groups()
            payload = bytes([0x03]) + self._encode_utf8_string_payload(s1) + self._encode_utf8_string_payload(s2)
            return bytes([0x83]) + payload
        # Fall back to regular string
        return self._write_string(s)

    def write_value(self, val: Any, is_root: bool = False) -> bytes:
        """Recursively encode a Python value to RTON v1 bytes."""
        if isinstance(val, dict):
            # Check for RTID
            if len(val) == 1 and "RTID" in val and isinstance(val["RTID"], str):
                return self._write_rtid(val["RTID"])
            return self._write_object(val, is_root=is_root)
        if isinstance(val, list):
            return self._write_array(val)
        if isinstance(val, bool):
            return bytes([0x01 if val else 0x00])
        if val is None:
            return bytes([0x84])  # RtidNull
        if isinstance(val, int):
            return self._write_int(val)
        if isinstance(val, float):
            return self._write_float(val)
        if isinstance(val, str):
            return self._write_string(val)
        if isinstance(val, bytes):
            # Binary blob
            hex_str = val.hex()
            return (bytes([0x87, 0x90]) +
                    self._encode_latin1_string_payload(hex_str) +
                    _write_varint(len(val)) + val)
        raise ValueError(f"Cannot encode: {type(val).__name__} {val!r}")

    def _write_object(self, obj: Dict[str, Any], is_root: bool = False) -> bytes:
        buf = bytearray()
        if not is_root:
            buf.append(0x85)  # ObjectBegin
        for key, val in obj.items():
            buf.extend(self.write_value(key))
            buf.extend(self.write_value(val))
        buf.append(0xFF)  # ObjectEnd (root also needs terminator)
        return bytes(buf)

    def _write_array(self, arr: List[Any]) -> bytes:
        buf = bytearray()
        buf.append(0x86)  # ArrayBegin
        buf.append(0xFD)  # ArrayCapacity
        buf.extend(_write_varint(len(arr)))
        for elem in arr:
            buf.extend(self.write_value(elem))
        buf.append(0xFE)  # ArrayEnd
        return bytes(buf)

    def encode(self, obj: Any) -> bytes:
        """Encode a Python object as a complete RTON v1 file."""
        self.__init__()  # Reset state
        header = b"RTON" + struct.pack("<I", 1)
        body = self.write_value(obj, is_root=True)
        footer = b"DONE"
        return header + body + footer


def parse_args():
    parser = argparse.ArgumentParser(description="RTON v1 encoder (JSON -> RTON)")
    parser.add_argument("input", type=Path, help="Input JSON file or directory (with --dir)")
    parser.add_argument("output", type=Path, nargs="?", help="Output RTON file")
    parser.add_argument("--dir", action="store_true",
                        help="Treat input as directory, convert all .json files")
    parser.add_argument("--extract-all", type=Path, default=None,
                        help="Convert all .json files from a directory tree to RTON")
    return parser.parse_args()


def main():
    args = parse_args()

    if args.dir or args.extract_all:
        base = args.extract_all or args.input
        if not base.is_dir():
            print(f"error: {base} is not a directory", file=sys.stderr)
            sys.exit(1)

        output_base = args.output or base / "rton_out"
        output_base.mkdir(parents=True, exist_ok=True)

        json_files = list(base.rglob("*.json"))
        print(f"Found {len(json_files)} JSON files in {base}/", file=sys.stderr)

        ok = fail = 0
        for jf in json_files:
            rel = jf.relative_to(base)
            try:
                with open(jf, "r", encoding="utf-8") as f:
                    root = json.load(f)
                writer = RtonWriter()
                rton_data = writer.encode(root)
                out_file = output_base / str(rel).replace(".json", ".RTON")
                out_file.parent.mkdir(parents=True, exist_ok=True)
                out_file.write_bytes(rton_data)
                print(f"  OK {rel} -> {out_file}", file=sys.stderr)
                ok += 1
            except Exception as e:
                print(f"  FAIL {rel}: {e}", file=sys.stderr)
                fail += 1

        print(f"\nDone: {ok} OK, {fail} failed. Output in {output_base}/", file=sys.stderr)
        return

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


if __name__ == "__main__":
    main()
