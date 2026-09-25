//===- bolt/unittest/Target/PowerPC/PPCMCPlusBuilderTest.cpp
//-------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Target/PowerPC/PPCMCPlusBuilder.h"
#include "MCTargetDesc/PPCFixupKinds.h"
#include "MCTargetDesc/PPCMCAsmInfo.h"
#include "MCTargetDesc/PPCMCTargetDesc.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/MCPlusBuilder.h"
#include "bolt/Core/Relocation.h"
#include "bolt/Rewrite/RewriteInstance.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCSymbolELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/TargetSelect.h"
#include "gtest/gtest.h"

// glibc's <elf.h> defines the whole R_PPC64_* relocation list as object-like
// macros, and on a ppc64le host it reaches this translation unit through the
// system headers pulled in above -- turning `ELF::R_PPC64_ADDR16` into `ELF::3`
// and failing the build with "expected unqualified-id before numeric constant".
// Drop the ones named below so the llvm::ELF enumerators are visible.
#undef R_PPC64_REL14
#undef R_PPC64_REL24
#undef R_PPC64_ADDR16
#undef R_PPC64_ADDR16_DS
#undef R_PPC64_ADDR16_HA
#undef R_PPC64_ADDR16_HI
#undef R_PPC64_ADDR16_HIGH
#undef R_PPC64_ADDR16_HIGHA
#undef R_PPC64_ADDR16_HIGHER
#undef R_PPC64_ADDR16_HIGHERA
#undef R_PPC64_ADDR16_HIGHEST
#undef R_PPC64_ADDR16_HIGHESTA
#undef R_PPC64_ADDR16_LO
#undef R_PPC64_ADDR16_LO_DS

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;
using namespace bolt;

namespace {

TEST(PPCMCPlusBuilderTest, CreatePushRegisters) {

  MCInst Inst1, Inst2;
  MCPhysReg Reg1 = PPC::R3;

  PPCMCPlusBuilder::createPushRegisters(Inst1, Inst2, Reg1, /*Reg2=*/PPC::R4);

  // Check Inst is ORI R0, R0, 0
  auto ExpectNop = [](const MCInst &Inst) {
    EXPECT_EQ(Inst.getOpcode(), PPC::ORI);
    ASSERT_EQ(Inst.getNumOperands(), 3u);

    ASSERT_TRUE(Inst.getOperand(0).isReg());
    ASSERT_TRUE(Inst.getOperand(1).isReg());
    ASSERT_TRUE(Inst.getOperand(2).isImm());

    EXPECT_EQ(Inst.getOperand(0).getReg(), PPC::R0);
    EXPECT_EQ(Inst.getOperand(1).getReg(), PPC::R0);
    EXPECT_EQ(Inst.getOperand(2).getImm(), 0);
  };
  ExpectNop(Inst1);
  ExpectNop(Inst2);
}

// Fixture that stands up a minimal PPC64 BinaryContext (mirroring
// bolt/unittests/Core/MCPlusBuilder.cpp's MCPlusBuilderTester, but fixed to
// PowerPC64 since these tests are PPC-specific rather than parameterized
// across targets). This gives real, target-initialized PPCMCPlusBuilder
// (MRI/MII/STI all populated) rather than hand-rolling those pieces.
class PPCMCPlusBuilderFixture : public testing::Test {
public:
  void SetUp() override {
    initalizeLLVM();
    prepareElf();
    initializeBolt();
  }

protected:
  void initalizeLLVM() {
#define BOLT_TARGET(target)                                                  \
  LLVMInitialize##target##TargetInfo();                                      \
  LLVMInitialize##target##TargetMC();                                        \
  LLVMInitialize##target##AsmParser();                                       \
  LLVMInitialize##target##Disassembler();                                    \
  LLVMInitialize##target##Target();                                          \
  LLVMInitialize##target##AsmPrinter();

#include "bolt/Core/TargetConfig.def"
  }

  void prepareElf() {
    memcpy(ElfBuf, "\177ELF", 4);
    ELF64LE::Ehdr *EHdr = reinterpret_cast<ELF64LE::Ehdr *>(ElfBuf);
    EHdr->e_ident[llvm::ELF::EI_CLASS] = llvm::ELF::ELFCLASS64;
    EHdr->e_ident[llvm::ELF::EI_DATA] = llvm::ELF::ELFDATA2LSB;
    EHdr->e_machine = EM_PPC64;
    MemoryBufferRef Source(StringRef(ElfBuf, sizeof(ElfBuf)), "ELF");
    ObjFile = cantFail(ObjectFile::createObjectFile(Source));
  }

