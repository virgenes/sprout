#!/usr/bin/env python3
"""rton_parser.py — PopCap RTON (Resource TOken Notation) binary format parser.

RTON is a binary serialization format used by PopCap games (PvZ2, etc.) to store
game configuration data such as plant stats, zombie stats, level definitions,
strings, and other resources.

Known RTON files in PvZ2 (inside OBB → pgsr groups):
  - PROPERTIES/RESOURCES.RTON  — Main game config (plants, zombies, levels, etc.)
  - PROPERTIES/*.RTON          — Additional property files
  - STRINGS_*/*.RTON           — Localized strings
  - RTON files in other groups — Various game data

Format (reverse-engineered):
  Header:
    +0x00: magic "RTON"
    +0x04: version (typically 0x0100 or 0x0200)
    +0x06: flags
    +0x08: string table offset
    +0x0C: string table size
    +0x10: root object offset

  Token types:
    0x00: null
    0x01: boolean false
    0x02: boolean true
    0x03: int8
    0x04: uint8
    0x05: int16
    0x06: uint16
    0x07: int32
    0x08: uint32
    0x09: int64
    0x0A: uint64
    0x0B: float32
    0x0C: float64
    0x0D: string (index into string table)
    0x0E: binary data
    0x0F: array
    0x10: object (key-value pairs)
    0x11: object_end marker
    0x12: array_end marker

Usage:
    python rton_parser.py PROPERTIES/RESOURCES.RTON  # Parse and print as JSON
    python rton_parser.py PROPERTIES/RESOURCES.RTON output.json  # Save to file
    python rton_parser.py --dir extracted/  # Parse all RTON files in directory
"""

import argparse
import json
import struct
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


RTON_MAGIC = b"RTON"

# Token types
TOKEN_NULL = 0x00
TOKEN_FALSE = 0x01
TOKEN_TRUE = 0x02
TOKEN_INT8 = 0x03
TOKEN_UINT8 = 0x04
TOKEN_INT16 = 0x05
TOKEN_UINT16 = 0x06
TOKEN_INT32 = 0x07
TOKEN_UINT32 = 0x08
TOKEN_INT64 = 0x09
TOKEN_UINT64 = 0x0A
TOKEN_FLOAT32 = 0x0B
TOKEN_FLOAT64 = 0x0C
TOKEN_STRING = 0x0D
TOKEN_BINARY = 0x0E
TOKEN_ARRAY = 0x0F
TOKEN_OBJECT = 0x10
TOKEN_OBJECT_END = 0x11
TOKEN_ARRAY_END = 0x12

TOKEN_NAMES = {
    0x00: "null",
    0x01: "false",
    0x02: "true",
    0x03: "int8",
    0x04: "uint8",
    0x05: "int16",
    0x06: "uint16",
    0x07: "int32",
    0x08: "uint32",
    0x09: "int64",
    0x0A: "uint64",
    0x0B: "float32",
    0x0C: "float64",
    0x0D: "string",
    0x0E: "binary",
    0x0F: "array",
    0x10: "object",
    0x11: "object_end",
    0x12: "array_end",
}


