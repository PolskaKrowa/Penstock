#!/usr/bin/env bash
# Tests for the third_party/{llvm,alive2,z3} submodule integration
# (cmake/ThirdPartySubmodules.cmake + the LlvmBootstrap/PenstockSuperopt
# wiring). Exercises the CMake logic only: no real LLVM/clang build happens.
#
# usage: scripts/test-third-party-submodules.sh
set -uo pipefail

cd "$(dirname "$0")/.."
REPO="$PWD"
WORK="$(mktemp -d /tmp/penstock-submodule-tests.XXXXXX)"
PASS=0
FAIL=0

cleanup() {
  # The patch tests intentionally dirty the z3/alive2 checkouts; restore them
  # so the repository stays pristine after a test run (and so a run started
  # from a previously patched state sees the pristine upstream markers).
  git -C "$REPO/third_party/z3" checkout -- . >/dev/null 2>&1 || true
  git -C "$REPO/third_party/alive2" checkout -- . >/dev/null 2>&1 || true
  rm -rf "$WORK"
}
trap cleanup EXIT

# Enter with pristine submodules (a configure from an earlier run may have
# left the idempotent patches applied).
git -C "$REPO/third_party/z3" checkout -- . >/dev/null 2>&1 || true
git -C "$REPO/third_party/alive2" checkout -- . >/dev/null 2>&1 || true

note() { printf '\n== %s\n' "$*"; }
pass() { echo "   PASS: $*"; PASS=$((PASS + 1)); }
fail() { echo "   FAIL: $*" >&2; FAIL=$((FAIL + 1)); }

HARNESS="$REPO/scripts/third-party-submodule-tests.cmake"

run_mode() { # run_mode <mode> <third_party_root> [extra -D args...]
  local mode="$1" root="$2"; shift 2
  mkdir -p "$(dirname "$root")"
  cmake -DPENSTOCK_TEST_MODE="$mode" \
        -DPENSTOCK_TEST_WORK="$WORK/$mode" \
        -DPENSTOCK_TEST_REPO="$REPO" \
        -DPENSTOCK_THIRD_PARTY_ROOT="$root" \
        "$@" -P "$HARNESS"
}

# ---------------------------------------------------------------- T1 detection
note "T1: detection (absent / uninitialised / marker / overrides)"
if run_mode detection "$WORK/detection/third_party" >"$WORK/detection.log" 2>&1; then
  pass "detection mode all checks"
else
  fail "detection mode"; sed -n '1,40p' "$WORK/detection.log"
fi

# ------------------------------------------------------- T2 forced-but-missing
note "T2: PENSTOCK_SUBMODULE_Z3=ON with a missing checkout must fail"
mkdir -p "$WORK/fatal/third_party/z3/src/api"
if run_mode fatal-forced "$WORK/fatal/third_party" \
     -DPENSTOCK_SUBMODULE_Z3=ON >"$WORK/fatal.log" 2>&1; then
  fail "forced-missing submodule did not fail"
else
  if grep -q "PENSTOCK_SUBMODULE_Z3=ON but third_party/z3 is not initialised" \
      "$WORK/fatal.log"; then
    pass "forced-missing submodule fails with the git hint"
  else
    fail "forced-missing submodule failed with an unexpected message"
    sed -n '1,30p' "$WORK/fatal.log"
  fi
fi

# ------------------------------------------------------------------ T3 real z3
note "T3: z3 resolver + patcher on the real third_party/z3 checkout"
if [ ! -f "$REPO/third_party/z3/src/api/CMakeLists.txt" ]; then
  echo "   (skipped: third_party/z3 is not initialised)"
else
  if run_mode real-z3 "$REPO/third_party" >"$WORK/real-z3.log" 2>&1; then
    pass "real z3 resolver + patcher + idempotency"
  else
    fail "real z3 mode"; sed -n '1,40p' "$WORK/real-z3.log"
  fi
fi