  void initializeBolt() {
    Relocation::Arch = ObjFile->makeTriple().getArch();
    BC = cantFail(BinaryContext::createBinaryContext(
        ObjFile->makeTriple(), std::make_shared<orc::SymbolStringPool>(),
        ObjFile->getFileName(), nullptr, true, DWARFContext::create(*ObjFile),
        {llvm::outs(), llvm::errs()}));
    ASSERT_FALSE(!BC);
    BC->initializeTarget(std::unique_ptr<MCPlusBuilder>(
        createMCPlusBuilder(Triple::ppc64le, BC->MIA.get(), BC->MII.get(),
                            BC->MRI.get(), BC->STI.get())));
  }

  // Build an MCInst representing an unconditional branch (b <target>) to a
  // freshly-created resolvable symbol.
  MCInst makeUncondBranch(StringRef TargetName = "target_sym") {
    MCInst I;
    I.setOpcode(PPC::B);
    MCSymbol *Sym = BC->Ctx->getOrCreateSymbol(TargetName);
    I.addOperand(MCOperand::createExpr(MCSymbolRefExpr::create(Sym, *BC->Ctx)));
    return I;
  }

  // Build an MCInst representing a conditional branch (bc BO, BI, <target>)
  // to a freshly-created resolvable symbol.
  MCInst makeCondBranch(StringRef TargetName = "target_sym") {
    MCInst I;
    I.setOpcode(PPC::BC);
    // BC operands: BO (branch options), BI (CR bit), BD (target)
    // BO=12: branch if CR bit is set (true)
    I.addOperand(MCOperand::createImm(12));
    I.addOperand(MCOperand::createReg(PPC::CR0LT));
    MCSymbol *Sym = BC->Ctx->getOrCreateSymbol(TargetName);
    I.addOperand(MCOperand::createExpr(MCSymbolRefExpr::create(Sym, *BC->Ctx)));
    return I;
  }

  MCInst makeReturn() {
    MCInst I;
    I.setOpcode(PPC::BLR);
    return I;
  }

  MCInst makeIndirectBranch() {
    MCInst I;
    I.setOpcode(PPC::BCTR);
    return I;
  }

  // A call (bl <target>) followed conceptually by nothing else in the block
  // -- i.e. the block's last instruction is not a branch terminator at all.
  MCInst makeCall(StringRef TargetName = "callee") {
    MCInst I;
    I.setOpcode(PPC::BL8);
    MCSymbol *Sym = BC->Ctx->getOrCreateSymbol(TargetName);
    I.addOperand(MCOperand::createExpr(MCSymbolRefExpr::create(Sym, *BC->Ctx)));
    return I;
  }

  // A symbol reference, optionally carrying a PPC relocation specifier
  // (@l, @ha, @highest, ...). This is the shape createLongTailCall() and
  // createLongJmp() build their immediates from.
  const MCExpr *symExpr(StringRef Name, uint16_t Spec = PPC::S_None) {
    MCSymbol *Sym = BC->Ctx->getOrCreateSymbol(Name);
    return MCSymbolRefExpr::create(Sym, Spec, *BC->Ctx);
  }

  // Map a fixup of the given PPC fixup kind, carrying \p Value, the way
  // BinaryFunction::scanExternalRefs() does after re-encoding an instruction.
  // PPCFixupKinds.h numbers its enumerators from FirstTargetFixupKind, so they
  // are already MCFixupKind values.
  std::optional<Relocation> relocForFixup(MCFixupKind Kind,
                                          const MCExpr *Value) {
    MCFixup F = MCFixup::create(/*Offset=*/0, Value, Kind);
    return BC->MIB->createRelocation(F, *BC->MAB);
  }

