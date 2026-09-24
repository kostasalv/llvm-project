//===- bolt/Target/PowerPC/PPCMCPlusBuilder.cpp -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides PowerPC-specific MCPlus builder.
//
//===----------------------------------------------------------------------===//

#include "bolt/Target/PowerPC/PPCMCPlusBuilder.h"
#include "MCTargetDesc/PPCMCTargetDesc.h"
#include "bolt/Core/MCPlusBuilder.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCRegisterInfo.h"
#include <cstdint>
#define DEBUG_TYPE "bolt-ppc"
#include "MCTargetDesc/PPCFixupKinds.h"
#include "MCTargetDesc/PPCMCAsmInfo.h"
#include "bolt/Core/BinaryFunction.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <string>

using namespace llvm;
using namespace bolt;

static inline unsigned opc(const MCInst &I) { return I.getOpcode(); }

void PPCMCPlusBuilder::createPushRegisters(MCInst &Inst1, MCInst &Inst2,
                                           MCPhysReg Reg1, MCPhysReg /*Reg2*/) {
  // Emit two NOPs (ori r0, r0, 0)
  Inst1.clear();
  Inst1.setOpcode(PPC::ORI);
  Inst1.addOperand(MCOperand::createReg(PPC::R0));
  Inst1.addOperand(MCOperand::createReg(PPC::R0));
  Inst1.addOperand(MCOperand::createImm(0));
  Inst2 = Inst1;
}

bool PPCMCPlusBuilder::shouldRecordCodeRelocation(unsigned Type) const {
  // On PPC64 ELFv2, R_PPC64_REL24 is used for direct calls (bl instructions).
  // For functions that BOLT processes (simple functions), call targets are
  // symbolized during disassembly via evaluateBranch/replaceBranchTarget, so
  // storing the raw relocation is not needed.
  // For non-simple functions emitted as raw bytes (e.g. PLT branch stubs),
  // emitting R_PPC64_REL24 causes JITLink to create CallBranchDeltaRestoreTOC
  // edges that expect a NOP at call+4. But PLT stubs have real code there
  // (ld r2,24(r1) or the next instruction), causing an assertion failure.
  // Therefore, do NOT record R_PPC64_REL24 as a code relocation on PPC64.
  switch (Type) {
  case ELF::R_PPC64_REL14:
    return true;
  default:
    return false;
  }
}

// Sign-extend 24-bit field (BD/LI is 24 bits, multiplied by 4)
static inline int64_t signExtend24(int64_t v) {
  v &= 0x00ffffff;
  if (v & 0x00800000)
    v |= ~0x00ffffff;
  return v;
}

bool PPCMCPlusBuilder::evaluateBranch(const MCInst &I, uint64_t PC,
                                      uint64_t Size, uint64_t &Target) const {
  if (!hasPCRelOperand(I))
    return false;
  const int Op = getPCRelOperandNum(I);
  if (Op < 0 || !I.getOperand(Op).isImm())
    return false;

  int64_t wordDisp = I.getOperand(Op).getImm();   // units of 4 bytes
  int64_t byteDisp = signExtend24(wordDisp) << 2; // 24-bit signed * 4
  Target =
      PC + byteDisp; // PPC branches are relative to the branch insn address
  return true;
}

bool PPCMCPlusBuilder::evaluateMemOperandTarget(const MCInst &, uint64_t &,
                                                uint64_t, uint64_t) const {
  LLVM_DEBUG(dbgs() << "PPC: no PC-rel mem operand on this target\n");
  return false;
}

bool PPCMCPlusBuilder::hasPCRelOperand(const MCInst &I) const {
  return getPCRelOperandNum(I) >= 0;
}

int PPCMCPlusBuilder::getPCRelOperandNum(const MCInst &I) const {
  switch (I.getOpcode()) {
  // Relative direct call/branch – target is operand #0 in MC (Imm/Expr)
  case PPC::BL:           // 32-bit relative call
  case PPC::BL8:          // 64-bit relative call
  case PPC::BL8_TLS:
  case PPC::BL8_TLS_:
  case PPC::BL8_NOP:
  case PPC::BL8_NOP_TLS:
  case PPC::BL8_NOTOC:
  case PPC::BL8_NOTOC_TLS:
  case PPC::BL8_RM:
  case PPC::BL8_NOP_RM:
  case PPC::BL8_NOTOC_RM:
  case PPC::BL8_LDinto_toc:
  case PPC::BL8_LDinto_toc_RM:
  case PPC::B:             // unconditional relative branch
  // BDNZ/BDZ family (decrement CTR and branch if [not] zero) and their
  // link/branch-hint variants -- see the block comment on
  // getPCRelEncodingSize() below for why these share BC/BCL's 14-bit BD
  // field rather than B/BL's 26-bit LI field. AA=0 (PC-relative) forms only;
  // the AA=1 (absolute, *A suffix) forms are handled in the "no PC-relative
  // operand" bucket below, matching BA/BLA.
  case PPC::BDNZ:
  case PPC::BDNZL:
  case PPC::BDNZp:
  case PPC::BDNZLp:
  case PPC::BDNZm:
  case PPC::BDNZLm:
  case PPC::BDZ:
  case PPC::BDZL:
  case PPC::BDZp:
  case PPC::BDZLp:
  case PPC::BDZm:
  case PPC::BDZLm:
  case PPC::BDNZ8:
  case PPC::BDZ8:
    return 0;

  // Conditional relative branch: BO, BI, BD (target at operand 2)
  case PPC::BC:
  case PPC::BCL:
  case PPC::gBC:  // bt/bf mnemonics (alias for BC with specific BO values)
  case PPC::gBCL:
  // BCC/BCCL: pred(imm), CR(reg), target(imm/expr) — target at operand 2
  case PPC::BCC:
  case PPC::BCCL:
    return 2;

  // Absolute branches/calls (AA=1) — no PC-relative operand
  case PPC::BLA:
  case PPC::BLA8:
  case PPC::BLA8_NOP:
  case PPC::BLA8_RM:
  case PPC::BLA8_NOP_RM:
  case PPC::BA:
  case PPC::BDNZA:
  case PPC::BDNZLA:
  case PPC::BDNZAp:
  case PPC::BDNZLAp:
  case PPC::BDNZAm:
  case PPC::BDNZLAm:
  case PPC::BDZA:
  case PPC::BDZLA:
  case PPC::BDZAp:
  case PPC::BDZLAp:
  case PPC::BDZAm:
  case PPC::BDZLAm:
    return -1;

  default:
    return -1;
  }
}

int PPCMCPlusBuilder::getPCRelEncodingSize(const MCInst &Inst) const {
  switch (Inst.getOpcode()) {
  // Unconditional branch / call: 26-bit signed offset (±32MB)
  // Must match every opcode that getPCRelOperandNum() returns >=0 for,
  // otherwise needsStub() gets BitsAvail=-1 and flags every branch as
  // out-of-range, causing infinite stub-insertion iterations.
  case PPC::B:
  case PPC::BL:
  case PPC::BL8:
  case PPC::BL8_TLS:
  case PPC::BL8_TLS_:
  case PPC::BL8_NOP:
  case PPC::BL8_NOP_TLS:
  case PPC::BL8_NOTOC:
  case PPC::BL8_NOTOC_TLS:
  case PPC::BL8_RM:
  case PPC::BL8_NOP_RM:
  case PPC::BL8_NOTOC_RM:
  case PPC::BL8_LDinto_toc:
  case PPC::BL8_LDinto_toc_RM:
    return 26;
  // Conditional branch: 16-bit signed offset (±32KB).
  // BDNZ/BDZ ("decrement CTR and branch if [not] zero", used for loop
  // backedges) share the exact same B-form 14-bit BD displacement field as
  // BC/BCL (see PPCInstrFormats.td's BForm_1, used by both) -- they are NOT
  // 26-bit like B/BL. Misclassifying them as 26-bit made needsStub()
  // consider far-away BDNZ targets (e.g. across a hot/cold split, ~500KB
  // away) as "in range" when they are actually restricted to ±32KB, so no
  // stub was ever created and JITLink later rejected the Delta14 fixup as
  // out of range.
  //
  // This is the whole BDNZ/BDZ family, not just BDNZ/BDNZL: every mnemonic
  // built from BForm_1 (bdnz/bdz, their absolute "*a" forms, their
  // link "*l" forms, and their branch-hint "+"/"-" forms, i.e. "*p"/"*m" in
  // TableGen) encodes the same 14-bit BD field, and real codegen emits
  // several of them -- PPCCTRLoops.cpp materializes plain bdnz/bdz for
  // hardware-loop backedges, and PPCBranchSelector.cpp rewrites those to
  // their "opposite" mnemonic (bdnz<->bdz) when expanding an out-of-range
  // branch, so bdz is just as reachable as bdnz in real binaries. The
  // absolute ("*a") and 64-bit-pseudo (BDNZ8/BDZ8, used pre-encoding by
  // codegen but not emitted by the disassembler) forms are included here
  // for completeness/defensiveness even though only bdnz/bdz/bdnzl/bdzl are
  // expected to appear in disassembled PPC64 ELFv2 binaries BOLT processes.
  case PPC::BC:
  case PPC::gBC:
  case PPC::BCL:
  case PPC::gBCL:
  case PPC::BCC:   // extended-mnemonic conditional branch (bt/bf/beq/bne...)
  case PPC::BCCA:  // conditional branch absolute (extended mnemonic)
  case PPC::BCCL:  // conditional branch with link (extended mnemonic)
  case PPC::BCCLA: // conditional branch with link absolute (extended mnemonic)
  case PPC::BDNZ:
  case PPC::BDNZL:
  case PPC::BDNZA:
  case PPC::BDNZLA:
  case PPC::BDNZp:
  case PPC::BDNZLp:
  case PPC::BDNZAp:
  case PPC::BDNZLAp:
  case PPC::BDNZm:
  case PPC::BDNZLm:
  case PPC::BDNZAm:
  case PPC::BDNZLAm:
  case PPC::BDZ:
  case PPC::BDZL:
  case PPC::BDZA:
  case PPC::BDZLA:
  case PPC::BDZp:
  case PPC::BDZLp:
  case PPC::BDZAp:
  case PPC::BDZLAp:
  case PPC::BDZm:
  case PPC::BDZLm:
  case PPC::BDZAm:
  case PPC::BDZLAm:
  case PPC::BDNZ8:
  case PPC::BDZ8:
    return 16;
  default:
    return 0;
  }
}

