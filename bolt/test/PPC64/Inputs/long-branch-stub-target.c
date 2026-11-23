/* The result depends on r2: six loads from g, addressed relative to the TOC
   pointer (-mcmodel=medium: addis/lwz against r2). Normally the sum is 41 and
   target(0) is 42. If r2 is 8 bytes too high every load reads two ints further
   on, and the sum is different. The body is longer than the 36 bytes BOLT needs
   to patch the entry of a function with a local entry. */
int g[8] = {41, 0, 0, 0, 0, 0, 9, 9};
int target(int x) {
  int s = g[0];
  s += g[1];
  s += g[2];
  s += g[3];
  s += g[4];
  s += g[5];
  return s + 1 + x;
}
