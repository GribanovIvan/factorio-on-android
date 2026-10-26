"""Minimal, strict AArch64 ELF64 reader/writer for the Factorio port.

Deliberately small: it only understands what the porting pipeline needs
(program headers, sections, symbols, dynamic tags, string tables) and never
rewrites data it was not asked to change. All patching happens on byte ranges
that the caller has located through symbol tables or program headers.
"""
from __future__ import annotations

import struct
from pathlib import Path

PT_NULL = 0
PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
PT_TLS = 7

SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_DYNAMIC = 6
SHT_NOBITS = 8
SHT_DYNSYM = 11

DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_SYMTAB = 6
DT_SONAME = 14
DT_DEBUG = 21
DT_FLAGS_1 = 0x6FFFFFFB
DT_VERSYM = 0x6FFFFFF0
DT_VERDEF = 0x6FFFFFFC
DT_VERDEFNUM = 0x6FFFFFFD
DT_VERNEED = 0x6FFFFFFE
DT_VERNEEDNUM = 0x6FFFFFFF

DF_1_PIE = 0x08000000
EM_AARCH64 = 183
ET_DYN = 3


class ELFError(Exception):
    pass


def _cstr(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ELFError("unterminated string")
    return data[offset:end].decode("utf-8", "replace")


class ELF:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.data = bytearray(self.path.read_bytes())
        if self.data[:4] != b"\x7fELF" or self.data[4] != 2 or self.data[5] != 1:
            raise ELFError("expected little-endian ELF64")
        (self.e_type, self.e_machine, self.e_version, self.e_entry, self.e_phoff,
         self.e_shoff, self.e_flags, self.e_ehsize, self.e_phentsize,
         self.e_phnum, self.e_shentsize, self.e_shnum,
         self.e_shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", self.data, 16)
        if self.e_machine != EM_AARCH64:
            raise ELFError(f"expected AArch64, got e_machine={self.e_machine}")
        self.phdrs = []
        for i in range(self.e_phnum):
            off = self.e_phoff + i * 56
            (p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
             p_align) = struct.unpack_from("<IIQQQQQQ", self.data, off)
            self.phdrs.append(dict(index=i, header_offset=off, type=p_type,
                                   flags=p_flags, offset=p_offset, vaddr=p_vaddr,
                                   paddr=p_paddr, filesz=p_filesz, memsz=p_memsz,
                                   align=p_align))
        self.shdrs = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * 64
            (name, sh_type, flags, addr, offset, size, link, info, addralign,
             entsize) = struct.unpack_from("<IIQQQQIIQQ", self.data, off)
            self.shdrs.append(dict(index=i, header_offset=off, name_offset=name,
                                   type=sh_type, flags=flags, addr=addr,
                                   offset=offset, size=size, link=link, info=info,
                                   align=addralign, entsize=entsize))
        self._name_sections()

    # ------------------------------------------------------------------ basics
    def _name_sections(self) -> None:
        if not self.e_shnum:
            return
        strings = self.shdrs[self.e_shstrndx]
        base = strings["offset"]
        for section in self.shdrs:
            section["name"] = _cstr(self.data, base + section["name_offset"])

    def section(self, name: str) -> dict:
        for section in self.shdrs:
            if section["name"] == name:
                return section
        raise ELFError(f"missing section {name}")

    def sections(self, sh_type: int) -> list[dict]:
        return [s for s in self.shdrs if s["type"] == sh_type]

    def segment(self, p_type: int) -> list[dict]:
        return [p for p in self.phdrs if p["type"] == p_type]

    def vaddr_to_offset(self, vaddr: int) -> int:
        for segment in self.segment(PT_LOAD):
            start = segment["vaddr"]
            if start <= vaddr < start + segment["filesz"]:
                return segment["offset"] + (vaddr - start)
        raise ELFError(f"vaddr 0x{vaddr:x} is not file-backed")

    def offset_to_vaddr(self, offset: int) -> int:
        for segment in self.segment(PT_LOAD):
            start = segment["offset"]
            if start <= offset < start + segment["filesz"]:
                return segment["vaddr"] + (offset - start)
        raise ELFError(f"offset 0x{offset:x} is not mapped")

    # ---------------------------------------------------------------- symbols
    def symbols(self, sh_type: int = SHT_SYMTAB) -> list[dict]:
        result = []
        for section in self.sections(sh_type):
            strings = self.shdrs[section["link"]]
            str_base = strings["offset"]
            count = section["size"] // 24
            for i in range(count):
                off = section["offset"] + i * 24
                name, info, other, shndx, value, size = struct.unpack_from(
                    "<IBBHQQ", self.data, off)
                if not name:
                    continue
                result.append(dict(
                    name=_cstr(self.data, str_base + name), info=info,
                    bind=info >> 4, type=info & 0xF, other=other,
                    shndx=shndx, value=value, size=size,
                    table_index=i))
        return result

    def defined_function_symbols(self) -> list[dict]:
        out = []
        seen = set()
        for symbol in self.symbols(SHT_SYMTAB):
            if symbol["shndx"] in (0, 0xFFF1) or symbol["type"] != 2:
                continue
            if symbol["value"] in seen:
                continue
            seen.add(symbol["value"])
            out.append(symbol)
        return out

    # --------------------------------------------------------------- dynamic
    def dynamic(self) -> list[dict]:
        entries = []
        for segment in self.segment(PT_DYNAMIC):
            for i in range(segment["filesz"] // 16):
                off = segment["offset"] + i * 16
                tag, value = struct.unpack_from("<qQ", self.data, off)
                if tag == DT_NULL:
                    break
                entries.append(dict(tag=tag, value=value, offset=off))
        return entries

    def dynamic_strtab_offset(self) -> int:
        for entry in self.dynamic():
            if entry["tag"] == DT_STRTAB:
                return self.vaddr_to_offset(entry["value"])
        raise ELFError("no DT_STRTAB")

    def dynamic_string(self, offset: int) -> str:
        """Resolve an offset into the dynamic string table (DT_NEEDED/SONAME)."""
        return _cstr(self.data, self.dynamic_strtab_offset() + offset)

    def dynamic_segment(self) -> dict:
        segments = self.segment(PT_DYNAMIC)
        if len(segments) != 1:
            raise ELFError("expected exactly one PT_DYNAMIC")
        return segments[0]

    def write(self, path: str | Path, mode: int = 0o755) -> None:
        destination = Path(path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(self.data)
        destination.chmod(mode)

    # ------------------------------------------------------------- primitives
    def u32(self, offset: int) -> int:
        return struct.unpack_from("<I", self.data, offset)[0]

    def put_u32(self, offset: int, value: int) -> None:
        struct.pack_into("<I", self.data, offset, value & 0xFFFFFFFF)

    def put_bytes(self, offset: int, payload: bytes) -> None:
        self.data[offset:offset + len(payload)] = payload