  char ElfBuf[sizeof(ELF64LE::Ehdr)] = {};
  std::unique_ptr<ObjectFile> ObjFile;
  std::unique_ptr<BinaryContext> BC;
};

#ifdef POWERPC_AVAILABLE

// --- getPCRelEncodingSize regression tests (bug fix 285d3a8bcaa5) ---
//
// BDNZ/BDNZL ("decrement CTR and branch if not zero", used for loop
// backedges) share BC/BCL's 14-bit BD displacement field (+-32KB), NOT the
// 26-bit LI field used by B/BL. Prior to the fix, getPCRelEncodingSize()
// misclassified them as 26-bit, which made LongJmpPass::needsStub()
// consider far-away BDNZ targets (e.g. across a hot/cold split) as in
// range when they were not, causing JITLink to later reject the Delta14
// fixup as out of range.

TEST_F(PPCMCPlusBuilderFixture, PCRelEncodingSize_BDNZ_Is16Bit) {
  MCInst BDNZ;
  BDNZ.setOpcode(PPC::BDNZ);
  EXPECT_EQ(BC->MIB->getPCRelEncodingSize(BDNZ), 16);
}

TEST_F(PPCMCPlusBuilderFixture, PCRelEncodingSize_BDNZL_Is16Bit) {
  MCInst BDNZL;
  BDNZL.setOpcode(PPC::BDNZL);
  EXPECT_EQ(BC->MIB->getPCRelEncodingSize(BDNZL), 16);
}

TEST_F(PPCMCPlusBuilderFixture, PCRelEncodingSize_BC_Family_Is16Bit) {
  for (unsigned Opc : {PPC::BC, PPC::gBC, PPC::BCL, PPC::gBCL, PPC::BCC,
                      PPC::BCCA, PPC::BCCL, PPC::BCCLA}) {
    MCInst I;
    I.setOpcode(Opc);
    EXPECT_EQ(BC->MIB->getPCRelEncodingSize(I), 16)
        << "opcode " << Opc << " should be classified as 16-bit PC-relative";
  }
}

TEST_F(PPCMCPlusBuilderFixture, PCRelEncodingSize_B_Family_Is26Bit) {
  for (unsigned Opc :
       {PPC::B, PPC::BL, PPC::BL8, PPC::BL8_NOP, PPC::BL8_NOTOC}) {
    MCInst I;
    I.setOpcode(Opc);
    EXPECT_EQ(BC->MIB->getPCRelEncodingSize(I), 26)
        << "opcode " << Opc << " should be classified as 26-bit PC-relative";
  }
}

// --- BDZ family regression tests (this fix) ---
//
// BDZ/BDZL ("decrement CTR and branch if ZERO") is the opposite-polarity
// sibling of BDNZ/BDNZL ("...branch if NOT zero"), fixed for isBranch()/
// isConditionalBranch() in commit 3c60598a2f9b. That fix only covered
// BDNZ/BDNZL -- BDZ/BDZL (and the rest of the BForm_1 family: the absolute
// "*a", link "*l", and branch-hint "+"/"-" variants, plus the BDNZ8/BDZ8
// 64-bit pseudo forms) were left completely unclassified by isBranch(),
// isConditionalBranch(), getPCRelOperandNum()/evaluateBranch(), and
// getPCRelEncodingSize(). A block ending in bdz was therefore invisible to
// BinaryFunction::buildCFG()'s fallthrough-successor-edge logic, so its real
// (fallthrough) successor block looked unreachable and was deleted by
// eliminate-unreachable-blocks -- observed as a deterministic SIGSEGV in
// llvm::StringRef::find_last_not_of (an unrolled bdz/bdnz-terminated
// bitset-scan loop) when BOLT-rewriting a real ppc64le binary (llc) with
// zero optimization flags.
TEST_F(PPCMCPlusBuilderFixture, IsBranch_BDZ_Family) {
  for (unsigned Opc :
       {PPC::BDZ, PPC::BDZL, PPC::BDZp, PPC::BDZLp, PPC::BDZm, PPC::BDZLm,
        PPC::BDNZ, PPC::BDNZL, PPC::BDNZp, PPC::BDNZLp, PPC::BDNZm,
        PPC::BDNZLm}) {
    MCInst I;
    I.setOpcode(Opc);
    EXPECT_TRUE(BC->MIB->isBranch(I))
        << "opcode " << Opc << " should be classified as a branch";
  }
}

TEST_F(PPCMCPlusBuilderFixture, IsConditionalBranch_BDZ_Family) {
  // This is the crux of the regression: before the fix, isConditionalBranch
  // returned false for BDZ/BDZL, causing BinaryFunction::buildCFG()'s
  // "IsPrevFT = MIB->isConditionalBranch(*LastInstr)" fallthrough-edge logic
  // (BinaryFunction.cpp's addSuccessor loop, ~line 2619) to treat a
  // bdz-terminated block as having no fallthrough successor at all.
  for (unsigned Opc :
       {PPC::BDZ, PPC::BDZL, PPC::BDZp, PPC::BDZLp, PPC::BDZm, PPC::BDZLm,
        PPC::BDNZ, PPC::BDNZL, PPC::BDNZp, PPC::BDNZLp, PPC::BDNZm,
        PPC::BDNZLm}) {
    MCInst I;
    I.setOpcode(Opc);
    EXPECT_TRUE(BC->MIB->isConditionalBranch(I))
        << "opcode " << Opc << " should be classified as a conditional branch";
  }
}

TEST_F(PPCMCPlusBuilderFixture, PCRelEncodingSize_BDZ_Family_Is16Bit) {
  for (unsigned Opc :
       {PPC::BDZ, PPC::BDZL, PPC::BDZp, PPC::BDZLp, PPC::BDZm, PPC::BDZLm,
        PPC::BDZ8, PPC::BDNZ8}) {
    MCInst I;
    I.setOpcode(Opc);
    EXPECT_EQ(BC->MIB->getPCRelEncodingSize(I), 16)
        << "opcode " << Opc << " should be classified as 16-bit PC-relative";
  }
}

TEST_F(PPCMCPlusBuilderFixture, IsReversibleBranch_BDZ_Family_IsFalse) {
  // BDZ's opposite-sense branch is the genuinely different instruction
  // BDNZ (and vice versa), not a flippable operand -- same rationale as the
  // existing BDNZ isReversibleBranch handling.
  for (unsigned Opc : {PPC::BDZ, PPC::BDZL, PPC::BDNZ, PPC::BDNZL}) {
    MCInst I;
    I.setOpcode(Opc);
    EXPECT_FALSE(BC->MIB->isReversibleBranch(I))
        << "opcode " << Opc << " should not be reversible";
  }
}

TEST_F(PPCMCPlusBuilderFixture, AnalyzeBranch_BDZ_IsConditionalWithFallthrough) {
  // A block ending in bdz to a resolvable target should be analyzed the
  // same way as a block ending in bdnz: a conditional branch with an
  // assumed fallthrough successor (computed precisely later once the CFG
  // is built), NOT an unanalyzable terminator.
  MCInst BDZ;
  BDZ.setOpcode(PPC::BDZ);
  MCSymbol *Sym = BC->Ctx->getOrCreateSymbol("bdz_target");
  BDZ.addOperand(MCOperand::createExpr(MCSymbolRefExpr::create(Sym, *BC->Ctx)));

  InstructionListType Insts{BDZ};
  const MCSymbol *Tgt = nullptr, *Fallthrough = nullptr;
  MCInst *CondBr = nullptr, *UncondBr = nullptr;

  bool Result = BC->MIB->analyzeBranch(Insts.begin(), Insts.end(), Tgt,
                                       Fallthrough, CondBr, UncondBr);

  EXPECT_TRUE(Result);
  EXPECT_NE(Tgt, nullptr);
  EXPECT_EQ(Tgt, BC->Ctx->lookupSymbol("bdz_target"));
  EXPECT_NE(CondBr, nullptr);
  EXPECT_EQ(UncondBr, nullptr);
}

// --- analyzeBranch regression tests (most recent fix, b7aeee9bb973) ---
//
// analyzeBranch previously returned `false` on every code path, silently
// disabling BinaryFunction::fixBranches() for all PPC64 functions. It must
// now return `true` whenever the block's control flow was successfully
// classified (including the "ends in a call" fallthrough case), and `false`
// only when the terminator is genuinely unanalyzable (indirect branch, or a
// branch whose target symbol can't be resolved).

TEST_F(PPCMCPlusBuilderFixture, AnalyzeBranch_EmptyBlock) {
  InstructionListType Insts;
  const MCSymbol *Tgt = nullptr, *Fallthrough = nullptr;
  MCInst *CondBr = nullptr, *UncondBr = nullptr;

  bool Result = BC->MIB->analyzeBranch(Insts.begin(), Insts.end(), Tgt,
                                       Fallthrough, CondBr, UncondBr);

  EXPECT_TRUE(Result);
  EXPECT_EQ(Tgt, nullptr);
  EXPECT_EQ(Fallthrough, nullptr);
  EXPECT_EQ(CondBr, nullptr);
  EXPECT_EQ(UncondBr, nullptr);
}

TEST_F(PPCMCPlusBuilderFixture, AnalyzeBranch_Return) {
  InstructionListType Insts{makeReturn()};
  const MCSymbol *Tgt = nullptr, *Fallthrough = nullptr;
  MCInst *CondBr = nullptr, *UncondBr = nullptr;

  bool Result = BC->MIB->analyzeBranch(Insts.begin(), Insts.end(), Tgt,
                                       Fallthrough, CondBr, UncondBr);

  EXPECT_TRUE(Result);
  EXPECT_EQ(CondBr, nullptr);
  EXPECT_EQ(UncondBr, nullptr);
}

TEST_F(PPCMCPlusBuilderFixture, AnalyzeBranch_UnconditionalBranchResolvable) {
  InstructionListType Insts{makeUncondBranch("uncond_target")};
  const MCSymbol *Tgt = nullptr, *Fallthrough = nullptr;
  MCInst *CondBr = nullptr, *UncondBr = nullptr;

  bool Result = BC->MIB->analyzeBranch(Insts.begin(), Insts.end(), Tgt,
                                       Fallthrough, CondBr, UncondBr);

  EXPECT_TRUE(Result);
  EXPECT_NE(Tgt, nullptr);
  EXPECT_EQ(Tgt, BC->Ctx->lookupSymbol("uncond_target"));
  EXPECT_NE(UncondBr, nullptr);
  EXPECT_EQ(CondBr, nullptr);
}

TEST_F(PPCMCPlusBuilderFixture, AnalyzeBranch_ConditionalBranchResolvable) {
  InstructionListType Insts{makeCondBranch("cond_target")};
  const MCSymbol *Tgt = nullptr, *Fallthrough = nullptr;
  MCInst *CondBr = nullptr, *UncondBr = nullptr;

  bool Result = BC->MIB->analyzeBranch(Insts.begin(), Insts.end(), Tgt,
                                       Fallthrough, CondBr, UncondBr);

  EXPECT_TRUE(Result);
  EXPECT_NE(Tgt, nullptr);
  EXPECT_EQ(Tgt, BC->Ctx->lookupSymbol("cond_target"));
  EXPECT_NE(CondBr, nullptr);
  EXPECT_EQ(UncondBr, nullptr);
}

TEST_F(PPCMCPlusBuilderFixture, AnalyzeBranch_IndirectBranchIsUnanalyzable) {
  InstructionListType Insts{makeIndirectBranch()};
  const MCSymbol *Tgt = nullptr, *Fallthrough = nullptr;
  MCInst *CondBr = nullptr, *UncondBr = nullptr;

  bool Result = BC->MIB->analyzeBranch(Insts.begin(), Insts.end(), Tgt,
                                       Fallthrough, CondBr, UncondBr);

  EXPECT_FALSE(Result);
}

TEST_F(PPCMCPlusBuilderFixture, AnalyzeBranch_CallEndingBlockIsFallthrough) {
  // A block ending in a call (e.g. `bl RegisterManagedStatic`) with no
  // terminator instruction after it: purely a fallthrough block. This is
  // exactly the case that was silently mishandled before the fix (the block
  // has one CFG successor reached only by fallthrough, and fixBranches()
  // relies on `true` here to know it may need to insert a corrective
  // unconditional branch if that successor is no longer next in the final
  // layout).
  InstructionListType Insts{makeCall("RegisterManagedStatic")};
  const MCSymbol *Tgt = nullptr, *Fallthrough = nullptr;
  MCInst *CondBr = nullptr, *UncondBr = nullptr;

  bool Result = BC->MIB->analyzeBranch(Insts.begin(), Insts.end(), Tgt,
                                       Fallthrough, CondBr, UncondBr);

  EXPECT_TRUE(Result);
  EXPECT_EQ(CondBr, nullptr);
  EXPECT_EQ(UncondBr, nullptr);
}

// --- 64-bit opcode-twin tests ---
//
// BLR8, BCTR8 and BCTRL8 are declared under
// `let Interpretation64Bit = 1, isCodeGenOnly = 1` in PPCInstr64Bit.td: they
// encode identically to BLR/BCTR/BCTRL, so the disassembler never produces
// them and every predicate below saw them only when this port created them
// itself -- createLongJmp() ends its stub with BCTR8 (tail call) or BCTRL8
// (call), and buildCallStubTOCThunk() ends its with BLR8. Every predicate in
// PPCMCPlusBuilder listed only the non-8 spellings, so the port could not
// classify its own output: a stub's terminator was neither a branch nor a
// return nor convertible to a tail call. These tests pin both spellings to
// the same answer.

TEST_F(PPCMCPlusBuilderFixture, IsReturn_BLR_And_BLR8) {
  for (unsigned Opc : {PPC::BLR, PPC::BLR8}) {
    MCInst I;
    I.setOpcode(Opc);
    EXPECT_TRUE(BC->MIB->isReturn(I))
        << "opcode " << Opc << " should be classified as a return";
  }
}

TEST_F(PPCMCPlusBuilderFixture, IsBranch_CTR_And_LR_Twins) {
  for (unsigned Opc : {PPC::BCTR, PPC::BCTR8, PPC::BCTRL, PPC::BCTRL8,
                       PPC::BLR, PPC::BLR8}) {
    MCInst I;
    I.setOpcode(Opc);
    EXPECT_TRUE(BC->MIB->isBranch(I))
        << "opcode " << Opc << " should be classified as a branch";
  }
}

TEST_F(PPCMCPlusBuilderFixture, IsIndirectBranch_CTR_Twins) {
  for (unsigned Opc : {PPC::BCTR, PPC::BCTR8, PPC::BCTRL, PPC::BCTRL8}) {
    MCInst I;
    I.setOpcode(Opc);
    EXPECT_TRUE(BC->MIB->isIndirectBranch(I))
        << "opcode " << Opc << " should be classified as an indirect branch";
  }
}

TEST_F(PPCMCPlusBuilderFixture, IsUnconditionalBranch_BCTR8) {
  // isUnconditionalBranch() short-circuits on isTailCall(), so check the
  // unannotated instruction.
  MCInst I;
  I.setOpcode(PPC::BCTR8);
  EXPECT_TRUE(BC->MIB->isUnconditionalBranch(I));
}

TEST_F(PPCMCPlusBuilderFixture, ConvertJmpToTailCall_BCTR8) {
  for (unsigned Opc : {PPC::BCTR, PPC::BCTR8}) {
    MCInst I;
    I.setOpcode(Opc);
    EXPECT_TRUE(BC->MIB->convertJmpToTailCall(I))
        << "opcode " << Opc << " should be convertible to a tail call";
    EXPECT_TRUE(BC->MIB->isTailCall(I))
        << "opcode " << Opc << " should carry the tail-call annotation after "
                               "convertJmpToTailCall()";
  }
}

TEST_F(PPCMCPlusBuilderFixture, ConvertJmpToTailCall_RejectsNonBranch) {
  // A conditional branch is not an unconditional jump and must be left alone;
  // guards against the switch above being widened carelessly.
  MCInst I = makeCondBranch("cond_target");
  EXPECT_FALSE(BC->MIB->convertJmpToTailCall(I));
  EXPECT_FALSE(BC->MIB->isTailCall(I));
}

// --- createLongJmp round trip ---
//
// The property that actually matters: whatever createLongJmp() emits, this
// same builder has to be able to classify. Asserting on the terminator via
// the predicates rather than on a literal opcode keeps the test honest if the
// stub is ever re-spelled.

TEST_F(PPCMCPlusBuilderFixture, CreateLongJmp_TailCall_TerminatorIsClassified) {
  InstructionListType Seq;
  MCSymbol *Target = BC->Ctx->getOrCreateSymbol("far_target");
  BC->MIB->createLongJmp(Seq, Target, BC->Ctx.get(), /*IsTailCall=*/true);

  ASSERT_FALSE(Seq.empty());
  MCInst &Last = Seq.back();
  EXPECT_TRUE(BC->MIB->isBranch(Last));
  EXPECT_TRUE(BC->MIB->isIndirectBranch(Last));
  EXPECT_FALSE(BC->MIB->isCall(Last)) << "a tail-call stub must not link";
  EXPECT_TRUE(BC->MIB->convertJmpToTailCall(Last));
}

TEST_F(PPCMCPlusBuilderFixture, CreateLongJmp_Call_TerminatorIsClassified) {
  InstructionListType Seq;
  MCSymbol *Target = BC->Ctx->getOrCreateSymbol("far_callee");
  BC->MIB->createLongJmp(Seq, Target, BC->Ctx.get(), /*IsTailCall=*/false);

  ASSERT_FALSE(Seq.empty());
  MCInst &Last = Seq.back();
  EXPECT_TRUE(BC->MIB->isCall(Last)) << "a call stub must link";
  EXPECT_TRUE(BC->MIB->isIndirectBranch(Last));
}

// --- createRelocation(): fixup kind -> PPC64 relocation type ---
//
// BinaryFunction::scanExternalRefs() re-encodes the instructions of a function
// BOLT is not going to rewrite and turns each resulting MCFixup into a
// relocation through this hook, then hands the type straight to
// Relocation::getSizeForType(). A wrong type here therefore either patches the
// wrong bits into a live instruction or trips getSizeForTypePPC64()'s
// llvm_unreachable, both silently in a release build.

TEST_F(PPCMCPlusBuilderFixture, CreateRelocation_DirectBranchesArePCRelative) {
  std::optional<Relocation> Br24 =
      relocForFixup(PPC::fixup_ppc_br24, symExpr("callee"));
  ASSERT_TRUE(Br24.has_value());
  EXPECT_EQ(Br24->Type, uint32_t(ELF::R_PPC64_REL24));

  // A "notoc" call differs only in the TOC convention at the call site, not in
  // the field BOLT rewrites.
  std::optional<Relocation> Notoc =
      relocForFixup(PPC::fixup_ppc_br24_notoc, symExpr("callee"));
  ASSERT_TRUE(Notoc.has_value());
  EXPECT_EQ(Notoc->Type, uint32_t(ELF::R_PPC64_REL24));

  std::optional<Relocation> Cond14 =
      relocForFixup(PPC::fixup_ppc_brcond14, symExpr("taken"));
  ASSERT_TRUE(Cond14.has_value());
  EXPECT_EQ(Cond14->Type, uint32_t(ELF::R_PPC64_REL14));
}

// 'ba'/'bla' and 'bca'/'bcla' hold an absolute target. Reporting them as
// REL24/REL14 -- which the old substring match on "br24"/"cond14" did, since
// those substrings also occur in fixup_ppc_br24abs/fixup_ppc_brcond14abs --
// makes BOLT rewrite the field as a displacement from the instruction, i.e.
// with the opposite meaning.
TEST_F(PPCMCPlusBuilderFixture, CreateRelocation_AbsoluteBranchesAreRejected) {
  EXPECT_FALSE(
      relocForFixup(PPC::fixup_ppc_br24abs, symExpr("abs_target")).has_value());
  EXPECT_FALSE(relocForFixup(PPC::fixup_ppc_brcond14abs, symExpr("abs_target"))
                   .has_value());
}

// The 32-/34-bit Power10 prefixed-instruction fixups span two words and have no
// BOLT relocation type at all; guessing one would corrupt the prefix.
TEST_F(PPCMCPlusBuilderFixture, CreateRelocation_PrefixedFixupsAreRejected) {
  EXPECT_FALSE(relocForFixup(PPC::fixup_ppc_pcrel34, symExpr("d")).has_value());
  EXPECT_FALSE(relocForFixup(PPC::fixup_ppc_imm34, symExpr("d")).has_value());
  EXPECT_FALSE(
      relocForFixup(PPC::fixup_ppc_pcrel32, symExpr("d")).has_value());
  EXPECT_FALSE(relocForFixup(PPC::fixup_ppc_imm32, symExpr("d")).has_value());
}

// Every half16 variant shares one fixup kind and is distinguished only by the
// relocation specifier on the symbol reference, so each must be read off the
// specifier rather than the kind.
TEST_F(PPCMCPlusBuilderFixture, CreateRelocation_Half16FollowsTheSpecifier) {
  struct {
    uint16_t Spec;
    uint32_t Type;
  } Cases[] = {
      {PPC::S_None, ELF::R_PPC64_ADDR16},
      {PPC::S_LO, ELF::R_PPC64_ADDR16_LO},
      {PPC::S_HI, ELF::R_PPC64_ADDR16_HI},
      {PPC::S_HA, ELF::R_PPC64_ADDR16_HA},
      {PPC::S_HIGH, ELF::R_PPC64_ADDR16_HIGH},
      {PPC::S_HIGHA, ELF::R_PPC64_ADDR16_HIGHA},
      {PPC::S_HIGHER, ELF::R_PPC64_ADDR16_HIGHER},
      {PPC::S_HIGHERA, ELF::R_PPC64_ADDR16_HIGHERA},
      {PPC::S_HIGHEST, ELF::R_PPC64_ADDR16_HIGHEST},
      {PPC::S_HIGHESTA, ELF::R_PPC64_ADDR16_HIGHESTA},
  };

  for (const auto &C : Cases) {
    std::optional<Relocation> R =
        relocForFixup(PPC::fixup_ppc_half16, symExpr("sym", C.Spec));
    ASSERT_TRUE(R.has_value()) << "specifier " << C.Spec;
    EXPECT_EQ(R->Type, C.Type) << "specifier " << C.Spec;
    // getSizeForTypePPC64() llvm_unreachable()s on a type it does not list,
    // and scanExternalRefs() calls it on whatever comes back from here.
    EXPECT_EQ(Relocation::getSizeForType(R->Type), 2u) << "specifier "
                                                       << C.Spec;
  }
}

// DS-form (ld/std) and DQ-form (lxv/stxv) instructions own only bits 2..15 of
// the half word the fixup covers; the low two bits belong to the opcode. The
// non-DS relocation types overwrite them, which turns `ld` into `lwa`.
TEST_F(PPCMCPlusBuilderFixture, CreateRelocation_DSFormKeepsTheOpcodeBits) {
  std::optional<Relocation> DS =
      relocForFixup(PPC::fixup_ppc_half16ds, symExpr("sym", PPC::S_LO));
  ASSERT_TRUE(DS.has_value());
  EXPECT_EQ(DS->Type, uint32_t(ELF::R_PPC64_ADDR16_LO_DS));
  EXPECT_EQ(Relocation::getSizeForType(DS->Type), 2u);

  std::optional<Relocation> DQ =
      relocForFixup(PPC::fixup_ppc_half16dq, symExpr("sym", PPC::S_LO));
  ASSERT_TRUE(DQ.has_value());
  EXPECT_EQ(DQ->Type, uint32_t(ELF::R_PPC64_ADDR16_LO_DS));

  std::optional<Relocation> NoSpec =
      relocForFixup(PPC::fixup_ppc_half16ds, symExpr("sym"));
  ASSERT_TRUE(NoSpec.has_value());
  EXPECT_EQ(NoSpec->Type, uint32_t(ELF::R_PPC64_ADDR16_DS));
  EXPECT_EQ(Relocation::getSizeForType(NoSpec->Type), 2u);

  // Same specifier on a D-form fixup keeps the plain type.
  std::optional<Relocation> D =
      relocForFixup(PPC::fixup_ppc_half16, symExpr("sym", PPC::S_LO));
  ASSERT_TRUE(D.has_value());
  EXPECT_EQ(D->Type, uint32_t(ELF::R_PPC64_ADDR16_LO));
}

// A specifier that redirects the reference to another entity -- a GOT or TOC
// slot, a PLT stub, a TLS offset -- cannot be described by a plain ADDR16, so
// the fixup has to be declined rather than mapped by its kind.
TEST_F(PPCMCPlusBuilderFixture, CreateRelocation_OtherSpecifiersAreRejected) {
  for (uint16_t Spec : {PPC::S_GOT, PPC::S_GOT_HA, PPC::S_TOC, PPC::S_TOC_LO,
                        PPC::S_PLT, PPC::S_TPREL, PPC::S_DTPREL_HA})
    EXPECT_FALSE(relocForFixup(PPC::fixup_ppc_half16, symExpr("sym", Spec))
                     .has_value())
        << "specifier " << Spec;
}

// The addend has to survive: scanExternalRefs() re-emits the relocation as
// (Symbol, Type, Addend), so dropping it retargets `sym + 8` at `sym`.
TEST_F(PPCMCPlusBuilderFixture, CreateRelocation_KeepsTheAddend) {
  const MCExpr *SymPlus8 =
      MCBinaryExpr::createAdd(symExpr("sym", PPC::S_LO),
                              MCConstantExpr::create(8, *BC->Ctx), *BC->Ctx);
  std::optional<Relocation> R =
      relocForFixup(PPC::fixup_ppc_half16, SymPlus8);
  ASSERT_TRUE(R.has_value());
  EXPECT_EQ(R->Type, uint32_t(ELF::R_PPC64_ADDR16_LO));
  EXPECT_EQ(R->Symbol->getName(), "sym");
  EXPECT_EQ(R->Addend, 8u);

  // `Sym + C + C` folds into a single addend, as the assembler may leave it.
  const MCExpr *SymPlus12 = MCBinaryExpr::createAdd(
      SymPlus8, MCConstantExpr::create(4, *BC->Ctx), *BC->Ctx);
  std::optional<Relocation> R2 =
      relocForFixup(PPC::fixup_ppc_half16, SymPlus12);
  ASSERT_TRUE(R2.has_value());
  EXPECT_EQ(R2->Addend, 12u);
}

// One relocation names one symbol. A sum of two is not expressible, and picking
// either one silently relocates against the wrong target.
TEST_F(PPCMCPlusBuilderFixture, CreateRelocation_RejectsTwoSymbols) {
  const MCExpr *Sum = MCBinaryExpr::createAdd(
      symExpr("a", PPC::S_LO), symExpr("b", PPC::S_LO), *BC->Ctx);
  EXPECT_FALSE(relocForFixup(PPC::fixup_ppc_half16, Sum).has_value());
}

// replaceImmWithSymbolRef() builds `(.TOC. + 4 - .Ltmp0)@l` for the ELFv2
// global-entry TOC preamble -- an MCSpecifierExpr wrapping a subtraction -- and
// those instructions do reach scanExternalRefs() for any function BOLT decides
// not to rewrite. Neither shape is one relocation, and
// MCPlusBuilder::extractFixupExpr() (which handles only `Sym`, `Sym + C` and
// `Sym + C + C`) asserts on both, so the hook has to decline such a fixup
// itself rather than abort an assertions build.
TEST_F(PPCMCPlusBuilderFixture, CreateRelocation_RejectsUnparsableExpressions) {
  MCSymbol *TOC = BC->Ctx->getOrCreateSymbol(".TOC.");
  MCSymbol *Label = BC->Ctx->getOrCreateSymbol(".Ltmp0");
  const MCExpr *Diff = MCBinaryExpr::createSub(
      MCBinaryExpr::createAdd(MCSymbolRefExpr::create(TOC, *BC->Ctx),
                              MCConstantExpr::create(4, *BC->Ctx), *BC->Ctx),
      MCSymbolRefExpr::create(Label, *BC->Ctx), *BC->Ctx);
  const MCExpr *Preamble =
      MCSpecifierExpr::create(Diff, PPC::S_LO, *BC->Ctx);

  EXPECT_FALSE(relocForFixup(PPC::fixup_ppc_half16, Preamble).has_value());
  // The bare subtraction is just as unparsable, without the wrapper.
  EXPECT_FALSE(relocForFixup(PPC::fixup_ppc_half16, Diff).has_value());
}

#endif // POWERPC_AVAILABLE

} // end anonymous namespace