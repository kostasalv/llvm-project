/* Calls target() in another object. Linked with long-branch-stub-pad.s between
   the two objects, the call is more than 32 MiB long and GNU ld puts a
   `.long_branch.` stub in front of it. */
extern int target(int);
int main(void) {
#ifdef WRONG_R2
  /* Planted fault: r2 is 8 too high while target runs. target loads g through
     r2, so it returns a different value and main exits with 1. The call goes to
     the local entry, which does not recompute r2. */
  int r;
  __asm__ volatile("addi 2, 2, 8" ::: "memory");
  r = target(0);
  __asm__ volatile("addi 2, 2, -8" ::: "memory");
  return r == 42 ? 0 : 1;
#else
  return target(0) == 42 ? 0 : 1;
#endif
}