# -------------------------------------------------------------- T4 real alive2
note "T4: alive2 resolver + patcher on the real third_party/alive2 checkout"
if [ ! -f "$REPO/third_party/alive2/ir/instr.h" ]; then
  echo "   (skipped: third_party/alive2 is not initialised)"
else
  if run_mode real-alive2 "$REPO/third_party" >"$WORK/real-alive2.log" 2>&1; then
    pass "real alive2 resolver + patcher + idempotency"
  else
    fail "real alive2 mode"; sed -n '1,40p' "$WORK/real-alive2.log"
  fi
fi

# ------------------------------------------------------------------ T5 builder
note "T5: LLVM backend from-source builder (fake llvm-project tree, realistic build-tree layout)"
if run_mode builder "$WORK/builder/third_party" >"$WORK/builder.log" 2>&1; then
  pass "builder configure/build/validate/stamp + reuse + move rebuild"
else
  fail "builder mode"; sed -n '1,40p' "$WORK/builder.log"
fi

# --------------------------------------------------------------- T5b incomplete
note "T5b: builder fatal on an incomplete backend (no PassPlugin.h anywhere)"
if run_mode builder-incomplete "$WORK/builder-incomplete/third_party" \
     >"$WORK/builder-incomplete.log" 2>&1; then
  fail "incomplete backend was accepted"
else
  if grep -q "does not contain a complete backend prefix" \
      "$WORK/builder-incomplete.log" && \
     grep -q "PassPlugin.h" "$WORK/builder-incomplete.log"; then
    pass "incomplete backend fatals, naming the missing components"
  else
    fail "incomplete backend failed with an unexpected message"
    sed -n '1,40p' "$WORK/builder-incomplete.log"
  fi
fi

# ------------------------------------------------------- T6 full project config
note "T6: full project configure with a fake SDK (superopt OFF)"
FAKE_SDK="$WORK/fake-sdk"
mkdir -p "$FAKE_SDK/bin" "$FAKE_SDK/lib/cmake/llvm" "$FAKE_SDK/include/llvm/Plugins"
cat > "$FAKE_SDK/bin/clang" <<'EOF'
#!/bin/sh
case "$1" in
  --version) echo "clang version 19.1.7 (fake penstock test SDK)"; exit 0 ;;
esac
exec /usr/bin/cc "$@"
EOF
cat > "$FAKE_SDK/bin/clang++" <<'EOF'
#!/bin/sh
case "$1" in
  --version) echo "clang version 19.1.7 (fake penstock test SDK)"; exit 0 ;;
esac
exec /usr/bin/c++ "$@"
EOF
cat > "$FAKE_SDK/bin/opt" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$FAKE_SDK/bin/clang" "$FAKE_SDK/bin/clang++" "$FAKE_SDK/bin/opt"
printf '// fake\n' > "$FAKE_SDK/include/llvm/Plugins/PassPlugin.h"
cat > "$FAKE_SDK/lib/cmake/llvm/LLVMConfig.cmake" <<EOF
set(LLVM_PACKAGE_VERSION 19.1.7)
set(LLVM_ENABLE_RTTI ON)
set(LLVM_ENABLE_EH ON)
set(LLVM_INCLUDE_DIRS "$FAKE_SDK/include")
set(LLVM_DEFINITIONS "")
set(LLVM_CMAKE_DIR "$FAKE_SDK/lib/cmake/llvm")
EOF
cat > "$FAKE_SDK/lib/cmake/llvm/AddLLVM.cmake" <<'EOF'
# Minimal fake of LLVM's AddLLVM for configure-only tests (Alive2 calls
# add_llvm_executable and llvm_map_components_to_libnames at configure time).
function(add_llvm_executable name)
  add_executable(${name} ${ARGN})
endfunction()
function(llvm_map_components_to_libnames out)
  set(${out} "" PARENT_SCOPE)
endfunction()
EOF