// PPC64 unconditional branch (b / bl) uses a 26-bit signed LI field → ±32MB.
int PPCMCPlusBuilder::getUncondBranchEncodingSize() const { return 26; }

// PPC64 has no intermediate "short jump" encoding — the only single-instruction
// unconditional branch is 'b' (26-bit). Return the same value so the short-jmp
// relaxation path in LongJmpPass::relaxStub is effectively skipped.
int PPCMCPlusBuilder::getShortJmpEncodingSize() const { return 26; }

bool PPCMCPlusBuilder::replaceImmWithSymbolRef(MCInst &Inst,
                                               const MCSymbol *Symbol,
                                               int64_t Addend, MCContext *Ctx,
                                               int64_t &Value,
                                               uint32_t RelType) const {
  // Only the PC-relative half16 family is handled here. Those immediates hold
  // ha()/hi()/lo() of a displacement from the address of the instruction that
  // carries them, so they have to be re-expressed against wherever BOLT emits
  // the instruction rather than copied. The ELFv2 global entry point preamble
  // is the ubiquitous case:
  //
  //   addis r2, r12, (.TOC. - func)@ha    R_PPC64_REL16_HA .TOC. + 0
  //   addi  r2, r2,  (.TOC. - func)@l     R_PPC64_REL16_LO .TOC. + 4
  //
  // Everything else is either a branch (symbolized from its target during
  // disassembly) or TOC/GOT-relative, which survives the move untouched
  // because neither r2 nor the TOC changes. Report those as not replaced.
  PPC::Specifier Spec;
  switch (RelType) {
  default:
    return false;
  case ELF::R_PPC64_REL16:
    Spec = PPC::S_None;
    break;
  case ELF::R_PPC64_REL16_LO:
    Spec = PPC::S_LO;
    break;
  case ELF::R_PPC64_REL16_HI:
    Spec = PPC::S_HI;
    break;
  case ELF::R_PPC64_REL16_HA:
    Spec = PPC::S_HA;
    break;
  }

  // Locate the immediate field. The half16 forms land on addis/addi/ori-style
  // instructions, which carry exactly one immediate; bail out rather than guess
  // if that is not the shape we got.
  const unsigned NumOperands = MCPlus::getNumPrimeOperands(Inst);
  unsigned OpIdx = NumOperands;
  for (unsigned I = 0; I != NumOperands; ++I) {
    if (!Inst.getOperand(I).isImm())
      continue;
    if (OpIdx != NumOperands)
      return false;
    OpIdx = I;
  }
  if (OpIdx == NumOperands)
    return false;

  // Build (Symbol + Addend - L)@spec, with L a label emitted immediately before
  // this instruction (see BinaryEmitter's getInstLabel handling). That is the
  // relocation's own definition, S + A - P, with L standing in for P, so it
  // stays correct at any output address. For the preamble above it yields
  // (.TOC. + 0 - func) on the addis and (.TOC. + 4 - (func + 4)) on the addi,
  // i.e. the same .TOC. - func displacement in both - and the assembler
  // re-emits R_PPC64_REL16_HA/_LO against .TOC., which JITLink resolves as
  // Delta16HA/Delta16LO once the final layout is known.
  MCSymbol *Label = getOrCreateInstLabel(Inst, "PPCPCRel", Ctx);
  const MCExpr *Ref = MCSymbolRefExpr::create(Symbol, *Ctx);
  if (Addend)
    Ref = MCBinaryExpr::createAdd(Ref, MCConstantExpr::create(Addend, *Ctx),
                                  *Ctx);
  const MCExpr *Expr =
      MCBinaryExpr::createSub(Ref, MCSymbolRefExpr::create(Label, *Ctx), *Ctx);
  if (Spec != PPC::S_None)
    Expr = MCSpecifierExpr::create(Expr, Spec, *Ctx);

  Inst.getOperand(OpIdx) = MCOperand::createExpr(Expr);
  Value = 0;
  return true;
}

void PPCMCPlusBuilder::createLongJmp(InstructionListType &Seq,
                                     const MCSymbol *Target, MCContext *Ctx,
                                     bool IsTailCall) {
  // PPC64 ELFv2 absolute jump via CTR using r12 (scratch/callee-clobber).
  // This sequence loads a full 64-bit address into r12 and jumps via CTR:
  //   lis8  r12, target@highest    ; r12 = bits[63:48]
  //   ori8  r12, r12, target@higher ; r12 |= bits[47:32]
  //   rldicr r12, r12, 32, 31     ; r12 <<= 32 (shift high half into place)
  //   oris8 r12, r12, target@h    ; r12 |= bits[31:16]
  //   ori8  r12, r12, target@l    ; r12 |= bits[15:0]
  //   mtctr r12                   ; CTR = r12
  //   bctr / bctrl                ; jump to CTR
  //
  // r12 is the ELFv2 ABI "function entry address" register used by GEP
  // prologues to reconstruct r2/TOC, so reusing it here is ABI-correct.
  // Same materialization sequence is used by the PLT call stubs below.
  const unsigned R12 = PPC::X12;

  const MCExpr *HST = MCSymbolRefExpr::create(Target, PPC::S_HIGHEST, *Ctx);
  const MCExpr *HER = MCSymbolRefExpr::create(Target, PPC::S_HIGHER, *Ctx);
  const MCExpr *HI  = MCSymbolRefExpr::create(Target, PPC::S_HI, *Ctx);
  const MCExpr *LO  = MCSymbolRefExpr::create(Target, PPC::S_LO, *Ctx);

  MCInst I;

  // lis8 r12, target@highest
  I = MCInst();
  I.setOpcode(PPC::LIS8);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createExpr(HST));
  Seq.emplace_back(I);

  // ori8 r12, r12, target@higher
  I = MCInst();
  I.setOpcode(PPC::ORI8);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createExpr(HER));
  Seq.emplace_back(I);

  // rldicr r12, r12, 32, 31
  I = MCInst();
  I.setOpcode(PPC::RLDICR);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createImm(32));
  I.addOperand(MCOperand::createImm(31));
  Seq.emplace_back(I);

  // oris8 r12, r12, target@h
  I = MCInst();
  I.setOpcode(PPC::ORIS8);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createExpr(HI));
  Seq.emplace_back(I);

  // ori8 r12, r12, target@l
  I = MCInst();
  I.setOpcode(PPC::ORI8);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createExpr(LO));
  Seq.emplace_back(I);

  // mtctr r12
  I = MCInst();
  I.setOpcode(PPC::MTCTR8);
  I.addOperand(MCOperand::createReg(R12));
  Seq.emplace_back(I);

  // bctr (tail call) or bctrl (regular call)
  I = MCInst();
  I.setOpcode(IsTailCall ? PPC::BCTR8 : PPC::BCTRL8);
  Seq.emplace_back(I);
}

void PPCMCPlusBuilder::createLongJmpWithTOCRestore(InstructionListType &Seq,
                                                    const MCSymbol *Target,
                                                    MCContext *Ctx,
                                                    uint64_t TOCBase,
                                                    bool IsTailCall) {
  // PPC64 ELFv2: linker-generated PLT/branch-extension stubs (.plt_call.,
  // .plt_branch.) are the ORIGINAL binary's un-rewritten code.  Per the
  // ELFv2 ABI (Section 4.2.5.3, Procedure Linkage Table): "the caller has
  // set up r2 to hold the TOC pointer" -- these stubs load their real
  // target from a TOC/GOT-relative offset using r2, with no GEP prologue
  // of their own to reconstruct it (unlike a BOLT-rewritten function, whose
  // 2-instruction GEP prologue sets its OWN r2 independently of the caller).
  //
  // A plain createLongJmp() 'bctr' preserves whatever r2 the CALLING
  // function's rewritten body last set, which is that function's own new
  // TOC base -- not the original one these stubs need.  The stub then
  // computes a garbage GOT-relative address and jumps to it (observed as a
  // SIGSEGV to a non-canonical PC).
  //
  // Fix: materialize the ORIGINAL TOC base into r2 immediately before the
  // jump, using the same absolute-immediate-load sequence as the address
  // load into r12 (lis/ori/rldicr/oris/ori). r2 is not used again by our
  // stub itself (control transfers away via bctr), so clobbering it here is
  // safe from our side; it is exactly what the target expects to find.
  const unsigned R2 = PPC::X2;
  const uint16_t Highest = (TOCBase >> 48) & 0xffff;
  const uint16_t Higher = (TOCBase >> 32) & 0xffff;
  const uint16_t Hi = (TOCBase >> 16) & 0xffff;
  const uint16_t Lo = TOCBase & 0xffff;

  MCInst I;

  // lis r2, TOCBase@highest
  I = MCInst();
  I.setOpcode(PPC::LIS8);
  I.addOperand(MCOperand::createReg(R2));
  I.addOperand(MCOperand::createImm((int16_t)Highest));
  Seq.emplace_back(I);

  // ori r2, r2, TOCBase@higher
  I = MCInst();
  I.setOpcode(PPC::ORI8);
  I.addOperand(MCOperand::createReg(R2));
  I.addOperand(MCOperand::createReg(R2));
  I.addOperand(MCOperand::createImm(Higher));
  Seq.emplace_back(I);

  // rldicr r2, r2, 32, 31
  I = MCInst();
  I.setOpcode(PPC::RLDICR);
  I.addOperand(MCOperand::createReg(R2));
  I.addOperand(MCOperand::createReg(R2));
  I.addOperand(MCOperand::createImm(32));
  I.addOperand(MCOperand::createImm(31));
  Seq.emplace_back(I);

  // oris r2, r2, TOCBase@h
  I = MCInst();
  I.setOpcode(PPC::ORIS8);
  I.addOperand(MCOperand::createReg(R2));
  I.addOperand(MCOperand::createReg(R2));
  I.addOperand(MCOperand::createImm(Hi));
  Seq.emplace_back(I);

  // ori r2, r2, TOCBase@l
  I = MCInst();
  I.setOpcode(PPC::ORI8);
  I.addOperand(MCOperand::createReg(R2));
  I.addOperand(MCOperand::createReg(R2));
  I.addOperand(MCOperand::createImm(Lo));
  Seq.emplace_back(I);

  // Now the usual absolute jump via r12/CTR, reusing createLongJmp's logic.
  InstructionListType JmpSeq;
  createLongJmp(JmpSeq, Target, Ctx, IsTailCall);
  Seq.insert(Seq.end(), JmpSeq.begin(), JmpSeq.end());
}

