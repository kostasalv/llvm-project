// Exercise the ELFv2 global entry point (GEP) TOC-recompute preamble after
// BOLT has moved the function that carries it.
//
// A call through a function pointer enters a function at its *global* entry
// point, so the two-instruction preamble
//
//   addis r2, r12, (.TOC. - func)@ha    R_PPC64_REL16_HA .TOC. + 0
//   addi  r2, r2,  (.TOC. - func)@l     R_PPC64_REL16_LO .TOC. + 4
//
// actually executes -- unlike a direct call, which the linker retargets to
// the local entry point and which therefore skips it. Both immediates are
// PC-relative, so copying them verbatim to the function's new address leaves
// r2 pointing at unrelated memory and the first TOC access after the preamble
// reads garbage or faults.

#include <stdio.h>

static const int table[4] = {11, 22, 33, 44};

// Reached through fp below, i.e. via its global entry point, and reads a
// static through the TOC so that a wrong r2 is observable.
__attribute__((noinline)) int getval(int i) { return table[i & 3]; }

// volatile so the indirect call is not turned back into a direct call to the
// local entry point, which would never run the preamble.
int (*volatile fp)(int) = getval;

int main(void) {
  int V = fp(2);
  printf("value = %d\n", V);
  return V == 33 ? 0 : 1;
}
