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

static const MCSymbol *getBranchTargetSymbol(const MCInst &I) {
  // For B/BC the last operand is a branch target (expr)
  if (I.getNumOperands() == 0)
    return nullptr;
  const MCOperand &Op = I.getOperand(I.getNumOperands() - 1);
  if (!Op.isExpr())
    return nullptr;
  if (auto *SymRef = dyn_cast<MCSymbolRefExpr>(Op.getExpr()))
    return &SymRef->getSymbol();
  return nullptr;
}

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
  case PPC::BDNZ:          // decrement CTR, branch if not zero
  case PPC::BDNZL:         // decrement CTR, branch with link if not zero
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
  case PPC::BDNZ:
  case PPC::BDNZL:
    return 26;
  // Conditional branch: 16-bit signed offset (±32KB)
  case PPC::BC:
  case PPC::gBC:
  case PPC::BCL:
  case PPC::gBCL:
  case PPC::BCC:   // extended-mnemonic conditional branch (bt/bf/beq/bne...)
  case PPC::BCCA:  // conditional branch absolute (extended mnemonic)
  case PPC::BCCL:  // conditional branch with link (extended mnemonic)
  case PPC::BCCLA: // conditional branch with link absolute (extended mnemonic)
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
  // Same instruction sequence as buildCallStubAbsolute().
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
  switch (Inst.getOpcode()) {
  case PPC::B:
  case PPC::BA:
  case PPC::BCTR:
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
  case PPC::BDNZ:  // decrement CTR and branch if not zero
  case PPC::BDNZL: // decrement CTR and branch with link
  case PPC::BCTR:  // branch to CTR
  case PPC::BCTRL: // branch to CTR with link
  case PPC::BLR:   // branch to LR
  case PPC::BLRL:  // branch to LR with link
    return true;
  default:
    return false;
  }
}

bool PPCMCPlusBuilder::isTailCall(const MCInst &I) const {
  (void)I;
  return false;
}

bool PPCMCPlusBuilder::isReturn(const MCInst &Inst) const {
  return Inst.getOpcode() == PPC::BLR;
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
  // terminator, mirroring what the base-class does for architectures that set
  // the Terminator flag consistently.
  if (isBranch(Inst) || isReturn(Inst))
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
    return true;
  default:
    return false;
  }
}

bool PPCMCPlusBuilder::isUnconditionalBranch(const MCInst &I) const {
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

  if (Begin == End)
    return false;

  // Look at the last instruction (canonical BOLT pattern)
  InstructionIterator I = End;
  --I;
  const MCInst &Last = *I;

  // Return (blr) → no branch terminator
  if (Last.getOpcode() == PPC::BLR) {
    return false;
  }

  if (isUnconditionalBranch(Last)) {
    UncondBr = const_cast<MCInst *>(&Last);
    Tgt = getBranchTargetSymbol(Last);
    // with an unconditional branch, there's no fall-through
    return false;
  }

  if (isConditionalBranch(Last)) {
    CondBr = const_cast<MCInst *>(&Last);
    Tgt = getBranchTargetSymbol(Last);
    // Assume the block has a fallthrough if no following unconditional branch.
    // (BOLT will compute actual fallthrough later once CFG is built.)
    return false;
  }

  // Otherwise: not a branch terminator (let caller treat as fallthrough/ret)
  return false;
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

// Build a 64-bit absolute address of the callee's function address (e.g.
// "puts") into r12, then tail-call it via BCTR.
void PPCMCPlusBuilder::buildCallStubAbsolute(MCContext *Ctx,
                                             const MCSymbol *Target,
                                             std::vector<MCInst> &Out) const {
  Out.clear();
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

  // std r2, 24(r1)      ; save caller's TOC
  I = MCInst();
  I.setOpcode(PPC::STD);
  I.addOperand(R(PPC::X2)); // reg (src)
  I.addOperand(MCOperand::createExpr(
      MCConstantExpr::create(24, *Ctx))); // disp (slot #1)
  I.addOperand(R(PPC::X1));               // base (slot #2)
  Out.push_back(I);

  // lis    r12, Target@highest         ; r12 = highest << 16
  I = MCInst();
  I.setOpcode(PPC::LIS8);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createExpr(HST));
  Out.push_back(I);

  // ori    r12, r12, Target@higher     ; r12 |= higher
  I = MCInst();
  I.setOpcode(PPC::ORI8);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createExpr(HER));
  Out.push_back(I);

  // rldicr r12, r12, 32, 31            ; shift the top 32 bits up
  I = MCInst();
  I.setOpcode(PPC::RLDICR);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createImm(32)); // shift amount
  I.addOperand(MCOperand::createImm(
      31)); // mask end (MB..ME semantics from PPCInstrInfo.cpp:3470)
  Out.push_back(I);

  // oris   r12, r12, Target@h          ; r12 |= (high << 16)
  I = MCInst();
  I.setOpcode(PPC::ORIS8);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createExpr(HI));
  Out.push_back(I);

  // ori    r12, r12, Target@l          ; r12 |= low
  I = MCInst();
  I.setOpcode(PPC::ORI8);
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createReg(R12));
  I.addOperand(MCOperand::createExpr(LO));
  Out.push_back(I);
  // --- r12 now holds the absolute address of Target ---

  // mtctr r12
  I = MCInst();
  I.setOpcode(PPC::MTCTR8);
  I.addOperand(R(R12));
  Out.push_back(I);

  // bctrl               ; link-return to stub
  I = MCInst();
  I.setOpcode(PPC::BCTRL);
  Out.push_back(I);

  // ld r2, 24(r1)       ; restore TOC
  I = MCInst();
  I.setOpcode(PPC::LD);
  I.addOperand(R(PPC::X2)); // reg (dst)
  I.addOperand(MCOperand::createExpr(
      MCConstantExpr::create(24, *Ctx))); // disp (slot #1)
  I.addOperand(R(PPC::X1));               // base (slot #2)
  Out.push_back(I);

  // blr                 ; return to caller
  I = MCInst();
  I.setOpcode(PPC::BLR8); // or PPC::BLR on some trees
  Out.push_back(I);
}

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