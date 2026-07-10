#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TEST="$ROOT/test/compat51/tailcall_debug.lua"
LJ="${LUAJIT:-$ROOT/src/luajit}"
LUA51="${LUA51:-lua5.1}"

# Hard limits: prevent runaway memory if getstack loops
ulimit -v 1048576 2>/dev/null || true   # 1GB virtual (if supported)
ulimit -d 1048576 2>/dev/null || true
ulimit -t 30 2>/dev/null || true        # 30s CPU

run_one() {
  local name="$1"; shift
  echo "=== $name ==="
  # timeout if available
  if command -v timeout >/dev/null 2>&1; then
    timeout 20s "$@" || local rc=$?
    echo "exit=${rc:-0}"
    return ${rc:-0}
  else
    "$@"
  fi
}

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

set +e
run_one "Lua 5.1.5" "$LUA51" "$TEST" | tee "$OUT/51.txt"
rc51=${PIPESTATUS[0]}
run_one "LuaJIT -joff" "$LJ" -joff "$TEST" | tee "$OUT/joff.txt"
rcjoff=${PIPESTATUS[0]}
run_one "LuaJIT jit-on" "$LJ" "$TEST" | tee "$OUT/jon.txt"
rcjon=${PIPESTATUS[0]}

echo
echo "=== machine dump ==="
timeout 10s "$LUA51" "$TEST" --dump | tee "$OUT/d51.txt"
timeout 10s "$LJ" -joff "$TEST" --dump | tee "$OUT/dlj.txt"
norm_dump() {
  sed -e '/^engine=/d' \
      -e 's/source=@/source=/' \
      -e 's/ name=[^ ]*/ name=/' \
      "$1"
}
norm_dump "$OUT/d51.txt" > "$OUT/d51n.txt" || true
norm_dump "$OUT/dlj.txt" > "$OUT/dljn.txt" || true
echo "--- diff (ignore engine, @source, name) ---"
if diff -u "$OUT/d51n.txt" "$OUT/dljn.txt"; then
  echo "(no dump diff)"
else
  echo "(remaining dump diffs are non-tail or unexpected)"
fi

echo
echo "=== PASS/FAIL matrix ==="
python3 - "$OUT/51.txt" "$OUT/joff.txt" "$OUT/jon.txt" <<'PY'
import sys, re
from collections import OrderedDict
def parse(p):
  d=OrderedDict()
  try:
    for line in open(p, errors='replace'):
      m=re.match(r'^(PASS|FAIL)\s+(.*)$', line.rstrip())
      if m: d[m.group(2)]=m.group(1)
  except FileNotFoundError:
    pass
  return d
ds=[parse(p) for p in sys.argv[1:]]
names=OrderedDict()
for d in ds:
  for k in d: names[k]=1
print(f"{'test':<48} {'5.1.5':<8} {'joff':<8} {'jon':<8}")
print('-'*80)
mis=0
for n in names:
  row=[d.get(n,'MISS') for d in ds]
  mark='  <== DIFF' if len(set(row))>1 else ''
  if mark: mis+=1
  print(f"{n:<48} {row[0]:<8} {row[1]:<8} {row[2]:<8}{mark}")
print(f"disagreements: {mis}")
PY
echo "summary: lua51=$rc51 joff=$rcjoff jon=$rcjon"