CONF_OFF="$WORK/conf-off"
if cmake -S "$REPO" -B "$CONF_OFF" \
     -DLLVM_ROOT="$FAKE_SDK" \
     -DPENSTOCK_BUILD_EMBEDDED_SUPEROPT=OFF \
     -DPENSTOCK_BUILD_OPT_PLUGIN=OFF \
     -DCMAKE_BUILD_TYPE=Debug >"$WORK/conf-off.log" 2>&1 &&
   grep -q "PENSTOCK CONFIGURATION COMPLETE" "$WORK/conf-off.log"; then
  pass "configure completes with submodules present (superopt OFF)"
else
  fail "configure (superopt OFF)"; sed -n '1,50p' "$WORK/conf-off.log"
fi

# ... and again with the submodules explicitly ignored.
CONF_OFF2="$WORK/conf-off2"
if cmake -S "$REPO" -B "$CONF_OFF2" \
     -DLLVM_ROOT="$FAKE_SDK" \
     -DPENSTOCK_BUILD_EMBEDDED_SUPEROPT=OFF \
     -DPENSTOCK_BUILD_OPT_PLUGIN=OFF \
     -DPENSTOCK_USE_SUBMODULES=OFF \
     -DCMAKE_BUILD_TYPE=Debug >"$WORK/conf-off2.log" 2>&1 &&
   grep -q "PENSTOCK CONFIGURATION COMPLETE" "$WORK/conf-off2.log"; then
  pass "configure completes with PENSTOCK_USE_SUBMODULES=OFF"
else
  fail "configure (PENSTOCK_USE_SUBMODULES=OFF)"; sed -n '1,50p' "$WORK/conf-off2.log"
fi

# --------------------------------------------------- T7 superopt-ON configure
note "T7: full project configure with the z3/alive2 submodules (superopt ON)"
# re2c is required by Alive2's build; extract it from the Debian package when
# the system has none (the test never builds Alive2, only configures it).
RE2C_BIN="$(command -v re2c || true)"
if [ -z "$RE2C_BIN" ] && [ -x /tmp/re2c-pkg/usr/bin/re2c ]; then
  export PATH="/tmp/re2c-pkg/usr/bin:$PATH"
  RE2C_BIN="/tmp/re2c-pkg/usr/bin/re2c"
fi
CONF_ON="$WORK/conf-on"
if [ -n "$RE2C_BIN" ]; then
  if cmake -S "$REPO" -B "$CONF_ON" \
       -DLLVM_ROOT="$FAKE_SDK" \
       -DPENSTOCK_BUILD_EMBEDDED_SUPEROPT=ON \
       -DPENSTOCK_BUILD_OPT_PLUGIN=OFF \
       -DPENSTOCK_BUILD_RTTI_LLVM=OFF \
       -DCMAKE_BUILD_TYPE=Debug >"$WORK/conf-on.log" 2>&1; then
    ok=1
    grep -q "building Z3 from the third_party/z3 submodule" "$WORK/conf-on.log" \
      && pass "superopt configure uses the z3 submodule" || { fail "z3 submodule not used"; ok=0; }
    grep -q "building Alive2 from the third_party/alive2 submodule" "$WORK/conf-on.log" \
      && pass "superopt configure uses the alive2 submodule" || { fail "alive2 submodule not used"; ok=0; }
    grep -q "PENSTOCK CONFIGURATION COMPLETE" "$WORK/conf-on.log" \
      && pass "superopt configure completes" || { fail "superopt configure incomplete"; ok=0; }
  else
    fail "superopt-ON configure"; sed -n '1,60p' "$WORK/conf-on.log"
  fi
else
  echo "   (skipped: no re2c available for the Alive2 configure gate)"
fi

# The z3/alive2 checkouts are now patched; restore them for the commit.
git -C "$REPO/third_party/z3" checkout -- . >/dev/null 2>&1 || true
git -C "$REPO/third_party/alive2" checkout -- . >/dev/null 2>&1 || true

printf '\n== results: %d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