int PPCMCPlusBuilder::getMemoryOperandNo(const MCInst & /*Inst*/) const {
  return -1;
}

void PPCMCPlusBuilder::replaceBranchTarget(MCInst &Inst, const MCSymbol *TBB,
                                           MCContext *Ctx) const {
  const int OpNum = getPCRelOperandNum(Inst);
  if (OpNum < 0) {
    LLVM_DEBUG(dbgs() << "PPC: no PC-rel operand to replace in "
                      << Info->getName(Inst.getOpcode()) << "\n");
    return; // gracefully do nothing
  }
  Inst.getOperand(OpNum) =
      MCOperand::createExpr(MCSymbolRefExpr::create(TBB, *Ctx));
}

bool PPCMCPlusBuilder::isIndirectBranch(const MCInst &I) const {
  switch (I.getOpcode()) {
  case PPC::BCTR:
  case PPC::BCTRL:
  case PPC::BCLR:
  case PPC::BCLRL:
    return true;
  default:
    return false;
  }
}

const MCSymbol *PPCMCPlusBuilder::getTargetSymbol(const MCInst &Inst,
                                                  unsigned OpNum) const {
  // If OpNum was not explicitly specified by the caller, find it via
  // getPCRelOperandNum (the same operand used by evaluateBranch).
  // This is needed by LongJmpPass::needsStub() which calls
  // getTargetSymbol(Inst) with the default OpNum=0 for all branch/call insns.
  //
  // Root cause this fixes for callers such as analyzeBranch(): a branch's
  // target operand is NOT always the last MCOperand on the instruction.
  // BOLT attaches extra state (e.g. execution-count/misprediction-count
  // MCAnnotation operands, added once a profile is read -- see
  // BinaryFunction::readProfileData / applyProfile-like sites) as trailing
  // MCOperands on branch instructions. A naive "target = last operand"
  // lookup (as a prior version of analyzeBranch() used, via a static local
  // getBranchTargetSymbol() helper in this file) silently returns whatever
  // trailing annotation operand happens to be last -- which is not an
  // MCExpr, so it looks like "no target symbol" and analyzeBranch() reports
  // the block as unanalyzable. BinaryFunction::fixBranches() then skips that
  // block entirely instead of appending the corrective unconditional branch
  // it needs after a later layout change (e.g. the second -split-functions
  // run that happens after -reorder-functions, or -reorder-blocks). The
  // block silently falls through into the bytes of whatever block the
  // layout put next -- observed as a heap-corruption-shaped SIGSEGV deep in
  // an unrelated DenseMap, because the fallen-through-into code overwrites
  // registers (e.g. the hash/bucket-index computation in
  // DenseMap::LookupBucketFor) without ever running.
  //
  // getPCRelOperandNum() below returns a FIXED, per-opcode operand index
  // (e.g. 2 for gBC/BC's BD field) rather than "the last operand", so it is
  // immune to trailing annotation operands and is the correct way to find
  // a branch's target operand on this target.
  int PCRelOp = getPCRelOperandNum(Inst);
  if (PCRelOp < 0)
    return nullptr;
  // Use the PC-relative operand index unless the caller passed an explicit one.
  unsigned EffectiveOp = (OpNum == 0 && (unsigned)PCRelOp != 0)
                             ? (unsigned)PCRelOp
                             : OpNum;
  if (EffectiveOp >= Inst.getNumOperands())
    return nullptr;
  const MCOperand &Op = Inst.getOperand(EffectiveOp);
  if (!Op.isExpr())
    return nullptr;
  return MCPlusBuilder::getTargetSymbol(Op.getExpr());
}

bool PPCMCPlusBuilder::convertJmpToTailCall(MCInst &Inst) {
  // Root cause fixed here: this function must mark the instruction with
  // setTailCall() so that isTailCall() (the base-class implementation,
  // which just checks the kTailCall annotation -- PPC does not override
  // it, matching X86/AArch64) actually recognizes it afterward. Without
  // this call, every plain unconditional branch used as a tail call
  // (e.g. "b <plt_call>" in _start, or any direct-branch tail call
  // produced by the compiler) is indistinguishable from a normal
  // intra-function branch to analyzeBranch()/the CFG builder: it gets
  // UncondBr set with a target symbol pointing at code outside this
  // function, and validateCFG() then fails because the block legitimately
  // has zero local successors (it's an exit via tail call) while
  // analyzeBranch() still reports a live UncondBr.
  switch (Inst.getOpcode()) {
  case PPC::B:
  case PPC::BA:
  case PPC::BCTR:
    setTailCall(Inst);
    return true;
  default:
    return false;
  }
}

bool PPCMCPlusBuilder::isCall(const MCInst &I) const {
  switch (I.getOpcode()) {
  // 32-bit direct calls
  case PPC::BL:
  case PPC::BLA:
  // 64-bit direct calls
  case PPC::BL8:
  case PPC::BL8_TLS:
  case PPC::BL8_TLS_:
  case PPC::BLA8:
  case PPC::BL8_NOP:
  case PPC::BL8_NOP_TLS:
  case PPC::BLA8_NOP:
  case PPC::BL8_NOTOC:
  case PPC::BL8_NOTOC_TLS:
  case PPC::BL8_RM:
  case PPC::BLA8_RM:
  case PPC::BL8_NOP_RM:
  case PPC::BLA8_NOP_RM:
  case PPC::BL8_NOTOC_RM:
  case PPC::BL8_LDinto_toc:
  case PPC::BL8_LDinto_toc_RM:
  // Indirect calls via CTR (32 and 64-bit)
  case PPC::BCTRL:
  case PPC::BCTRL8:
  case PPC::BCTRL8_RM:
  case PPC::BCTRL8_LDinto_toc:
  case PPC::BCTRL8_LDinto_toc_RM:
    return true;
  default:
    return false;
  }
}

bool PPCMCPlusBuilder::isCallWithNOPSlot(const MCInst &I) const {
  // These call variants encode both the bl and the nop as a single MCInst
  // (8 bytes total). Do not inject an additional NOP after them.
  switch (I.getOpcode()) {
  case PPC::BL8_NOP:
  case PPC::BL8_NOP_TLS:
  case PPC::BLA8_NOP:
  case PPC::BL8_NOP_RM:
  case PPC::BLA8_NOP_RM:
  case PPC::BL8_LDinto_toc:
  case PPC::BL8_LDinto_toc_RM:
    return true;
  default:
    return false;
  }
}

bool PPCMCPlusBuilder::ensureCallNOPSlot(MCInst &Inst) const {
  // Upgrade a plain BL8 (4 bytes) to BL8_NOP (8 bytes = bl + nop) so that:
  //  1. The assembler emits a NOP slot JITLink can rewrite to a TOC-restore
  //     for external calls via CallBranchDeltaRestoreTOC.
  //  2. computeCodeSize() correctly accounts for the 8-byte encoding in the
  //     tentative layout, keeping LongJmpPass range checks accurate.
  // BL8_NOP takes the same single operand (target symbol) as BL8.
  if (Inst.getOpcode() == PPC::BL8) {
    Inst.setOpcode(PPC::BL8_NOP);
    return true;
  }
  return false;
}

bool PPCMCPlusBuilder::isIndirectCall(const MCInst &I) const {
  switch (I.getOpcode()) {
  case PPC::BCTRL:
  case PPC::BCTRL8:
  case PPC::BCTRL8_RM:
  case PPC::BCTRL8_LDinto_toc:
  case PPC::BCTRL8_LDinto_toc_RM:
    return true;
  default:
    return false;
  }
}

bool PPCMCPlusBuilder::isBranch(const MCInst &I) const {
  switch (I.getOpcode()) {
  case PPC::B:     // unconditional branch
  case PPC::BL:    // branch with link (treated as call, but still a branch)
  case PPC::BLA:   // absolute branch with link
  case PPC::BC:    // conditional branch (BC/BCL with explicit BO,BI fields)
  case PPC::BCL:   // conditional branch with link
  case PPC::BCC:   // conditional branch using extended mnemonics (bt/bf/beq/bne...)
  case PPC::BCCA:  // conditional branch absolute (extended mnemonic)
  case PPC::BCCL:  // conditional branch with link (extended mnemonic)
  case PPC::BCCLA: // conditional branch with link absolute (extended mnemonic)
  case PPC::gBC:   // generic conditional branch (bt/bf with BO field)
  case PPC::gBCL:  // generic conditional branch with link
  // BDNZ/BDZ family (decrement CTR and branch if [not] zero, plus their
  // absolute/link/branch-hint variants) -- see isConditionalBranch() for
  // why the whole BForm_1 family, not just BDNZ/BDNZL, needs to be listed.
  case PPC::BDNZ:
  case PPC::BDNZL:
  case PPC::BDNZA:
  case PPC::BDNZLA:
  case PPC::BDNZp:
  case PPC::BDNZLp:
  case PPC::BDNZAp:
  case PPC::BDNZLAp:
  case PPC::BDNZm:
  case PPC::BDNZLm:
  case PPC::BDNZAm:
  case PPC::BDNZLAm:
  case PPC::BDZ:
  case PPC::BDZL:
  case PPC::BDZA:
  case PPC::BDZLA:
  case PPC::BDZp:
  case PPC::BDZLp:
  case PPC::BDZAp:
  case PPC::BDZLAp:
  case PPC::BDZm:
  case PPC::BDZLm:
  case PPC::BDZAm:
  case PPC::BDZLAm:
  case PPC::BDNZ8:
  case PPC::BDZ8:
  case PPC::BCTR:  // branch to CTR
  case PPC::BCTRL: // branch to CTR with link
  case PPC::BLR:   // branch to LR
  case PPC::BLRL:  // branch to LR with link
    return true;
  default:
    return false;
  }
}

bool PPCMCPlusBuilder::isReturn(const MCInst &Inst) const {
  return Inst.getOpcode() == PPC::BLR;
}

