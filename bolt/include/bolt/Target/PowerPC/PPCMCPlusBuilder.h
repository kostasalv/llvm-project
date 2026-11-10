#pragma once
#include "bolt/Core/MCPlusBuilder.h"
#include <vector>

namespace llvm {
class MCInst;
class MCSymbol;
namespace bolt {

class PPCMCPlusBuilder : public MCPlusBuilder {
public:
  using MCPlusBuilder::MCPlusBuilder;

  static void createPushRegisters(MCInst &Inst1, MCInst &Inst2, MCPhysReg Reg1,
                                  MCPhysReg Reg2);

  bool shouldRecordCodeRelocation(unsigned Type) const override;

  bool evaluateBranch(const MCInst &Inst, uint64_t Addr, uint64_t Size,
                      uint64_t &Target) const override;

  bool evaluateMemOperandTarget(const MCInst &Inst, uint64_t &Target,
                                uint64_t Address = 0,
                                uint64_t Size = 0) const override;
  bool hasPCRelOperand(const MCInst &I) const override;
  int getPCRelOperandNum(const MCInst &Inst) const;

  int getMemoryOperandNo(const MCInst &Inst) const override;

  void replaceBranchTarget(MCInst &Inst, const MCSymbol *TBB,
                           MCContext *Ctx) const override;
  bool isIndirectBranch(const MCInst &I) const override;

  const MCSymbol *getTargetSymbol(const MCInst &Inst,
                                  unsigned OpNum = 0) const override;

  bool convertJmpToTailCall(MCInst &Inst) override;

  bool isCall(const MCInst &Inst) const override;

  bool isIndirectCall(const MCInst &I) const override;

  bool isBranch(const MCInst &Inst) const override;

  bool isTailCall(const MCInst &Inst) const;
  bool isReturn(const MCInst &Inst) const override;
  bool isConditionalBranch(const MCInst &Inst) const override;
  bool isUnconditionalBranch(const MCInst &Inst) const override;

  const MCInst *getConditionalTailCall(const MCInst &Inst) const;

  IndirectBranchType
  analyzeIndirectBranch(MCInst &Instruction, InstructionIterator Begin,
                        InstructionIterator End, const unsigned PtrSize,
                        MCInst *&MemLocInstrOut, unsigned &BaseRegNumOut,
                        unsigned &IndexRegNumOut, int64_t &DispValueOut,
                        const MCExpr *&DispExprOut, MCInst *&PCRelBaseOut,
                        MCInst *&FixedEntryLoadInstr) const override;

  bool isNoop(const MCInst &Inst) const override;
  void createNoop(MCInst &Nop) const override;

  /// The unconditional trap, `tw 31,0,0` (extended mnemonic `trap`), in the
  /// byte order of the target.
  StringRef getTrapFillValue() const override;

  /// Create a return instruction (blr on PPC64).
  void createReturn(MCInst &Inst) const override;

  bool analyzeBranch(InstructionIterator Begin, InstructionIterator End,
                     const llvm::MCSymbol *&Tgt,
                     const llvm::MCSymbol *&Fallthrough, llvm::MCInst *&CondBr,
                     llvm::MCInst *&UncondBr) const override;

  bool lowerTailCall(llvm::MCInst &Inst) override;

  uint64_t analyzePLTEntry(MCInst &Instruction, InstructionIterator Begin,
                           InstructionIterator End,
                           uint64_t BeginPC) const override;

  void createLongTailCall(std::vector<MCInst> &Seq, const MCSymbol *Target,
                          MCContext *Ctx) override;

  std::optional<Relocation>
  createRelocation(const MCFixup &Fixup,
                   const MCAsmBackend &MAB) const override;

  bool isTOCRestoreAfterCall(const MCInst &I) const override;

  /// BL_NOP and BL8_NOP are one 8-byte instruction, `bl target` plus the nop
  /// of the ELFv2 call slot.
  bool isCallWithNOPSlot(const MCInst &Inst) const override;

  /// True for a nop (`nop` or `ori r0,r0,0`) or the TOC restore
  /// `ld r2,24(r1)`.
  bool isCallSlotInstruction(const MCInst &Inst) const override;

  /// Turn `bl` into `BL_NOP`, so the slot is part of the instruction and is
  /// counted by every size computation.
  bool ensureCallNOPSlot(MCInst &Inst) const override;

  bool symbolizeTOCSetup(MCInst &Inst, ArrayRef<uint8_t> Code, uint64_t Offset,
                         uint64_t FuncAddr, uint64_t TOCBase,
                         const MCSymbol *FuncSym, bool IsLittleEndian,
                         MCContext *Ctx) const override;

  bool symbolizeTOC16Reference(MCInst &Inst, const Relocation &Rel,
                               MCContext *Ctx) const override;

  const MCSymbol *getTargetSymbol(const MCExpr *Expr) const override;

  /// Two specifier expressions (@ha, @l, @toc@ha, ...) are equal when they have
  /// the same specifier and equal sub-expressions. The default implementation
  /// aborts, and identical code folding reaches it with the operands that
  /// symbolizeTOCSetup and symbolizeTOC16Reference create.
  bool equals(const MCSpecifierExpr &A, const MCSpecifierExpr &B,
              CompFuncTy Comp) const override;

  /// If \p Inst is a plain relative branch (b, not bl) whose target is a
  /// symbol, return that symbol. Return nullptr for every other instruction.
  /// Such a branch does not set LR, so it can be sent through a tail-jump stub.
  const MCSymbol *getPlainBranchTargetSymbol(const MCInst &Inst) const;

  /// Return true if \p Instruction is a bctr that dispatches a PIC jump table:
  ///   lwax  rDst, rBase, rIndex  (load signed 32-bit table entry)
  ///   ...
  ///   mtctr rDst
  ///   bctr                       <- Instruction
  bool isPICJumpTableBctr(const MCInst &Instruction, InstructionIterator Begin,
                          InstructionIterator End) const override;
};

} // namespace bolt
} // namespace llvm