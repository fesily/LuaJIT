#!/bin/sh
# LuaJIT multi-architecture build/test driver. Runs inside the CI containers
# started from ../docker-compose.yaml.
#
#   ci.sh native    copy $SRC to $WORK, build it, run smoke + test stage
#   ci.sh cross     build every target in docker/matrix.txt, run what can run
#
# Environment:
#   SRC          source tree bind-mounted read-only              (/src)
#   WORK         scratch build directory inside the container    (/work)
#   MAKE_JOBS    parallel make jobs                              (nproc)
#   MAKE_FLAGS   extra make arguments, e.g. "XCFLAGS=-DLUAJIT_DISABLE_JIT"
#   STAGE        build | test | all                              (all)
#   EXPECT_ARCH  expected jit.arch of the smoke run, e.g. arm64
#   TEST_CMD     shell command replacing the tests/run.sh hook

set -eu

SRC=${SRC:-/src}
WORK=${WORK:-/work}
STAGE=${STAGE:-all}
JOBS=${MAKE_JOBS:-$(nproc)}
MAKE_FLAGS=${MAKE_FLAGS:-}
EXPECT_ARCH=${EXPECT_ARCH:-}
TEST_CMD=${TEST_CMD:-}

say() { printf '\n===== %s =====\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# Copy the tree so host artifacts (foreign .o files, stale binaries) never leak
# into a build, and so every target gets a pristine build directory.
copy_src() {
  rm -rf "$1"
  mkdir -p "$1"
  tar -C "$SRC" -cf - \
    --exclude=.git \
    --exclude='*.o' \
    --exclude='*.a' \
    --exclude='*.so' \
    --exclude='*.so.*' \
    . | tar -C "$1" -xf -
}

do_build() {
  say "build: $(uname -m) ($(. /etc/os-release && echo "$PRETTY_NAME")) make -j$JOBS $MAKE_FLAGS"
  # Unquoted on purpose: MAKE_FLAGS holds several make arguments.
  # shellcheck disable=SC2086
  make -j"$JOBS" $MAKE_FLAGS
  say "build: ok ($(stat -c%s src/luajit) bytes, $(file -b src/luajit | cut -d, -f1-2))"
}

do_smoke() {
  arch=$(./src/luajit -e 'io.write(require("jit").arch)')
  os=$(./src/luajit -e 'io.write(require("jit").os)')
  say "smoke: jit.arch=$arch jit.os=$os"
  if [ -n "$EXPECT_ARCH" ] && [ "$arch" != "$EXPECT_ARCH" ]; then
    die "jit.arch=$arch, expected $EXPECT_ARCH"
  fi
  ./src/luajit docker/smoke.lua
}

do_test() {
  if [ -n "$TEST_CMD" ]; then
    say "test: $TEST_CMD"
    sh -c "$TEST_CMD"
  elif [ -x ./tests/run.sh ]; then
    say "test: tests/run.sh"
    ./tests/run.sh
  else
    say "test: no test suite in the tree yet (tests/run.sh / TEST_CMD) - skipped"
  fi
}

native() {
  copy_src "$WORK"
  cd "$WORK"
  do_build
  if [ "$STAGE" != build ]; then
    do_smoke
  fi
  case "$STAGE" in
    test|all) do_test ;;
    *) say "stage=$STAGE: test stage skipped" ;;
  esac
  say "PASS $(uname -m) (stage=$STAGE)"
}

# elf_check <file> <cross-prefix> <class> <data> <machine>
# Guards against the classic cross-build trap: silently producing host binaries.
elf_check() {
  f=$1; crosspfx=$2; wantcls=$3; wantdata=$4; wantmach=$5
  re="${crosspfx}readelf"
  command -v "$re" >/dev/null 2>&1 || re=readelf
  hdr=$("$re" -h "$f") || { echo "readelf failed on $f" >&2; return 1; }
  if echo "$hdr" | grep -q "Class: *$wantcls" &&
     echo "$hdr" | grep -q "Data:.*$wantdata" &&
     echo "$hdr" | grep -q "Machine:.*$wantmach"; then
    return 0
  fi
  echo "$hdr" | grep -E '^ *(Class|Data|Machine):' | tr -s ' ' >&2
  echo "expected $wantcls / $wantdata / $wantmach" >&2
  return 1
}