bool PPCMCPlusBuilder::isConditionalReturn(const MCInst &Inst) const {
  // PPC64 has three families of "conditional return" instructions: the
  // taken path returns to the caller via LR (no explicit branch-target
  // operand at all -- the target is implicit in the LR register), and the
  // not-taken path falls through to the next instruction in program order.
  // None of isBranch()/isConditionalBranch()/isReturn() recognize these
  // opcodes, which is the actual root cause this function exists to paper
  // over: without it, BinaryFunction::buildCFG()'s fallthrough-successor
  // logic for blocks with zero *branch* successors
  // (`IsPrevFT = !MIB->isTerminator(*LastInstr) ||
  // MIB->getConditionalTailCall(*LastInstr);` for the BB->succ_size() == 0
  // case) sees isTerminator()==true (correctly -- see isTerminator()'s
  // comment above) and getConditionalTailCall()==false, so it concludes
  // IsPrevFT == false and never adds the fallthrough edge into the
  // following block. The next block then looks unreachable from this one
  // and, once physical block adjacency is disturbed by any layout pass
  // (confirmed via -reorder-blocks=reverse, though the same corruption is
  // silently latent under every other layout mode too since the CFG model
  // itself is wrong, not just its printed layout), the not-taken path's
  // real target is missing from the CFG entirely: BOLT emits nothing there,
  // or worse emits an unrelated block, and the not-taken branch at runtime
  // falls into whatever finalize-functions happened to place next --
  // observed as a SIGSEGV inside unrelated PPC64 std/ld sequences whose
  // register state came from a completely different, structurally-unrelated
  // basic block (llvm::MachineRegisterInfo::moveOperands()'s "bdzlr; ...
  // loop body ..." idiom, reduced from a 65000-function binary via bisection
  // down to this exact instruction pattern).
  //
  // The three families, all "terminator, isReturn=1 in the .td, implicit
  // LR target, BO field determines taken/not-taken":
  //  - BCLR/BCLRn (explicit 5-bit BO field, "bclr 12/4, $BI, 0")
  //  - BCCLR (extended mnemonics beqlr/bnelr/bltlr/bgtlr/blelr/bgelr/...,
  //    by far the most common in real PPC64 binaries -- e.g. ~5900
  //    occurrences vs. bdzlr's ~20 in a typical `llc` binary)
  //  - gBCLR (the disassembler's generic "bclr $BO, $BI, $BH" form)
  //  - BDZLR/BDNZLR and their probability-hint (+/-) variants (decrement
  //    CTR, branch-to-LR if [not] zero -- the counted-loop-with-early-return
  //    idiom; this is the specific family the bisection above landed on)
  //
  // Deliberately NOT included: the "L"-suffixed link-setting siblings
  // (BCLRL/BCCLRL/gBCLRL/BDZLRL/BDNZLRL) — those write a new return address
  // into LR on the taken path, making the taken path an indirect call
  // through LR rather than a return, so they belong under isCall()'s
  // indirect-call handling instead (not currently implemented either, but
  // out of scope here: no compiler-generated code in the test corpus emits
  // a conditional indirect call, unlike the four families above which are
  // all common, naturally-occurring compiler output).
  // Note on gBCLR: its BO field is a runtime 5-bit immediate rather than a
  // fixed encoding, so it could in principle encode "branch always" (BO=20,
  // equivalent to plain blr) if some non-compiler-generated code hand-encoded
  // it that way instead of using the dedicated BLR opcode. Compiler output
  // never does this (BLR's InstAlias always wins for BO=20), so this
  // theoretical case is out of scope. Even if it occurred, misclassifying an
  // always-taken branch as conditional only adds a spurious, never-executed
  // fallthrough edge to the CFG -- comparatively harmless next to this
  // function's actual purpose of not silently dropping a genuinely-taken
  // fallthrough edge.
  switch (opc(Inst)) {
  case PPC::BCLR:
  case PPC::BCLRn:
  case PPC::BCCLR:
  case PPC::gBCLR:
  case PPC::BDZLR:
  case PPC::BDNZLR:
  case PPC::BDZLRp:
  case PPC::BDNZLRp:
  case PPC::BDZLRm:
  case PPC::BDNZLRm:
    return true;
  default:
    return false;
  }
}

bool PPCMCPlusBuilder::isTerminator(const MCInst &Inst) const {
  // The base class implementation uses MCInstrAnalysis::isTerminator(), which
  // relies on the MCInstrDesc::isTerminator() bit.  PPC's gBC/gBCL (the MC
  // forms of the generic conditional branch, used by the disassembler) are
  // missing the Terminator bit in their MCInstrDesc (they only have Branch).
  // Without this override, BOLT never splits basic blocks at gBC instructions,
  // causing it to treat the entire body of functions like
  // _GLOBAL__sub_I_*.cpp as a single 100+ instruction BB.
  //
  // We manually mark every instruction that isBranch() recognises as a
  // terminator, EXCEPT for call instructions (BL/BLA/BL8 etc.) which are
  // branches-with-link (calls) and should not terminate the basic block.
  // Call instructions have their own handling in BOLT's CFG builder.
  if ((isBranch(Inst) || isReturn(Inst)) && !isCall(Inst))
    return true;
  return MCPlusBuilder::isTerminator(Inst);
}

bool PPCMCPlusBuilder::isConditionalBranch(const MCInst &I) const {
  switch (opc(I)) {
  case PPC::BC:    // branch conditional (explicit BO,BI fields)
  case PPC::BCL:   // branch conditional with link
  case PPC::BCC:   // extended-mnemonic conditional branch (bt/bf/beq/bne/bgt...)
  case PPC::BCCA:  // extended-mnemonic conditional branch absolute
  case PPC::BCCL:  // extended-mnemonic conditional branch with link
  case PPC::BCCLA: // extended-mnemonic conditional branch with link absolute
  case PPC::gBC:   // generic conditional branch (bt/bf with full BO field)
  case PPC::gBCL:  // generic conditional branch with link
  // BDNZ/BDNZL ("decrement CTR and branch if nonzero") are the standard
  // PPC64 counted-loop backedge instructions emitted for hand-inlined or
  // vectorizer-generated fixed-trip-count copy loops (e.g. the SmallVector
  // growth pattern in every C++ global constructor's push_back calls).
  // They branch on the CTR register being nonzero and otherwise FALL
  // THROUGH to the next instruction -- i.e. they behave exactly like a
  // two-way conditional branch (taken = loop backedge, not-taken =
  // fallthrough to loop-exit code), and getPCRelEncodingSize()/
  // getPCRelOperandNum() already treat them that way for relocation and
  // stub-insertion purposes.
  //
  // Before this fix they were missing here, so BinaryFunction::buildCFG()'s
  // fallthrough-successor-edge logic
  // (`IsPrevFT = MIB->isConditionalBranch(*LastInstr);` for
  // `succ_size() == 1` blocks) treated a block ending in bdnz as having NO
  // fallthrough successor. The single-instruction loop-exit block right
  // after the bdnz (which had zero *taken*-branch predecessors, since the
  // taken edge is the loop backedge) then looked unreachable and was
  // deleted by the eliminate-unreachable pass. With the loop-exit block
  // gone, bdnz's only remaining successor was the loop head itself, so
  // BinaryFunction::fixBranches() "helpfully" appended an unconditional
  // `b <loop-head>` right after the bdnz to reach that lone successor.
  // At runtime, whenever CTR naturally reached zero and bdnz fell
  // through (the loop's normal exit), execution hit the injected
  // unconditional branch instead and re-entered the copy loop for one
  // extra iteration -- reading one element past the end of the old buffer
  // and writing one element past the end of the newly allocated buffer.
  // This matches the exact off-by-one heap overread/overwrite Valgrind
  // reported in _GLOBAL__sub_I_LoopInterchange.cpp's inlined
  // SmallVector::push_back growth code.
  //
  // The bug applied to the WHOLE BDNZ/BDZ family, not just BDNZ/BDNZL:
  // PPCBranchSelector.cpp's out-of-range-branch expansion rewrites bdnz to
  // its "opposite" mnemonic bdz (and vice versa, plus the *8 64-bit-mode
  // pseudo forms) when it needs to jump over an inserted long branch (see
  // PPCBranchSelector.cpp's runOnMachineFunction), so bdz is exactly as
  // likely to appear in a real PPC64 binary as bdnz -- and was previously
  // NOT recognized as a conditional branch at all, meaning a block ending
  // in bdz got the exact same silent-corruption treatment described above.
  case PPC::BDNZ:
  case PPC::BDNZL:
  case PPC::BDNZA:
  case PPC::BDNZLA:
  case PPC::BDNZp:
  case PPC::BDNZLp:
  case PPC::BDNZAp:
  case PPC::BDNZLAp:
  case PPC::BDNZm:
  case PPC::BDNZLm:
  case PPC::BDNZAm:
  case PPC::BDNZLAm:
  case PPC::BDZ:
  case PPC::BDZL:
  case PPC::BDZA:
  case PPC::BDZLA:
  case PPC::BDZp:
  case PPC::BDZLp:
  case PPC::BDZAp:
  case PPC::BDZLAp:
  case PPC::BDZm:
  case PPC::BDZLm:
  case PPC::BDZAm:
  case PPC::BDZLAm:
  case PPC::BDNZ8:
  case PPC::BDZ8:
    return true;
  default:
    return false;
  }
}

bool PPCMCPlusBuilder::isUnconditionalBranch(const MCInst &I) const {
  // Exclude tail calls here, mirroring the base MCPlusBuilder behavior
  // (isUnconditionalBranch() there is "Analysis->isUnconditionalBranch(Inst)
  // && !isTailCall(Inst)"). Without this exclusion, a tail-call branch
  // (e.g. the plain 'b <plt_call>' at the end of _start, or any other
  // direct-branch tail call produced by the compiler) is indistinguishable
  // here from a normal intra-function unconditional branch: analyzeBranch()
  // sets UncondBr with a target outside this function, but the block
  // legitimately has zero local CFG successors (it exits via tail call),
  // so BinaryBasicBlock::validateSuccessorInvariants() fails its
  // Successors.size()==0 case (which requires UncondBr to be null) and
  // postProcessBranches()'s validateCFG() assertion aborts BOLT.
  if (isTailCall(I))
    return false;
  switch (opc(I)) {
  case PPC::B:    // branch
  case PPC::BA:   // absolute branch
  case PPC::BCTR: // branch to CTR (no link) – often tail call
  case PPC::BCLR: // branch to LR  (no link)
    return true;
  default:
    return false;
  }
}

