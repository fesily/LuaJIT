#!/usr/bin/env bash

set -uo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX_DIR=${PREFIX_DIR:-"$ROOT_DIR/.test-prefix"}
DEFAULT_JOBS=${JOBS:-4}
RUN_OPENRESTY=${RUN_OPENRESTY:-auto}
OPENRESTY_JOBS=${OPENRESTY_JOBS:-1}
FAILURES=0

usage() {
  cat <<'EOF'
Usage: ./run-gen-gc-validation.sh [options]

Options:
  --jobs N                Parallel jobs for make. Default: 4 or $JOBS
  --prefix DIR            Install prefix for runtime validation.
  --openresty             Force running luajit2-test-suite.
  --skip-openresty        Skip luajit2-test-suite.
  --openresty-jobs N      Parallel jobs for luajit2-test-suite.
  -h, --help              Show this help.

Environment overrides:
  JOBS, PREFIX_DIR, RUN_OPENRESTY, OPENRESTY_JOBS
EOF
}

log() {
  printf '\n[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

fail() {
  echo "$*" >&2
  FAILURES=$((FAILURES + 1))
}

run_step() {
  local description=$1
  shift

  log "$description"
  if ! "$@"; then
    fail "$description failed"
    return 1
  fi
  return 0
}

run_gc_batch() {
  (
    cd "$ROOT_DIR/LuaJIT-test-cleanup"
    sh run-gc-batch.sh
  )
}

run_ffi_batch() {
  (
    cd "$ROOT_DIR/LuaJIT-test-cleanup"
    sh run-ffi-batch.sh
  )
}

restore_default_build() {
  make -C "$ROOT_DIR" clean >/dev/null
  make -C "$ROOT_DIR" -j"$DEFAULT_JOBS"
}

run_openresty_suite() {
  if [[ "$RUN_OPENRESTY" == "never" ]]; then
    log "Skipping luajit2-test-suite by request"
    return 0
  fi

  if ! command -v pkg-config >/dev/null 2>&1; then
    if [[ "$RUN_OPENRESTY" == "always" ]]; then
      echo "pkg-config is required for luajit2-test-suite" >&2
      return 1
    fi
    log "Skipping luajit2-test-suite: pkg-config not found"
    return 0
  fi

  if ! pkg-config --exists gtk+-2.0; then
    if [[ "$RUN_OPENRESTY" == "always" ]]; then
      echo "gtk+-2.0 development files are required for luajit2-test-suite" >&2
      return 1
    fi
    log "Skipping luajit2-test-suite: missing gtk+-2.0 pkg-config metadata"
    return 0
  fi

  (
    cd "$ROOT_DIR/luajit2-test-suite"
    ./run-tests -j "$OPENRESTY_JOBS" "$PREFIX_DIR" "$PREFIX_DIR/bin/luajit"
  )
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --jobs)
      DEFAULT_JOBS=$2
      shift 2
      ;;
    --prefix)
      PREFIX_DIR=$2
      shift 2
      ;;
    --openresty)
      RUN_OPENRESTY=always
      shift
      ;;
    --skip-openresty)
      RUN_OPENRESTY=never
      shift
      ;;
    --openresty-jobs)
      OPENRESTY_JOBS=$2
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

run_step "Cleaning tree before default build" make -C "$ROOT_DIR" clean >/dev/null
run_step "Building default configuration" make -C "$ROOT_DIR" -j"$DEFAULT_JOBS"
run_step "Installing default configuration into $PREFIX_DIR" make -C "$ROOT_DIR" install PREFIX="$PREFIX_DIR"

run_step "Running LuaJIT-test-cleanup GC batch" run_gc_batch || true
run_step "Running LuaJIT-test-cleanup FFI batch" run_ffi_batch || true
run_step "Running luajit2-test-suite" run_openresty_suite || true

run_step "Cleaning tree before LJ_GEN_GC-disabled build" make -C "$ROOT_DIR" clean >/dev/null
run_step "Validating LJ_GEN_GC-disabled build" make -C "$ROOT_DIR" XCFLAGS='-DLUAJIT_DISABLE_GEN_GC' -j"$DEFAULT_JOBS"
run_step "Restoring default configuration" restore_default_build

if [[ "$FAILURES" -eq 0 ]]; then
  log "Validation finished successfully"
else
  log "Validation finished with $FAILURES failing step(s)"
  exit 1
fi