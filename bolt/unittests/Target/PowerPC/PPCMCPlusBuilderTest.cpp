//===- bolt/unittest/Target/PowerPC/PPCMCPlusBuilderTest.cpp
//-------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Target/PowerPC/PPCMCPlusBuilder.h"
#include "MCTargetDesc/PPCMCTargetDesc.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/MCPlusBuilder.h"
#include "bolt/Rewrite/RewriteInstance.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCSymbolELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/TargetSelect.h"
#include "gtest/gtest.h"

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

#endif // POWERPC_AVAILABLE

} // end anonymous namespace