bool PPCMCPlusBuilder::isReversibleBranch(const MCInst &I) const {
  // NOTE: no PPC64 conditional branch is actually reversible today -- see
  // the "default:" case below. BC/BCC/gBC (and their linked forms) encode
  // the branch condition in a BO/CR-bit style operand that COULD in
  // principle be flipped via reverseBranchCondition()/getInvertedCondCode()
  // if PPCMCPlusBuilder implemented them, but it doesn't yet, so this
  // function must not report them as reversible.
  //
  // BDNZ/BDZ (and the rest of the family) are also conditional branches
  // (see isConditionalBranch() above), but their "condition" is implicit in
  // the opcode itself: the opposite-sense branch is a genuinely different
  // instruction, e.g. BDZ/BDZL ("decrement CTR and branch if ZERO") for
  // BDNZ/BDNZL, not a variant of the same instruction with a different
  // immediate/CR-bit operand. PPC's MCPlusBuilder does not implement
  // reverseBranchCondition()/getCondCode() for this family, so the base
  // class's reverseBranchCondition() would hit
  // `llvm_unreachable("not implemented")` if
  // BinaryFunction::fixBranches()'s "swap successors to avoid an extra
  // unconditional branch" optimization ever tried to invert one (now that
  // isConditionalBranch() reports the whole family as conditional, that
  // code path is reachable for all of them, not just BDNZ/BDNZL). Returning
  // false here makes fixBranches() take its safe fallback -- leave the
  // branch's sense alone and materialize an explicit unconditional branch
  // for the non-fallthrough successor instead of trying to reverse the
  // condition.
  switch (opc(I)) {
  case PPC::BDNZ:
  case PPC::BDNZL:
  case PPC::BDNZA:
  case PPC::BDNZLA:
  case PPC::BDNZp:
  case PPC::BDNZLp:
  case PPC::BDNZAp:
  case PPC::BDNZLAp:
  case PPC::BDNZm:
  case PPC::BDNZLm:
  case PPC::BDNZAm:
  case PPC::BDNZLAm:
  case PPC::BDZ:
  case PPC::BDZL:
  case PPC::BDZA:
  case PPC::BDZLA:
  case PPC::BDZp:
  case PPC::BDZLp:
  case PPC::BDZAp:
  case PPC::BDZLAp:
  case PPC::BDZm:
  case PPC::BDZLm:
  case PPC::BDZAm:
  case PPC::BDZLAm:
  case PPC::BDNZ8:
  case PPC::BDZ8:
    return false;
  default:
    // BC/BCC/gBC and their linked/absolute forms reach here. In principle
    // their BO/CR-bit condition operand could be flipped in place (that is
    // what the comment above this function describes as the intended
    // long-term design), but PPCMCPlusBuilder does not yet implement
    // reverseBranchCondition()/getCondCode()/getInvertedCondCode() for any
    // PPC64 opcode -- those all still hit the base MCPlusBuilder's
    // llvm_unreachable("not implemented"). Falling through to
    // MCPlusBuilder::isReversibleBranch(I) here (which just checks
    // isDynamicBranch() and otherwise returns true) was therefore a lie:
    // it told fixBranches()'s "swap successors to avoid an extra
    // unconditional branch" optimization that reversal was safe, and it
    // would crash the moment that optimization actually tried it. Return
    // false unconditionally until real reversal support is implemented,
    // matching the safe fallback already used above for the BDNZ/BDZ
    // family -- fixBranches() will materialize an explicit unconditional
    // branch for the non-fallthrough successor instead.
    return false;
  }
}

// Disable “conditional tail call” path for now.
const MCInst *PPCMCPlusBuilder::getConditionalTailCall(const MCInst &) const {
  return nullptr;
}

bool PPCMCPlusBuilder::isPICJumpTableBctr(const MCInst &Instruction,
                                          InstructionIterator Begin,
                                          InstructionIterator End) const {
  // Detect the PPC64 ELFv2 GCC PIC switch pattern:
  //   lwax  rDst, rBase, rIndex   (signed 32-bit table entry load)
  //   ...
  //   mtctr rDst
  //   bctr                        <- Instruction
  if (Instruction.getOpcode() != PPC::BCTR)
    return false;

  bool FoundMtctr = false;
  for (auto It = End; It != Begin;) {
    --It;
    const MCInst &Prev = *It;
    if (&Prev == &Instruction)
      continue;
    if (!FoundMtctr) {
      if (Prev.getOpcode() == PPC::MTCTR8 || Prev.getOpcode() == PPC::MTCTR) {
        FoundMtctr = true;
        continue;
      }
      return false; // First instruction before bctr must be mtctr
    }
    if (Prev.getOpcode() == PPC::LWAX)
      return true;
  }
  return false;
}

IndirectBranchType PPCMCPlusBuilder::analyzeIndirectBranch(
    MCInst &Instruction, InstructionIterator Begin, InstructionIterator End,
    const unsigned PtrSize, MCInst *&MemLocInstrOut, unsigned &BaseRegNumOut,
    unsigned &IndexRegNumOut, int64_t &DispValueOut, const MCExpr *&DispExprOut,
    MCInst *&PCRelBaseOut, MCInst *&FixedEntryLoadInstr) const {
  // Initialize all output parameters to safe defaults.
  MemLocInstrOut = nullptr;
  BaseRegNumOut = 0;
  IndexRegNumOut = 0;
  DispValueOut = 0;
  DispExprOut = nullptr;
  PCRelBaseOut = nullptr;
  FixedEntryLoadInstr = nullptr;

  // On PPC64 ELFv2, GCC emits PIC-style switch jump tables with this pattern:
  //
  //   addis  r8, r2, offset@ha       ; base = TOC + table offset
  //   addi   r8, r8, offset@l        ;
  //   rldic  r9, r9, 2, 54           ; index <<= 2  (scale by sizeof(int))
  //   lwax   r9, r8, r9              ; load signed 32-bit table entry
  //   add    r9, r9, r8              ; entry += base  (PIC-relative delta)
  //   mtctr  r9                      ; move target into CTR
  //   bctr                           ; <-- Instruction (the bctr we're called
  //   on)
  //
  // The data words immediately following bctr are the jump table entries.
  // We scan backwards from End (exclusive) to find the mtctr, then the lwax.
  // If found, we return POSSIBLE_PIC_JUMP_TABLE so BOLT treats the
  // post-bctr data as a constant island (jump table) rather than code.

  // Instruction is the bctr. Scan backwards through the basic block.
  // End points one past bctr, so we start from the instruction before it.
  if (Begin == End)
    return IndirectBranchType::UNKNOWN;

  // Walk backwards looking for mtctr, then lwax.
  MCInst *MtCtrInstr = nullptr;
  MCInst *LwaxInstr = nullptr;

  // Use reverse iteration over [Begin, End).
  // End currently points past the bctr (i.e. past Instruction).
  // We want to scan instructions that precede bctr.
  auto It = End;
  while (It != Begin) {
    --It;
    MCInst &Prev = *It;

    // Skip the bctr itself.
    if (&Prev == &Instruction)
      continue;

    // Step 1: Find mtctr (MTCTR or MTCTR8) immediately before bctr.
    if (MtCtrInstr == nullptr) {
      if (Prev.getOpcode() == PPC::MTCTR8 || Prev.getOpcode() == PPC::MTCTR) {
        MtCtrInstr = &Prev;
        LLVM_DEBUG(dbgs() << "PPC analyzeIndirectBranch: found mtctr\n");
        continue;
      }
      // If the first non-bctr instruction is not mtctr, not our pattern.
      return IndirectBranchType::UNKNOWN;
    }

    // Step 2: Find lwax which loads the jump table entry.
    // lwax  rDst, rBase, rIndex  -- signed 32-bit load indexed
    if (LwaxInstr == nullptr) {
      if (Prev.getOpcode() == PPC::LWAX) {
        LwaxInstr = &Prev;
        MemLocInstrOut = LwaxInstr;
        // Operand layout for LWAX: dst, base, index
        if (LwaxInstr->getNumOperands() >= 3) {
          BaseRegNumOut = LwaxInstr->getOperand(1).getReg();
          IndexRegNumOut = LwaxInstr->getOperand(2).getReg();
        }
        LLVM_DEBUG(dbgs() << "PPC analyzeIndirectBranch: found lwax, "
                          << "base=" << BaseRegNumOut
                          << " index=" << IndexRegNumOut << "\n");
        // Found enough to identify the pattern.
        return IndirectBranchType::POSSIBLE_PIC_JUMP_TABLE;
      }
      // Allow a few intervening instructions (add, rldic, etc.) before lwax.
      continue;
    }
  }

  return IndirectBranchType::UNKNOWN;
}

bool PPCMCPlusBuilder::isNoop(const MCInst &Inst) const {
  // PPC NOP can appear as two opcode forms:
  // 1. PPC::NOP  - the dedicated NOP pseudo-instruction (decoded from 0x60000000)
  // 2. PPC::ORI r0, r0, 0 - the underlying encoding (emitted by createNoop)
  LLVM_DEBUG(dbgs() << "isNoop check: opcode=" << Inst.getOpcode()
                    << " PPC::NOP=" << PPC::NOP
                    << " PPC::ORI=" << PPC::ORI << "\n");
  if (Inst.getOpcode() == PPC::NOP) {
    LLVM_DEBUG(dbgs() << "PPC-ISNOOP: opcode=" << Inst.getOpcode()
                      << " == PPC::NOP(" << PPC::NOP << ") -> TRUE\n");
    return true;
  }
  bool oriMatch = Inst.getOpcode() == PPC::ORI && Inst.getOperand(0).isReg() &&
         Inst.getOperand(0).getReg() == PPC::R0 && Inst.getOperand(1).isReg() &&
         Inst.getOperand(1).getReg() == PPC::R0 && Inst.getOperand(2).isImm() &&
         Inst.getOperand(2).getImm() == 0;
  LLVM_DEBUG(if (oriMatch) dbgs() << "PPC-ISNOOP: opcode=" << Inst.getOpcode()
                                  << " == ORI r0,r0,0 -> TRUE\n");
  return oriMatch;
}

void PPCMCPlusBuilder::createNoop(MCInst &Nop) const {
  Nop.clear();
  Nop.setOpcode(PPC::ORI);
  Nop.addOperand(MCOperand::createReg(PPC::R0)); // dst
  Nop.addOperand(MCOperand::createReg(PPC::R0)); // src
  Nop.addOperand(MCOperand::createImm(0));       // imm
}

void PPCMCPlusBuilder::createReturn(MCInst &Inst) const {
  // On PPC64 ELFv2, the standard function return is 'blr' (branch to LR).
  Inst.clear();
  Inst.setOpcode(PPC::BLR);
}

