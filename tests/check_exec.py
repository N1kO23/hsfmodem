#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail unless the given ELF file is a non-PIE executable (e_type == ET_EXEC).

The blobs use 32-bit absolute relocations, so anything they are linked into
must be placed at a fixed low address.
"""
import struct
import sys

ET_EXEC = 2

with open(sys.argv[1], "rb") as f:
    head = f.read(18)
if head[:4] != b"\x7fELF":
    sys.exit(f"{sys.argv[1]}: not an ELF file")
e_type = struct.unpack_from("<H", head, 16)[0]
if e_type != ET_EXEC:
    sys.exit(f"{sys.argv[1]}: ELF type {e_type}, expected {ET_EXEC} (ET_EXEC, non-PIE)")
print(f"{sys.argv[1]}: ET_EXEC")
