#!/bin/bash
# p1-repro-summary.sh <report>...: per case, dumps seen, dumps with a tail-call mark,
# cannot-relax and internal-call counts, read from a p1-repro-localentry.sh report.
for f in "$@"; do
  echo "##### $f"
  awk '
    /^== c_r/ { if (c != "") printf "%-6s dumps=%-3d marked=%-3d %s\n", c, n, t, x; c=$2; n=0; t=0; x="" }
    /branches=/ { n++; if ($0 ~ /tailcall=[1-9]/) t++ }
    /cannot-relax:|internal-call:/ { gsub(/^ +/, ""); x = x " " $0 }
    /^== all cannot-relax/ { if (c != "") printf "%-6s dumps=%-3d marked=%-3d %s\n", c, n, t, x; c="" }
  ' "$f"
done
