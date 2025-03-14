//===- bolt/Target/PowerPC/PowerPCMCPlusBuilder.cpp -----------------------===//
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

// #include "PowerPCMCSymbolizer.h"
// #include "MCTargetDesc/PPCMCExpr.h"
// #include "MCTargetDesc/PPCMCTargetDesc.h"
// #include "bolt/Core/BinaryBasicBlock.h"
// #include "bolt/Core/BinaryFunction.h"
// #include "bolt/Core/MCPlusBuilder.h"
// #include "llvm/BinaryFormat/ELF.h"
// #include "llvm/MC/MCContext.h"
// #include "llvm/MC/MCFixupKindInfo.h"
// #include "llvm/MC/MCInstBuilder.h"
// #include "llvm/MC/MCInstrInfo.h"
// #include "llvm/MC/MCRegister.h"
// #include "llvm/MC/MCRegisterInfo.h"
// #include "llvm/Support/DataExtractor.h"
// #include "llvm/Support/Debug.h"
// #include "llvm/Support/ErrorHandling.h"