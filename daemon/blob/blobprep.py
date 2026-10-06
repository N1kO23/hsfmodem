#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare a Conexant HSF blob (.O) for linking into a userspace executable.

The blobs were built as kernel-module objects, so a few sections need new
names before several of them can share one ordinary userspace link:

  * .ctors           -> .hsf.<blob>.ctors
        HsfEngineInit walks this table itself (it has relocations against
        .ctors+0 and +8) until it reads a NULL entry; in the kernel that zero
        came from alignment padding. Under its original name, ld would fold
        the table into .init_array and glibc would call the bogus first
        entry (value 1). The new name matches no default linker rule, so the
        link places it explicitly, followed by a zero word (hsf-ctors.ld).
  * .gnu.linkonce.*  -> .text|.data|.rodata.<blob>.lo.*  (chosen by flags)
        The engine and hda blobs reuse the same obfuscated linkonce names, and
        ld keeps only one section per linkonce name across all inputs.
  * executable .note.GNU-stack -> non-executable

Afterwards the script checks that the symbol table and every relocation are
unchanged apart from those renames. Only ELF metadata (headers, symbol and
relocation tables) is interpreted; section contents are left untouched.
"""

import argparse
import collections
import hashlib
import struct
import subprocess
import sys

SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 0x1, 0x2, 0x4
SHT_SYMTAB, SHT_RELA, SHT_REL = 2, 4, 9
STT_SECTION = 3
SHN_LORESERVE, SHN_XINDEX = 0xFF00, 0xFFFF


class Section:
    __slots__ = ("index", "name", "type", "flags", "offset", "size", "link", "info", "entsize")


class Elf:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF":
            raise ValueError(f"{path}: not an ELF file")
        if d[5] != 1:
            raise ValueError(f"{path}: only little-endian ELF is supported")
        self.is64 = d[4] == 2
        hdr_fmt = "<HHIQQQIHHHHHH" if self.is64 else "<HHIIIIIHHHHHH"
        hdr = struct.unpack_from(hdr_fmt, d, 16)
        self.e_type, self.e_machine = hdr[0], hdr[1]
        shoff, shentsize, shnum, shstrndx = hdr[5], hdr[10], hdr[11], hdr[12]
        sh_fmt = "<IIQQQQIIQQ" if self.is64 else "<IIIIIIIIII"
        if shnum == 0:  # extended section numbering
            shnum = struct.unpack_from(sh_fmt, d, shoff)[5]
        raw = [struct.unpack_from(sh_fmt, d, shoff + i * shentsize) for i in range(shnum)]
        if shstrndx == SHN_XINDEX:
            shstrndx = raw[0][6]
        names_off = raw[shstrndx][4]
        self.sections = []
        for i, (name, typ, flags, _addr, off, size, link, info, _align, entsize) in enumerate(raw):
            s = Section()
            s.index, s.name, s.type, s.flags = i, self._cstr(names_off + name), typ, flags
            s.offset, s.size, s.link, s.info, s.entsize = off, size, link, info, entsize
            self.sections.append(s)

    def _cstr(self, off):
        return self.data[off:self.data.index(b"\0", off)].decode()

    def symbols(self):
        """Return [(name, value, size, type, bind, shndx)] from .symtab."""
        for s in self.sections:
            if s.type != SHT_SYMTAB:
                continue
            strtab = self.sections[s.link]
            out = []
            for i in range(s.size // s.entsize):
                o = s.offset + i * s.entsize
                if self.is64:
                    name, info, _other, shndx, value, size = struct.unpack_from("<IBBHQQ", self.data, o)
                else:
                    name, value, size, info, _other, shndx = struct.unpack_from("<IIIBBH", self.data, o)
                out.append((self._cstr(strtab.offset + name), value, size, info & 0xF, info >> 4, shndx))
            return out
        return []

    def relocations(self):
        """Yield (target_section_index, offset, type, symbol_index, addend)."""
        for s in self.sections:
            if s.type not in (SHT_RELA, SHT_REL):
                continue
            for i in range(s.size // s.entsize):
                o = s.offset + i * s.entsize
                addend = None
                if self.is64:
                    if s.type == SHT_RELA:
                        off, info, addend = struct.unpack_from("<QQq", self.data, o)
                    else:
                        off, info = struct.unpack_from("<QQ", self.data, o)
                    typ, sym = info & 0xFFFFFFFF, info >> 32
                else:
                    if s.type == SHT_RELA:
                        off, info, addend = struct.unpack_from("<IIi", self.data, o)
                    else:
                        off, info = struct.unpack_from("<II", self.data, o)
                    typ, sym = info & 0xFF, info >> 8
                yield s.info, off, typ, sym, addend


def plan_renames(elf, blob):
    counts = collections.Counter(s.name for s in elf.sections)
    renames = {}
    for s in elf.sections:
        n = s.name
        if n == ".ctors":
            renames[n] = f".hsf.{blob}.ctors"
        elif n.startswith(".gnu.linkonce."):
            if counts[n] != 1:
                raise ValueError(f"duplicate section name {n}")
            if not s.flags & SHF_ALLOC:
                raise ValueError(f"non-allocated linkonce section {n}")
            if s.flags & SHF_EXECINSTR:
                kind = "text"
            elif s.flags & SHF_WRITE:
                kind = "data"
            else:
                kind = "rodata"
            renames[n] = f".{kind}.{blob}.lo.{n[len('.gnu.linkonce.'):]}"
    return renames


def exec_stack_note(elf):
    return any(s.name == ".note.GNU-stack" and s.flags & SHF_EXECINSTR for s in elf.sections)


def section_key(elf, renames, shndx):
    if 0 < shndx < SHN_LORESERVE:
        name = elf.sections[shndx].name
        return ("sec", renames.get(name, name))
    return ("special", shndx)


def symbol_set(elf, renames):
    out = collections.Counter()
    for name, value, size, typ, bind, shndx in elf.symbols():
        if typ == STT_SECTION:
            # GCC 3.2's assembler emitted section symbols even for .rela.*
            # sections; objcopy regenerates those as SHN_ABS. Nothing refers
            # to them, so only section symbols of allocated sections matter.
            if not 0 < shndx < SHN_LORESERVE or not elf.sections[shndx].flags & SHF_ALLOC:
                continue
        sec = section_key(elf, renames, shndx)
        out[(None if typ == STT_SECTION else name, value, size, typ, bind, sec)] += 1
    return out


def reloc_set(elf, renames):
    syms = elf.symbols()
    out = collections.Counter()
    for target, off, typ, sym, addend in elf.relocations():
        tname = elf.sections[target].name
        sname, _v, _s, styp, _b, sshndx = syms[sym]
        skey = section_key(elf, renames, sshndx) if styp == STT_SECTION else ("sym", sname)
        out[(renames.get(tname, tname), off, typ, skey, addend)] += 1
    return out


def alloc_sections(elf, renames):
    mask = SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR
    return collections.Counter(
        (renames.get(s.name, s.name), s.size, s.flags & mask)
        for s in elf.sections if s.flags & SHF_ALLOC)


def verify(orig, new, renames):
    problems = []
    if alloc_sections(orig, renames) != alloc_sections(new, {}):
        problems.append("allocated sections differ (name/size/flags)")
    if symbol_set(orig, renames) != symbol_set(new, {}):
        problems.append("symbol tables differ")
    if reloc_set(orig, renames) != reloc_set(new, {}):
        problems.append("relocations differ")
    leftovers = [s.name for s in new.sections if s.name == ".ctors" or s.name.startswith(".gnu.linkonce.")]
    if leftovers:
        problems.append(f"sections not renamed: {leftovers[:5]}")
    if exec_stack_note(new):
        problems.append(".note.GNU-stack is still executable")
    return problems


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def check_manifest(path, manifest):
    want = {}
    with open(manifest) as f:
        for line in f:
            if line.strip():
                digest, name = line.split()
                want[name.lstrip("*")] = digest
    name = path.rsplit("/", 1)[-1]
    if name not in want:
        raise SystemExit(f"{name}: not listed in {manifest}")
    if want[name] != sha256(path):
        raise SystemExit(f"{name}: SHA256 does not match {manifest}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--blob", required=True, help="short unique name used in new section names")
    ap.add_argument("--objcopy", default="objcopy")
    ap.add_argument("--manifest", help="SHA256SUMS file the input must match")
    args = ap.parse_args()

    if args.manifest:
        check_manifest(args.input, args.manifest)

    orig = Elf(args.input)
    renames = plan_renames(orig, args.blob)
    run_renames(args, orig, renames, args.input)

    new = Elf(args.output)
    problems = verify(orig, new, renames)
    if problems:
        for p in problems:
            print(f"{args.input}: {p}", file=sys.stderr)
        sys.exit(1)
    nrel = sum(1 for _ in new.relocations())
    print(f"{args.blob}: {len(renames)} sections renamed, {nrel} relocations verified")


def run_renames(args, orig, renames, src):
    cmd = [args.objcopy]
    for old, new in renames.items():
        cmd += ["--rename-section", f"{old}={new}"]
    if exec_stack_note(orig):
        cmd += ["--set-section-flags", ".note.GNU-stack=contents,readonly"]
    cmd += [src, args.output]
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
