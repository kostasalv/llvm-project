set(LLVM_LINK_COMPONENTS
  DebugInfoDWARF
  Object
  MC
  ${BOLT_TARGETS_TO_BUILD}
  )

set(BOLTTargetPowerPCTestsSources
    PPCMCPlusBuilderTest.cpp)

add_bolt_unittest(BOLTTargetPowerPCTests
  ${BOLTTargetPowerPCTestsSources}
)

target_link_libraries(BOLTTargetPowerPCTests PRIVATE
  LLVMBOLTTargetPowerPC
  LLVMBOLTCore
  LLVMBOLTRewrite
  LLVMCore
)

# PPCMCPlusBuilderTest.cpp's PPCMCPlusBuilderFixture calls initalizeLLVM(),
# which #includes bolt/Core/TargetConfig.def and calls LLVMInitializeXXX...()
# for EVERY target BOLT is configured to support (BOLT_TARGETS_TO_BUILD), not
# just PowerPC -- mirroring bolt/unittests/Core/CMakeLists.txt's CoreTests,
# which does the same target-agnostic init for the same reason. On a
# single-target (PowerPC-only) build this was invisible, but linking this
# unittest against a real multi-target build (e.g. the default
# X86;AArch64;RISCV;PowerPC configuration used upstream) fails with undefined
# references to e.g. LLVMInitializeX86AsmPrinter unless every configured
# target's libraries are linked in, not just PowerPC's. LLVM_LINK_COMPONENTS
# above (${BOLT_TARGETS_TO_BUILD}) supplies those; POWERPC_AVAILABLE plus one
# *_AVAILABLE define per configured target (below) mirrors CoreTests so any
# future PPC-specific target-conditional code in this test can rely on the
# same macros as the rest of the unittest suite.
foreach (tgt ${BOLT_TARGETS_TO_BUILD})
  target_include_directories(BOLTTargetPowerPCTests PRIVATE
    ${LLVM_MAIN_SRC_DIR}/lib/Target/${tgt}
    ${LLVM_BINARY_DIR}/lib/Target/${tgt}
  )
  string(TOUPPER "${tgt}" upper)
  target_compile_definitions(BOLTTargetPowerPCTests PRIVATE "${upper}_AVAILABLE")
endforeach()

target_include_directories(BOLTTargetPowerPCTests PRIVATE
  ${LLVM_BINARY_DIR}/include
  ${LLVM_SOURCE_DIR}/include
  ${LLVM_SOURCE_DIR}/bolt/include
  ${LLVM_BINARY_DIR}/tools/bolt/include
  ${CMAKE_SOURCE_DIR}
)