void PPCMCPlusBuilder::createUncondBranch(MCInst &Inst, const MCSymbol *TBB,
                                          MCContext *Ctx) const {
  // Emit a direct unconditional branch: b <target>
  // Operand 0 is the target symbol expression (PC-relative, ±32MB).
  // If the target is out of range, LongJmpPass will relax this stub to a
  // full 7-instruction absolute sequence via createLongJmp().
  Inst.clear();
  Inst.setOpcode(PPC::B);
  Inst.addOperand(MCOperand::createExpr(MCSymbolRefExpr::create(TBB, *Ctx)));
}

bool PPCMCPlusBuilder::analyzeBranch(InstructionIterator Begin,
                                     InstructionIterator End,
                                     const MCSymbol *&Tgt,
                                     const MCSymbol *&Fallthrough,
                                     MCInst *&CondBr, MCInst *&UncondBr) const {
  Tgt = nullptr;
  Fallthrough = nullptr;
  CondBr = nullptr;
  UncondBr = nullptr;

  // An empty block has no terminator: it simply falls through to its
  // successor. This IS analyzable (not an error case).
  if (Begin == End)
    return true;

  // Look at the last instruction, skipping trailing pseudo instructions
  // (CFI directives, debug-info annotations, etc). Functions with dense
  // .eh_frame unwind info -- observed in clang's C++ static-initializer
  // functions, which carry far more CFI than anything in llc's output --
  // can have a block's real terminator followed by several CFI pseudo-ops
  // (e.g. `bf 2, .LtmpA; b .LtmpB; !CFI OpRestore ...` x3). Without
  // skipping them, `--I` from `End` lands on a CFI pseudo-op instead of
  // the real branch; none of the branch-classification checks below match
  // a CFI instruction, so this function would silently fall through to
  // the "not a terminator, plain fallthrough" case at the bottom and
  // report zero successors -- while BinaryFunction::buildCFG() (which
  // correctly calls getLastNonPseudo() when building the CFG) had already
  // assigned two real successors to this same block. That mismatch is
  // exactly what BinaryBasicBlock::validateSuccessorInvariants()'s
  // 2-successor case flags as an invalid CFG, crashing
  // postProcessBranches()'s validateCFG() assertion. X86MCPlusBuilder's
  // analyzeBranch() already skips pseudo instructions the same way; PPC's
  // never did.
  InstructionIterator I = End;
  while (I != Begin) {
    --I;
    if (!isPseudo(*I))
      break;
  }
  if (isPseudo(*I)) {
    // The block contains no real instructions at all (only pseudo-ops) --
    // treat it like an empty block: analyzable, plain fallthrough.
    return true;
  }
  const MCInst &Last = *I;

  // Return (blr) -> no branch terminator, no successors. Analyzable.
  if (Last.getOpcode() == PPC::BLR) {
    return true;
  }

  // Indirect branches (bctr/bclr and their linked forms) have no statically
  // known target symbol -- genuinely unanalyzable here.
  if (isIndirectBranch(Last))
    return false;

  if (isUnconditionalBranch(Last)) {
    // Check for a preceding conditional branch first: the canonical
    // "bt cond, TargetA; b TargetB" two-instruction terminator idiom.
    // BinaryBasicBlock::validateSuccessorInvariants()'s 2-successor case
    // requires Tgt/TBB to be the CONDITIONAL branch's target and
    // Fallthrough/FBB to be the UNCONDITIONAL branch's target (the
    // unconditional branch stands in for the missing fallthrough here).
    // Without this, a block ending in cond-branch+uncond-branch only ever
    // reports the uncond branch as Tgt with no Fallthrough, which fails
    // validateCFG() once getTargetSymbol() (below) correctly resolves a
    // non-null target -- previously this path was masked because the old
    // last-operand lookup often returned null and the whole block was
    // treated as unanalyzable.
    //
    // As above, skip any pseudo instructions between the unconditional
    // branch and the conditional branch that precedes it -- the same CFI
    // gap that motivates skipping pseudo-ops when finding `Last` can in
    // principle separate the two real terminator instructions too.
    if (I != Begin) {
      InstructionIterator Prev = I;
      while (Prev != Begin) {
        --Prev;
        if (!isPseudo(*Prev))
          break;
      }
      if (!isPseudo(*Prev)) {
        const MCInst &SecondLast = *Prev;
        if (isConditionalBranch(SecondLast)) {
          const MCSymbol *CondTgt = getTargetSymbol(SecondLast);
          const MCSymbol *UncondTgt = getTargetSymbol(Last);
          if (CondTgt && UncondTgt) {
            Tgt = CondTgt;
            Fallthrough = UncondTgt;
            CondBr = const_cast<MCInst *>(&SecondLast);
            UncondBr = const_cast<MCInst *>(&Last);
            return true;
          }
        }
      }
    }

    // Plain single unconditional branch, no fallthrough. Use
    // getTargetSymbol() (which resolves the target via
    // getPCRelOperandNum()'s fixed, per-opcode operand index) instead of
    // grabbing "the last MCOperand" -- see the root-cause comment above
    // getTargetSymbol()'s definition for why the naive last-operand lookup
    // silently breaks once BOLT appends annotation operands (e.g. execution
    // count/misprediction-count MCAnnotation operands added during/after
    // profile attachment) after the real target operand.
    Tgt = getTargetSymbol(Last);
    if (!Tgt)
      return false;
    UncondBr = const_cast<MCInst *>(&Last);
    return true;
  }

  if (isConditionalBranch(Last)) {
    // See the comment above the isUnconditionalBranch case: use
    // getTargetSymbol(), not a last-operand lookup, for the same reason.
    Tgt = getTargetSymbol(Last);
    if (!Tgt)
      return false;
    CondBr = const_cast<MCInst *>(&Last);
    // Assume the block has a fallthrough if no following unconditional branch.
    // (BOLT will compute actual fallthrough later once CFG is built.)
    return true;
  }

  // Otherwise: the block's last instruction is not a branch terminator at
  // all (e.g. it ends in a call, or in a plain non-control-flow instruction
  // such as the TOC-restore `nop` slot after a call). This is a normal,
  // analyzable fallthrough block -- callers such as
  // BinaryFunction::fixBranches() rely on `true` here to know it is safe to
  // append a corrective unconditional branch if the block's fallthrough
  // successor is no longer the next block in the final layout. Returning
  // `false` in this case (as this function previously did) silently
  // disabled fixBranches() for every PPC64 function, since it treats a
  // `false` return as "unanalyzable, leave alone" and skips the block
  // entirely -- including the `succ_size() == 1` case that inserts the
  // missing branch. That allowed a call-ending block's fallthrough to
  // become stale/incorrect after any layout change, silently corrupting
  // control flow (observed as a runtime crash in
  // llvm::cl::Option::addArgument() after a call to RegisterManagedStatic).
  return true;
}

bool PPCMCPlusBuilder::lowerTailCall(MCInst &Inst) { return false; }

uint64_t PPCMCPlusBuilder::analyzePLTEntry(MCInst &Instruction,
                                           InstructionIterator Begin,
                                           InstructionIterator End,
                                           uint64_t BeginPC) const {
  (void)Instruction;
  (void)Begin;
  (void)End;
  (void)BeginPC;
  return 0;
}

void PPCMCPlusBuilder::createLongTailCall(std::vector<MCInst> &Seq,
                                          const MCSymbol *Target,
                                          MCContext *Ctx) {
  Seq.clear();

  // --- Absolute 64-bit materialization of Target into r12 (no TOC/r2) ---
  // r12 = Target, assembled from four 16-bit pieces via logical ORs.
  const unsigned R12 = PPC::X12;

  const MCExpr *HST =
      MCSymbolRefExpr::create(Target, PPC::S_HIGHEST, *Ctx); // bits 48..63
  const MCExpr *HER =
      MCSymbolRefExpr::create(Target, PPC::S_HIGHER, *Ctx); // bits 32..47
  const MCExpr *HI =
      MCSymbolRefExpr::create(Target, PPC::S_HI, *Ctx); // bits 16..31  (@h)
  const MCExpr *LO =
      MCSymbolRefExpr::create(Target, PPC::S_LO, *Ctx); // bits 0..15   (@l)

  MCInst I;

  // lis    r12, Target@highest         ; r12 = highest << 16
  I = MCInst();
  I.setOpcode(PPC::LIS8);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createExpr(HST));
  Seq.push_back(I);

  // ori    r12, r12, Target@higher     ; r12 |= higher
  I = MCInst();
  I.setOpcode(PPC::ORI8);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createExpr(HER));
  Seq.push_back(I);

  // rldicr r12, r12, 32, 31            ; shift the top 32 bits up
  I = MCInst();
  I.setOpcode(PPC::RLDICR);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createImm(32)); // shift amount
  I.addOperand(MCOperand::createImm(
      31)); // mask end (MB..ME semantics from PPCInstrInfo.cpp:3470)
  Seq.push_back(I);

  // oris   r12, r12, Target@h          ; r12 |= (high << 16)
  I = MCInst();
  I.setOpcode(PPC::ORIS8);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createExpr(HI));
  Seq.push_back(I);

  // ori    r12, r12, Target@l          ; r12 |= low
  I = MCInst();
  I.setOpcode(PPC::ORI8);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createExpr(LO));
  Seq.push_back(I);
  // --- r12 now holds the absolute address of Target ---

  // mtctr r12                    ; move target into CTR
  I = MCInst();
  I.setOpcode(PPC::MTCTR8);
  I.addOperand(MCOperand::createReg(R12));
  Seq.push_back(I);

  // bctr                         ; tail-call: branch to CTR, NO link
  I = MCInst();
  I.setOpcode(PPC::BCTR); // NOT BCTRL — tail call does not link
  Seq.push_back(I);
}

using namespace llvm::ELF;

