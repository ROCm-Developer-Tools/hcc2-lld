//===- GdbIndex.cpp -------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The -gdb-index option instructs the linker to emit a .gdb_index section.
// The section contains information to make gdb startup faster.
// The format of the section is described at
// https://sourceware.org/gdb/onlinedocs/gdb/Index-Section-Format.html.
//
//===----------------------------------------------------------------------===//

#include "GdbIndex.h"
#include "Memory.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugPubTable.h"
#include "llvm/Object/ELFObjectFile.h"

using namespace llvm;
using namespace llvm::object;
using namespace lld;
using namespace lld::elf;

std::pair<bool, GdbSymbol *> GdbHashTab::add(uint32_t Hash, size_t Offset) {
  GdbSymbol *&Sym = Map[Offset];
  if (Sym)
    return {false, Sym};
  Sym = make<GdbSymbol>(Hash, Offset);
  return {true, Sym};
}

void GdbHashTab::finalizeContents() {
  uint32_t Size = std::max<uint32_t>(1024, NextPowerOf2(Map.size() * 4 / 3));
  uint32_t Mask = Size - 1;
  Table.resize(Size);

  for (auto &P : Map) {
    GdbSymbol *Sym = P.second;
    uint32_t I = Sym->NameHash & Mask;
    uint32_t Step = ((Sym->NameHash * 17) & Mask) | 1;

    while (Table[I])
      I = (I + Step) & Mask;
    Table[I] = Sym;
  }
}

template <class ELFT>
LLDDwarfObj<ELFT>::LLDDwarfObj(ObjFile<ELFT> *Obj) : Obj(Obj) {
  for (InputSectionBase *Sec : Obj->getSections()) {
    if (!Sec)
      continue;
    if (LLDDWARFSection *M = StringSwitch<LLDDWARFSection *>(Sec->Name)
                                 .Case(".debug_info", &InfoSection)
                                 .Case(".debug_ranges", &RangeSection)
                                 .Case(".debug_line", &LineSection)
                                 .Default(nullptr)) {
      M->Data = toStringRef(Sec->Data);
      M->Sec = Sec;
      continue;
    }
    if (Sec->Name == ".debug_abbrev")
      AbbrevSection = toStringRef(Sec->Data);
    else if (Sec->Name == ".debug_gnu_pubnames")
      GnuPubNamesSection = toStringRef(Sec->Data);
    else if (Sec->Name == ".debug_gnu_pubtypes")
      GnuPubTypesSection = toStringRef(Sec->Data);
  }
}

// Find if there is a relocation at Pos in Sec.  The code is a bit
// more complicated than usual because we need to pass a section index
// to llvm since it has no idea about InputSection.
template <class ELFT>
template <class RelTy>
Optional<RelocAddrEntry>
LLDDwarfObj<ELFT>::findAux(const InputSectionBase &Sec, uint64_t Pos,
                           ArrayRef<RelTy> Rels) const {
  auto I = llvm::find_if(Rels,
                         [=](const RelTy &Rel) { return Rel.r_offset == Pos; });
  if (I == Rels.end())
    return None;
  const RelTy &Rel = *I;
  const ObjFile<ELFT> *File = Sec.getFile<ELFT>();
  uint32_t SymIndex = Rel.getSymbol(Config->IsMips64EL);
  const typename ELFT::Sym &Sym = File->getELFSymbols()[SymIndex];
  uint32_t SecIndex = File->getSectionIndex(Sym);
  SymbolBody &B = File->getRelocTargetSym(Rel);
  auto &DR = cast<DefinedRegular>(B);
  uint64_t Val = DR.Value + getAddend<ELFT>(Rel);

  // FIXME: We should be consistent about always adding the file
  // offset or not.
  if (DR.Section->Flags & ELF::SHF_ALLOC)
    Val += cast<InputSection>(DR.Section)->getOffsetInFile();

  RelocAddrEntry Ret{SecIndex, Val};
  return Ret;
}

template <class ELFT>
Optional<RelocAddrEntry> LLDDwarfObj<ELFT>::find(const llvm::DWARFSection &S,
                                                 uint64_t Pos) const {
  auto &Sec = static_cast<const LLDDWARFSection &>(S);
  if (Sec.Sec->AreRelocsRela)
    return findAux(*Sec.Sec, Pos, Sec.Sec->template relas<ELFT>());
  return findAux(*Sec.Sec, Pos, Sec.Sec->template rels<ELFT>());
}

template class elf::LLDDwarfObj<ELF32LE>;
template class elf::LLDDwarfObj<ELF32BE>;
template class elf::LLDDwarfObj<ELF64LE>;
template class elf::LLDDwarfObj<ELF64BE>;