# matrix.txt fields:
#   name|cross-prefix|target-cflags|ELF class|endianness|ELF machine|sysroot|jit.arch
cross() {
  matrix=$SRC/docker/matrix.txt
  [ -f "$matrix" ] || die "missing $matrix"
  rm -rf "$WORK/cross"
  mkdir -p "$WORK/cross"
  results=$WORK/cross/results.txt
  : >"$results"
  total=0; built=0; ran=0; failed=0

  while IFS='|' read -r name crosspfx tflags cls data machine sysroot wantarch; do
    case "$name" in ''|'#'*) continue ;; esac
    total=$((total + 1))
    if ! command -v "${crosspfx}gcc" >/dev/null 2>&1; then
      printf '%-13s SKIP-NOTOOLCHAIN (%sgcc not packaged for this host)\n' \
        "$name" "$crosspfx" >>"$results"
      continue
    fi
    dir=$WORK/cross/$name
    # LuaJIT's buildvm refuses to run when its pointer size differs from the
    # target's (src/host/buildvm.c: "pointer size mismatch in cross-build").
    # A 32-bit target therefore needs a 32-bit host tool, i.e. -m32 support.
    hostcc=
    if [ "$cls" = ELF32 ] && [ "$(getconf LONG_BIT)" = 64 ]; then
      if printf 'int main(void){return 0;}\n' | gcc -m32 -x c - -o /dev/null >/dev/null 2>&1; then
        hostcc="gcc -m32"
      else
        printf '%-13s SKIP-HOSTBITS (32-bit target, no 32-bit host toolchain)\n' \
          "$name" >>"$results"
        continue
      fi
    fi
    say "cross: $name (${crosspfx}gcc, $(uname -m) host${hostcc:+, HOST_CC=$hostcc})"
    copy_src "$dir"
    if ! (
      cd "$dir" || exit 1
      set --
      [ -z "$tflags" ] || [ "$tflags" = - ] || set -- TARGET_CFLAGS="$tflags"
      [ -z "$hostcc" ] || set -- HOST_CC="$hostcc"
      # shellcheck disable=SC2086
      make -j"$JOBS" $MAKE_FLAGS CROSS="$crosspfx" "$@"
    ) >"$dir/build.log" 2>&1; then
      failed=$((failed + 1))
      printf '%-13s BUILD-FAIL\n' "$name" >>"$results"
      tail -n 15 "$dir/build.log" >&2 || true
      continue
    fi
    if ! elf_check "$dir/src/luajit" "$crosspfx" "$cls" "$data" "$machine"; then
      failed=$((failed + 1))
      printf '%-13s BAD-ELF\n' "$name" >>"$results"
      continue
    fi
    built=$((built + 1))
    line=$(printf '%-13s BUILT  %s' "$name" "$(file -b "$dir/src/luajit" | cut -d, -f1,2)")
    # Optional runtime check: qemu-user + the cross sysroot as ld.so prefix.
    if [ -n "$sysroot" ] && [ "$sysroot" != - ]; then
      if out=$(cd "$dir" && QEMU_LD_PREFIX="$sysroot" ./src/luajit docker/smoke.lua 2>&1); then
        got=$(cd "$dir" && QEMU_LD_PREFIX="$sysroot" ./src/luajit -e 'io.write(require("jit").arch)')
        if [ "$got" = "$wantarch" ]; then
          ran=$((ran + 1))
          line="$line  RUN jit.arch=$got"
        else
          failed=$((failed + 1))
          line="$line  RUN-BADARCH got=$got want=$wantarch"
        fi
      else
        line="$line  SKIP-RUN $(printf '%s' "$out" | head -n 1)"
      fi
    fi
    printf '%s\n' "$line" >>"$results"
    # The objects are only needed until the binary is verified; dropping them
    # keeps a 14-target matrix inside a small build volume.
    rm -f "$dir"/src/*.o "$dir"/src/host/*.o "$dir"/src/libluajit.a
  done <"$matrix"

  cat "$results"
  say "cross: $total targets, $built built, $ran run under qemu, $failed failed"
  say "build logs: $WORK/cross/<target>/build.log"
  [ "$failed" -eq 0 ]
}

case "${1:-native}" in
  native) native ;;
  cross) cross ;;
  *) die "usage: ci.sh [native|cross]" ;;
esac
