//===- bolt/unittest/Core/Relocation.cpp ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Core/Relocation.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace bolt;

namespace {

/// Relocation's static helpers dispatch on Relocation::Arch, which
/// BinaryContext's constructor normally sets. Setting it directly keeps these
/// tests free of a target backend, so the relocation tables of every
/// architecture are checked wherever the tests are run.
struct RelocationTester : public testing::Test {
  void TearDown() override { Relocation::Arch = SavedArch; }

private:
  Triple::ArchType SavedArch = Relocation::Arch;
};

/// The width of a relocated field is the "Field" column of the relocation
/// table in the PowerPC64 ELFv2 ABI, Figure 4-1. Two consumers depend on it:
/// RewriteInstance::analyzeRelocation() reads exactly this many bytes out of
/// the input to recover the relocated value, and BinarySection::emitAsData()
/// steps over exactly this many bytes when it re-emits a data section. An
/// overlong size therefore both corrupts the extracted value and swallows the
/// bytes that follow the field.
TEST_F(RelocationTester, PPC64FieldWidths) {
  Relocation::Arch = Triple::ppc64le;

  // doubleword64
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_PPC64_ADDR64), 8u);
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_PPC64_REL64), 8u);
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_PPC64_TOC), 8u);

  // word32. R_PPC64_REL32 is the one .eh_frame uses for a
  // DW_EH_PE_pcrel|DW_EH_PE_sdata4 pointer; it was reported as 8 bytes.
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_PPC64_ADDR32), 4u);
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_PPC64_REL32), 4u);

  // low24 / low14, all encoded in one instruction word
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_PPC64_REL24), 4u);
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_PPC64_REL14), 4u);
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_PPC64_REL14_BRTAKEN), 4u);
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_PPC64_REL14_BRNTAKEN), 4u);

  // half16, every variant. These differ only in which slice of the value is
  // written into the instruction's 16-bit immediate field.
  for (uint32_t Type :
       {ELF::R_PPC64_ADDR16, ELF::R_PPC64_ADDR16_LO, ELF::R_PPC64_ADDR16_HI,
        ELF::R_PPC64_ADDR16_HA, ELF::R_PPC64_ADDR16_DS,
        ELF::R_PPC64_ADDR16_LO_DS, ELF::R_PPC64_ADDR16_HIGH,
        ELF::R_PPC64_ADDR16_HIGHA, ELF::R_PPC64_ADDR16_HIGHER,
        ELF::R_PPC64_ADDR16_HIGHERA, ELF::R_PPC64_ADDR16_HIGHEST,
        ELF::R_PPC64_ADDR16_HIGHESTA, ELF::R_PPC64_TOC16,
        ELF::R_PPC64_TOC16_LO, ELF::R_PPC64_TOC16_HI, ELF::R_PPC64_TOC16_HA,
        ELF::R_PPC64_TOC16_DS, ELF::R_PPC64_TOC16_LO_DS, ELF::R_PPC64_GOT16,
        ELF::R_PPC64_GOT16_LO, ELF::R_PPC64_GOT16_HI, ELF::R_PPC64_GOT16_HA,
        ELF::R_PPC64_DTPREL16, ELF::R_PPC64_DTPREL16_LO,
        ELF::R_PPC64_DTPREL16_HI, ELF::R_PPC64_DTPREL16_HA, ELF::R_PPC64_REL16,
        ELF::R_PPC64_REL16_LO, ELF::R_PPC64_REL16_HI, ELF::R_PPC64_REL16_HA})
    EXPECT_EQ(Relocation::getSizeForType(Type), 2u)
        << "for " << object::getELFRelocationTypeName(ELF::EM_PPC64, Type);

  EXPECT_EQ(Relocation::getSizeForType(ELF::R_PPC64_NONE), 0u);
}

/// A PC-relative relocation whose name carries a width must be that wide on
/// every architecture. The PPC64 table disagreed with the other two about
/// REL32/PREL32/PC32, which is what made the discrepancy worth pinning down.
TEST_F(RelocationTester, PCRelative32BitIsFourBytesEverywhere) {
  Relocation::Arch = Triple::x86_64;
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_X86_64_PC32), 4u);

  Relocation::Arch = Triple::aarch64;
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_AARCH64_PREL32), 4u);
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_AARCH64_PREL64), 8u);

  Relocation::Arch = Triple::ppc64le;
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_PPC64_REL32), 4u);
  EXPECT_EQ(Relocation::getSizeForType(ELF::R_PPC64_REL64), 8u);
}

/// Both PPC64 byte orders share one relocation table.
TEST_F(RelocationTester, PPC64BigAndLittleEndianAgree) {
  for (uint32_t Type :
       {ELF::R_PPC64_ADDR64, ELF::R_PPC64_ADDR32, ELF::R_PPC64_REL32,
        ELF::R_PPC64_REL24, ELF::R_PPC64_ADDR16_HA, ELF::R_PPC64_TOC}) {
    Relocation::Arch = Triple::ppc64le;
    const size_t LE = Relocation::getSizeForType(Type);
    Relocation::Arch = Triple::ppc64;
    EXPECT_EQ(Relocation::getSizeForType(Type), LE)
        << "for " << object::getELFRelocationTypeName(ELF::EM_PPC64, Type);
  }
}

/// isPCRelative() and getSizeForType() are consulted together by
/// analyzeRelocation(), which subtracts the relocation's own address from the
/// extracted value for a PC-relative type. Every PC-relative PPC64 type must
/// therefore have a size the value can actually be read out of.
TEST_F(RelocationTester, PPC64PCRelativeTypesHaveANonZeroSize) {
  Relocation::Arch = Triple::ppc64le;
  for (uint32_t Type :
       {ELF::R_PPC64_REL64, ELF::R_PPC64_REL32, ELF::R_PPC64_REL24,
        ELF::R_PPC64_REL14, ELF::R_PPC64_REL14_BRTAKEN,
        ELF::R_PPC64_REL14_BRNTAKEN, ELF::R_PPC64_REL16, ELF::R_PPC64_REL16_LO,
        ELF::R_PPC64_REL16_HI, ELF::R_PPC64_REL16_HA}) {
    EXPECT_TRUE(Relocation::isPCRelative(Type))
        << "for " << object::getELFRelocationTypeName(ELF::EM_PPC64, Type);
    const size_t Size = Relocation::getSizeForType(Type);
    EXPECT_GT(Size, 0u);
    EXPECT_LE(Size, 8u);
  }
}

} // namespace