std::optional<Relocation>
PPCMCPlusBuilder::createRelocation(const MCFixup &Fixup,
                                   const MCAsmBackend &MAB) const {
  Relocation R;
  R.Offset = Fixup.getOffset();

  // Extract (Symbol, Addend) from the fixup expression.
  auto [RelSymbol, RelAddend] = extractFixupExpr(Fixup);
  if (!RelSymbol)
    return std::nullopt;

  R.Symbol = const_cast<MCSymbol *>(RelSymbol);

  // PPC64 ELFv2: the generic MCFixupKindInfo name (e.g. "fixup_ppc_half16")
  // is IDENTICAL for every symbol-modifier variant of a half16 relocation
  // (@l, @ha, @high, @higha, @higher, @highera, @highest, @highesta) --
  // upstream PPCELFObjectWriter.cpp distinguishes them by inspecting the
  // MCSymbolRefExpr's *specifier* (PPC::S_LO, S_HA, S_HIGHEST, ...), not the
  // fixup kind name.  createRelocation() below only looked at the fixup kind
  // name, so every one of these variants fell through to the same generic
  // "TargetSize==16 -> R_PPC64_ADDR16_LO" fallback -- silently corrupting any
  // multi-instruction absolute address sequence (e.g. LongJmpPass's 7-
  // instruction PPC64 long-jump: lis/ori/rldicr/oris/ori@highest/higher/hi/lo)
  // by writing the LOW 16 bits into every immediate field regardless of which
  // part of the address it was supposed to hold.  This manifested as PPC64
  // long-jump stubs branching to a garbage address (SIGSEGV) at runtime.
  //
  // Fix: find the innermost MCSymbolRefExpr in the fixup's value expression
  // and check its specifier directly, before falling back to the generic
  // by-name/by-size heuristics.
  auto FindSpecifier = [](const MCExpr *E) -> std::optional<uint16_t> {
    while (E) {
      if (E->getKind() == MCExpr::SymbolRef)
        return cast<MCSymbolRefExpr>(E)->getSpecifier();
      if (E->getKind() == MCExpr::Binary) {
        // Addend expressions are Sym + Const (see extractFixupExpr); the
        // symbol side is whichever operand is not a plain constant.
        const auto *BE = cast<MCBinaryExpr>(E);
        if (BE->getLHS()->getKind() != MCExpr::Constant) {
          E = BE->getLHS();
          continue;
        }
        E = BE->getRHS();
        continue;
      }
      return std::nullopt;
    }
    return std::nullopt;
  };

  if (std::optional<uint16_t> Spec = FindSpecifier(Fixup.getValue())) {
    switch (*Spec) {
    case PPC::S_LO:
      R.Type = ELF::R_PPC64_ADDR16_LO;
      return R;
    case PPC::S_HI:
      R.Type = ELF::R_PPC64_ADDR16_HI;
      return R;
    case PPC::S_HA:
      R.Type = ELF::R_PPC64_ADDR16_HA;
      return R;
    case PPC::S_HIGH:
      R.Type = ELF::R_PPC64_ADDR16_HIGH;
      return R;
    case PPC::S_HIGHA:
      R.Type = ELF::R_PPC64_ADDR16_HIGHA;
      return R;
    case PPC::S_HIGHER:
      R.Type = ELF::R_PPC64_ADDR16_HIGHER;
      return R;
    case PPC::S_HIGHERA:
      R.Type = ELF::R_PPC64_ADDR16_HIGHERA;
      return R;
    case PPC::S_HIGHEST:
      R.Type = ELF::R_PPC64_ADDR16_HIGHEST;
      return R;
    case PPC::S_HIGHESTA:
      R.Type = ELF::R_PPC64_ADDR16_HIGHESTA;
      return R;
    default:
      break; // Not a half16-family specifier; fall through to name matching.
    }
  }

  const MCFixupKind Kind = Fixup.getKind();
  const MCFixupKindInfo FKI = MAB.getFixupKindInfo(Kind);
  llvm::StringRef Name = FKI.Name;

  // Make a lowercase copy for case-insensitive matching.
  std::string L = Name.lower();

  // Branch/call (24-bit) — BL/B
  if (Name.equals_insensitive("fixup_ppc_br24") ||
      Name.equals_insensitive("fixup_branch24") ||
      L.find("br24") != std::string::npos) {
    R.Type = ELF::R_PPC64_REL24;
    return R;
  }

  // Conditional branch (14-bit) — BC/BDNZ/…
  if (Name.equals_insensitive("fixup_ppc_brcond14") ||
      Name.equals_insensitive("fixup_branch14") ||
      L.find("br14") != std::string::npos ||
      L.find("cond14") != std::string::npos) {
    R.Type = ELF::R_PPC64_REL14;
    return R;
  }

  // DS-form low16 (implied 2 zero bits)
  if (Name.equals_insensitive("fixup_ppc_half16ds")) {
    R.Type = ELF::R_PPC64_ADDR16_LO_DS;
    return R;
  }
  // Generic half16 — in our stub we use it with ADDIS (HA)
  if (Name.equals_insensitive("fixup_ppc_half16")) {
    R.Type = ELF::R_PPC64_ADDR16_HA;
    return R;
  }
  if (Name.equals_insensitive("fixup_ppc_addr32") ||
      L.find("addr32") != std::string::npos) {
    R.Type = ELF::R_PPC64_ADDR32;
    return R;
  }
  if (Name.equals_insensitive("fixup_ppc_addr64") ||
      L.find("addr64") != std::string::npos) {
    R.Type = ELF::R_PPC64_ADDR64;
    return R;
  }

  // TOC-related (match loosely)
  if (L.find("toc16_lo") != std::string::npos) {
    R.Type = ELF::R_PPC64_TOC16_LO;
    return R;
  }
  if (L.find("toc16_ha") != std::string::npos) {
    R.Type = ELF::R_PPC64_TOC16_HA;
    return R;
  }
  if (Name.equals_insensitive("fixup_ppc_toc") ||
      L.find("toc16") != std::string::npos) {
    // Generic TOC16 fallback if needed
    R.Type = ELF::R_PPC64_TOC16;
    return R;
  }

  if (L.find("toc16_lo_ds") != std::string::npos) {
    // TOC16_LO_DS can be optimized to R_GOTREL if tocOptimize is on
    R.Type = ELF::R_PPC64_TOC16_LO_DS;
    return R;
  }
  if (L.find("toc16_ds") != std::string::npos) {
    R.Type = ELF::R_PPC64_TOC16_DS;
    return R;
  }
  if (L.find("addr16_lo_ds") != std::string::npos) {
    R.Type = ELF::R_PPC64_ADDR16_LO_DS;
    return R;
  }
  if (L.find("addr16_ds") != std::string::npos) {
    R.Type = ELF::R_PPC64_ADDR16_DS;
    return R;
  }

  // --- Fallback heuristic: use PCRel + bit-size ---
  if (Fixup.isPCRel()) {
    switch (FKI.TargetSize) {
    case 24:
      R.Type = ELF::R_PPC64_REL24;
      return R;
    case 14:
      R.Type = ELF::R_PPC64_REL14;
      return R;
    default:
      break;
    }
  } else {
    switch (FKI.TargetSize) {
    case 16:
      R.Type = ELF::R_PPC64_ADDR16_LO;
      return R; // safest low-16 default
    case 32:
      R.Type = ELF::R_PPC64_ADDR32;
      return R;
    case 64:
      R.Type = ELF::R_PPC64_ADDR64;
      return R;
    default:
      break;
    }
  }

  LLVM_DEBUG(dbgs() << "PPC createRelocation: unhandled fixup kind '" << Name
                    << "', size=" << FKI.TargetSize
                    << ", isPCRel=" << Fixup.isPCRel() << "\n");
  return std::nullopt;
}

bool PPCMCPlusBuilder::isTOCRestoreAfterCall(const MCInst &I) const {
  LLVM_DEBUG({
    dbgs() << "TOC-RESTORE check: " << I.getOpcode() << " (";
    for (unsigned k = 0; k < I.getNumOperands(); ++k) {
      if (k)
        dbgs() << ", ";
      const auto &Op = I.getOperand(k);
      if (Op.isReg())
        dbgs() << Op.getReg();
      else if (Op.isImm())
        dbgs() << Op.getImm();
      else if (Op.isExpr())
        dbgs() << "expr";
      else
        dbgs() << "<op" << k << ">";
    }
    dbgs() << ")\n";
  });

  if (I.getOpcode() != PPC::LD)
    return false;

  auto isR1 = [](unsigned R) { return R == PPC::X1 || R == PPC::R1; };
  auto isR2 = [](unsigned R) { return R == PPC::X2 || R == PPC::R2; };

  // ld r2, 24(r1) can appear in two forms depending on whether the binary
  // was compiled with --emit-relocs:
  //
  // Without --emit-relocs (3 prime operands): dst=r2, offset=24(imm), base=r1
  // With --emit-relocs (2 prime operands):    dst=r2, addr=expr(24(r1))
  //   The symbolizer folds offset+base into a single memory expression.
  //
  // BOLT may attach annotation operands (Offset, NOP marker, etc.) beyond the
  // prime operands. Use MCPlus::getNumPrimeOperands() instead of
  // getNumOperands() so that annotation-decorated instructions are still
  // recognised correctly.
  //
  // Both forms represent the same TOC-restore instruction.

  if (!I.getOperand(0).isReg() || !isR2(I.getOperand(0).getReg()))
    return false;

  const unsigned NumPrime = MCPlus::getNumPrimeOperands(I);

  if (NumPrime == 2) {
    // With --emit-relocs: operand 1 is a symbolized memory expression.
    // Any ld r2, expr form after a call is a TOC-restore.
    return I.getOperand(1).isExpr();
  }

  // Without --emit-relocs: 3-operand form (dst, offset_imm, base_reg)
  if (NumPrime == 3) {
    const MCOperand &OffOp = I.getOperand(1);
    if (OffOp.isImm()) {
      if (OffOp.getImm() != 24)
        return false;
    } else if (!OffOp.isExpr()) {
      return false;
    }
    return I.getOperand(2).isReg() && isR1(I.getOperand(2).getReg());
  }

  return false;
}

static inline MCOperand R(unsigned Reg) { return MCOperand::createReg(Reg); }