class RtonReader:
    """Reads and parses RTON binary format."""

    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0
        self.strings: List[str] = []
        self._parse_header()

    def _parse_header(self):
        if self.data[:4] != RTON_MAGIC:
            raise ValueError(f"Not an RTON file (magic={self.data[:4]!r})")

        self.version = struct.unpack_from("<H", self.data, 4)[0]
        self.flags = struct.unpack_from("<H", self.data, 6)[0]
        str_offset = struct.unpack_from("<I", self.data, 8)[0]
        str_size = struct.unpack_from("<I", self.data, 0x0C)[0]
        root_offset = struct.unpack_from("<I", self.data, 0x10)[0]

        self.pos = 0x14  # Header is 20 bytes

        # Read string table
        if str_offset > 0 and str_size > 0:
            self._read_strings(str_offset, str_size)

        # Start parsing at root
        self.pos = root_offset

    def _read_strings(self, offset: int, size: int):
        """Read string table: packed null-terminated strings."""
        end = offset + size
        pos = offset
        while pos < end:
            null_idx = self.data.find(b"\x00", pos)
            if null_idx == -1 or null_idx >= end:
                s = self.data[pos:end].decode("utf-8", errors="replace")
                self.strings.append(s)
                break
            s = self.data[pos:null_idx].decode("utf-8", errors="replace")
            self.strings.append(s)
            pos = null_idx + 1

    def read_value(self) -> Any:
        """Read one RTON value starting at current position."""
        if self.pos >= len(self.data):
            return None

        token = self.data[self.pos]
        self.pos += 1

        if token == TOKEN_NULL:
            return None
        elif token == TOKEN_FALSE:
            return False
        elif token == TOKEN_TRUE:
            return True
        elif token == TOKEN_INT8:
            val = struct.unpack_from("<b", self.data, self.pos)[0]
            self.pos += 1
            return val
        elif token == TOKEN_UINT8:
            val = self.data[self.pos]
            self.pos += 1
            return val
        elif token == TOKEN_INT16:
            val = struct.unpack_from("<h", self.data, self.pos)[0]
            self.pos += 2
            return val
        elif token == TOKEN_UINT16:
            val = struct.unpack_from("<H", self.data, self.pos)[0]
            self.pos += 2
            return val
        elif token == TOKEN_INT32:
            val = struct.unpack_from("<i", self.data, self.pos)[0]
            self.pos += 4
            return val
        elif token == TOKEN_UINT32:
            val = struct.unpack_from("<I", self.data, self.pos)[0]
            self.pos += 4
            return val
        elif token == TOKEN_INT64:
            val = struct.unpack_from("<q", self.data, self.pos)[0]
            self.pos += 8
            return val
        elif token == TOKEN_UINT64:
            val = struct.unpack_from("<Q", self.data, self.pos)[0]
            self.pos += 8
            return val
        elif token == TOKEN_FLOAT32:
            val = struct.unpack_from("<f", self.data, self.pos)[0]
            self.pos += 4
            return val
        elif token == TOKEN_FLOAT64:
            val = struct.unpack_from("<d", self.data, self.pos)[0]
            self.pos += 8
            return val
        elif token == TOKEN_STRING:
            idx = struct.unpack_from("<I", self.data, self.pos)[0]
            self.pos += 4
            if 0 <= idx < len(self.strings):
                return self.strings[idx]
            return f"<string[{idx}]>"
        elif token == TOKEN_BINARY:
            size = struct.unpack_from("<I", self.data, self.pos)[0]
            self.pos += 4
            val = self.data[self.pos:self.pos + size]
            self.pos += size
            # Try to decode as string, otherwise return hex
            try:
                return val.decode("utf-8")
            except UnicodeDecodeError:
                return val.hex()
        elif token == TOKEN_ARRAY:
            return self._read_array()
        elif token == TOKEN_OBJECT:
            return self._read_object()
        elif token == TOKEN_OBJECT_END:
            return "<object_end>"
        elif token == TOKEN_ARRAY_END:
            return "<array_end>"
        else:
            print(f"WARN: Unknown token 0x{token:02x} at offset {self.pos - 1}",
                  file=sys.stderr)
            return f"<unknown:0x{token:02x}>"

    def _read_array(self) -> List[Any]:
        """Read an array of values."""
        arr = []
        while self.pos < len(self.data):
            # Peek at next token
            peek = self.data[self.pos]
            if peek == TOKEN_ARRAY_END:
                self.pos += 1
                break
            val = self.read_value()
            if val == "<object_end>" or val == "<array_end>":
                break
            arr.append(val)
        return arr

    def _read_object(self) -> Dict[str, Any]:
        """Read an object (key-value pairs)."""
        obj = {}
        while self.pos < len(self.data):
            peek = self.data[self.pos]
            if peek == TOKEN_OBJECT_END:
                self.pos += 1
                break
            # Keys are always strings
            key = self.read_value()
            if key == "<object_end>" or key == "<array_end>":
                break
            # Read value
            val = self.read_value()
            if val == "<object_end>" or val == "<array_end>":
                break
            if isinstance(key, str):
                obj[key] = val
            else:
                obj[str(key)] = val
        return obj

    def parse_root(self) -> Any:
        """Parse the root object/value."""
        self.pos = struct.unpack_from("<I", self.data, 0x10)[0]
        return self.read_value()