void PPCMCPlusBuilder::buildCallStubGOTSlot(MCContext *Ctx,
                                            uint64_t GotSlotAddress,
                                            std::vector<MCInst> &Out) const {
  Out.clear();
  // Materialize GotSlotAddress into r11 (scratch; doesn't clobber r12 so the
  // ELFv2 convention of r12 = entry address is preserved for the final bctr).
  const unsigned R11 = PPC::X11;
  const unsigned R12 = PPC::X12;

  // Split the 64-bit address into four 16-bit pieces.
  // Note: use plain bit-extraction (no carry adjustment) because we use
  // logical oris/ori instructions which do NOT sign-extend operands.
  uint64_t Addr = GotSlotAddress;
  uint16_t Highest = (Addr >> 48) & 0xffff;
  uint16_t Higher  = (Addr >> 32) & 0xffff;
  uint16_t Lo  = Addr & 0xffff;
  uint16_t Hi  = (Addr >> 16) & 0xffff;

  MCInst I;

  // std r2, 24(r1)   ; save caller's TOC (r2) so ld r2,24(r1) after the call
  //                  ; restores it correctly, even if the callee (e.g. getenv)
  //                  ; makes further calls that clobber 24(r1).
  I = MCInst();
  I.setOpcode(PPC::STD);
  I.addOperand(R(PPC::X2));
  I.addOperand(MCOperand::createImm(24));
  I.addOperand(R(PPC::X1));
  Out.push_back(I);

  // lis r11, Highest
  I = MCInst();
  I.setOpcode(PPC::LIS8);
  I.addOperand(MCOperand::createReg(R11));
  I.addOperand(MCOperand::createImm((int16_t)Highest));
  Out.push_back(I);

  // ori r11, r11, Higher
  I = MCInst();
  I.setOpcode(PPC::ORI8);
  I.addOperand(MCOperand::createReg(R11));
  I.addOperand(MCOperand::createReg(R11));
  I.addOperand(MCOperand::createImm(Higher));
  Out.push_back(I);

  // rldicr r11, r11, 32, 31
  I = MCInst();
  I.setOpcode(PPC::RLDICR);
  I.addOperand(MCOperand::createReg(R11));
  I.addOperand(MCOperand::createReg(R11));
  I.addOperand(MCOperand::createImm(32));
  I.addOperand(MCOperand::createImm(31));
  Out.push_back(I);

  // oris r11, r11, Hi
  I = MCInst();
  I.setOpcode(PPC::ORIS8);
  I.addOperand(MCOperand::createReg(R11));
  I.addOperand(MCOperand::createReg(R11));
  I.addOperand(MCOperand::createImm(Hi));
  Out.push_back(I);

  // ori r11, r11, Lo
  I = MCInst();
  I.setOpcode(PPC::ORI8);
  I.addOperand(MCOperand::createReg(R11));
  I.addOperand(MCOperand::createReg(R11));
  I.addOperand(MCOperand::createImm(Lo));
  Out.push_back(I);

  // ld r12, 0(r11)   ; load callee address from .plt GOT slot
  I = MCInst();
  I.setOpcode(PPC::LD);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createImm(0));
  I.addOperand(MCOperand::createReg(R11));
  Out.push_back(I);

  // mtctr r12
  I = MCInst();
  I.setOpcode(PPC::MTCTR8);
  I.addOperand(MCOperand::createReg(R12));
  Out.push_back(I);

  // bctrl            ; call callee; LR = return address back into this stub
  I = MCInst();
  I.setOpcode(PPC::BCTRL8);
  Out.push_back(I);

  // ld r2, 24(r1)    ; restore caller's TOC after the call
  I = MCInst();
  I.setOpcode(PPC::LD);
  I.addOperand(R(PPC::X2));
  I.addOperand(MCOperand::createImm(24));
  I.addOperand(R(PPC::X1));
  Out.push_back(I);

  // blr              ; return to original caller
  I = MCInst();
  I.setOpcode(PPC::BLR8);
  Out.push_back(I);
}

void PPCMCPlusBuilder::buildCallStubTOCThunk(MCContext *Ctx,
                                              uint64_t ThunkAddress,
                                              uint64_t OrigTOCBase,
                                              std::vector<MCInst> &Out) const {
  Out.clear();
  // Helper lambda: materialize a 64-bit immediate into a GPR using the
  // lis/ori/rldicr/oris/ori sequence.
  // Note: plain bit-extraction, NO carry adjustment — oris/ori are logical
  // and do not sign-extend, so no carry from bit 15 is needed.
  auto mat64 = [&](unsigned Reg, uint64_t Val) {
    uint16_t Highest = (Val >> 48) & 0xffff;
    uint16_t Higher  = (Val >> 32) & 0xffff;
    uint16_t Lo  = Val & 0xffff;
    uint16_t Hi  = (Val >> 16) & 0xffff;
    MCInst I;
    // lis Reg, Highest
    I = MCInst(); I.setOpcode(PPC::LIS8);
    I.addOperand(MCOperand::createReg(Reg));
    I.addOperand(MCOperand::createImm((int16_t)Highest));
    Out.push_back(I);
    // ori Reg, Reg, Higher
    I = MCInst(); I.setOpcode(PPC::ORI8);
    I.addOperand(MCOperand::createReg(Reg));
    I.addOperand(MCOperand::createReg(Reg));
    I.addOperand(MCOperand::createImm(Higher));
    Out.push_back(I);
    // rldicr Reg, Reg, 32, 31
    I = MCInst(); I.setOpcode(PPC::RLDICR);
    I.addOperand(MCOperand::createReg(Reg));
    I.addOperand(MCOperand::createReg(Reg));
    I.addOperand(MCOperand::createImm(32));
    I.addOperand(MCOperand::createImm(31));
    Out.push_back(I);
    // oris Reg, Reg, Hi
    I = MCInst(); I.setOpcode(PPC::ORIS8);
    I.addOperand(MCOperand::createReg(Reg));
    I.addOperand(MCOperand::createReg(Reg));
    I.addOperand(MCOperand::createImm(Hi));
    Out.push_back(I);
    // ori Reg, Reg, Lo
    I = MCInst(); I.setOpcode(PPC::ORI8);
    I.addOperand(MCOperand::createReg(Reg));
    I.addOperand(MCOperand::createReg(Reg));
    I.addOperand(MCOperand::createImm(Lo));
    Out.push_back(I);
  };

  // PPC64 ELFv2 ABI: the PLT thunk starts with "std r2, 24(r1)" which saves
  // the caller's TOC into the *caller's* stack frame at offset 24.  When our
  // stub calls the thunk via bctrl, r1 still points to our caller's frame, so
  // the thunk overwrites offset 24 of our caller's frame -- destroying the
  // BOLT TOC we saved there.
  //
  // Fix: allocate our own stack frame (stdu) so the thunk's std writes into
  // OUR frame's slot 24, and save the BOLT TOC at slot 32 of OUR frame where
  // the thunk will not touch it.
  //
  // Frame layout (offsets from new r1 after stdu):
  //   0  : back-chain (old r1)     [stdu writes this]
  //   16 : LR save area            (ELFv2 ABI requirement)
  //   24 : TOC save area           (thunk will overwrite this -- OK)
  //   32 : our BOLT TOC save       (we use this to restore r2 after return)
  //
  // Minimum ELFv2 frame size is 32 bytes; we use 48 to be safe and 16-byte
  // aligned (48 = 3 * 16).

  MCInst I;

  // mflr r0          ; save lr (so bctrl doesn't clobber caller's return addr)
  I = MCInst(); I.setOpcode(PPC::MFLR8);
  I.addOperand(MCOperand::createReg(PPC::X0));
  Out.push_back(I);

  // Allocate a 48-byte frame: std r1, -48(r1) + addi r1, r1, -48
  // (Use STD+ADDI8 instead of STDU to avoid STDU's complex memrix operand.)
  // std r1, -48(r1)  ; save back-chain
  I = MCInst(); I.setOpcode(PPC::STD);
  I.addOperand(R(PPC::X1));
  I.addOperand(MCOperand::createImm(-48));
  I.addOperand(R(PPC::X1));
  Out.push_back(I);

  // addi r1, r1, -48 ; update stack pointer
  I = MCInst(); I.setOpcode(PPC::ADDI8);
  I.addOperand(R(PPC::X1));
  I.addOperand(R(PPC::X1));
  I.addOperand(MCOperand::createImm(-48));
  Out.push_back(I);

  // std r0, 64(r1)   ; save lr at old_r1+16 = new_r1+48+16 = new_r1+64
  //                  ; (ELFv2: lr saved at caller_frame+16, which is new_r1+48+16)
  I = MCInst(); I.setOpcode(PPC::STD);
  I.addOperand(MCOperand::createReg(PPC::X0));
  I.addOperand(MCOperand::createImm(64));
  I.addOperand(R(PPC::X1));
  Out.push_back(I);

  // std r2, 32(r1)   ; save BOLT TOC at our private slot (thunk won't touch 32)
  I = MCInst(); I.setOpcode(PPC::STD);
  I.addOperand(R(PPC::X2));
  I.addOperand(MCOperand::createImm(32));
  I.addOperand(R(PPC::X1));
  Out.push_back(I);

  // r2 = original TOC base
  mat64(PPC::X2, OrigTOCBase);

  // r12 = thunk address
  mat64(PPC::X12, ThunkAddress);

  // mtctr r12
  I = MCInst(); I.setOpcode(PPC::MTCTR8);
  I.addOperand(MCOperand::createReg(PPC::X12));
  Out.push_back(I);

  // bctrl  ; call thunk with original r2; thunk does std r2,24(r1) (our slot),
  //        ; ld r12,N(r2), bctr to real fn; real fn returns here via blr
  I = MCInst(); I.setOpcode(PPC::BCTRL8);
  Out.push_back(I);

  // ld r2, 32(r1)    ; restore BOLT TOC from our private save slot
  I = MCInst(); I.setOpcode(PPC::LD);
  I.addOperand(R(PPC::X2));
  I.addOperand(MCOperand::createImm(32));
  I.addOperand(R(PPC::X1));
  Out.push_back(I);

  // ld r0, 64(r1)    ; reload saved lr
  I = MCInst(); I.setOpcode(PPC::LD);
  I.addOperand(MCOperand::createReg(PPC::X0));
  I.addOperand(MCOperand::createImm(64));
  I.addOperand(R(PPC::X1));
  Out.push_back(I);

  // mtlr r0          ; restore lr
  I = MCInst(); I.setOpcode(PPC::MTLR8);
  I.addOperand(MCOperand::createReg(PPC::X0));
  Out.push_back(I);

  // addi r1, r1, 48  ; deallocate frame
  I = MCInst(); I.setOpcode(PPC::ADDI8);
  I.addOperand(R(PPC::X1));
  I.addOperand(R(PPC::X1));
  I.addOperand(MCOperand::createImm(48));
  Out.push_back(I);

  // blr
  I = MCInst(); I.setOpcode(PPC::BLR8);
  Out.push_back(I);
}

namespace llvm {
namespace bolt {

MCPlusBuilder *createPowerPCMCPlusBuilder(const MCInstrAnalysis *Analysis,
                                          const MCInstrInfo *Info,
                                          const MCRegisterInfo *RegInfo,
                                          const MCSubtargetInfo *STI) {
  return new PPCMCPlusBuilder(Analysis, Info, RegInfo, STI);
}

} // namespace bolt
} // namespace llvm