def format_value(val: Any, indent: int = 0) -> str:
    """Format a parsed RTON value for display (not JSON)."""
    prefix = "  " * indent
    if isinstance(val, dict):
        lines = ["{"]
        for k, v in val.items():
            lines.append(f"{prefix}  {json.dumps(k)}: {format_value(v, indent + 1)}")
        lines.append(f"{prefix}}}")
        return "\n".join(lines)
    elif isinstance(val, list):
        lines = ["["]
        for v in val:
            lines.append(f"{prefix}  {format_value(v, indent + 1)}")
        lines.append(f"{prefix}]")
        return "\n".join(lines)
    elif isinstance(val, str):
        return json.dumps(val)
    elif isinstance(val, bytes):
        return f"<binary:{len(val)} bytes>"
    else:
        return json.dumps(val)


def parse_args():
    parser = argparse.ArgumentParser(description="PopCap RTON parser")
    parser.add_argument("input", type=Path, help="RTON file or directory (with --dir)")
    parser.add_argument("output", type=Path, nargs="?", help="Output JSON file")
    parser.add_argument("--dir", action="store_true",
                        help="Treat input as directory, parse all .RTON files")
    parser.add_argument("--pretty", action="store_true",
                        help="Pretty-print output (not compact JSON)")
    parser.add_argument("--extract-all", type=Path, default=None,
                        help="Extract all .RTON files from a directory tree to JSON")
    return parser.parse_args()


def main():
    args = parse_args()

    if args.dir or args.extract_all:
        base = args.extract_all or args.input
        if not base.is_dir():
            print(f"error: {base} is not a directory", file=sys.stderr)
            sys.exit(1)

        output_base = args.output or base / "rton_json"
        output_base.mkdir(parents=True, exist_ok=True)

        rton_files = list(base.rglob("*.RTON")) + list(base.rglob("*.rton"))
        print(f"Found {len(rton_files)} RTON files in {base}/", file=sys.stderr)

        for rf in rton_files:
            rel = rf.relative_to(base)
            try:
                data = rf.read_bytes()
                reader = RtonReader(data)
                root = reader.parse_root()
                out_file = output_base / str(rel).replace(".RTON", ".json").replace(".rton", ".json")
                out_file.parent.mkdir(parents=True, exist_ok=True)
                with open(out_file, "w", encoding="utf-8") as f:
                    if args.pretty:
                        json.dump(root, f, indent=2, ensure_ascii=False)
                    else:
                        json.dump(root, f, ensure_ascii=False)
                print(f"  ✓ {rel} -> {out_file}", file=sys.stderr)
            except Exception as e:
                print(f"  ✗ {rel}: {e}", file=sys.stderr)

        print(f"\nDone. Output in {output_base}/", file=sys.stderr)
        return

    # Single file
    if not args.input.exists():
        print(f"error: {args.input} not found", file=sys.stderr)
        sys.exit(1)

    data = args.input.read_bytes()
    reader = RtonReader(data)

    root = reader.parse_root()

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            if args.pretty:
                json.dump(root, f, indent=2, ensure_ascii=False)
            else:
                json.dump(root, f, ensure_ascii=False)
        print(f"Written to {args.output}", file=sys.stderr)
    else:
        # Print to stdout
        if args.pretty:
            print(format_value(root))
        else:
            json.dump(root, sys.stdout, indent=2 if args.pretty else None,
                      ensure_ascii=False)
            print()


if __name__ == "__main__":
    main()
