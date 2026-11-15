#!/usr/bin/env bash
# The local gate (run it in the pinned toolchain: tools/container.sh tools/gate.sh [JOB...]).
# Jobs, in order: format, build, asan, fuzz, licence. Exit status 1 when any job fails; a summary of
# every job is printed at the end.
#   FUZZ_SECONDS      seconds of fuzzing (default 60)
#   IMGUI_DIR         Dear ImGui sources (set by the container image)
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
JOBS=("$@")
[[ ${#JOBS[@]} -gt 0 ]] || JOBS=(format build asan fuzz licence)
FUZZ_SECONDS=${FUZZ_SECONDS:-60}
export SDL_VIDEO_DRIVER=dummy

# No clang-format style is adopted; tracked text files must be free of tabs, trailing blanks and CRs
# and end in a newline.
format() {
  local f rc=0
  mapfile -t files < <(git ls-files -- . ':!fuzz/corpus')
  if grep -n -I -E $'\t| +$|\r' -- "${files[@]}"; then rc=1; fi
  for f in "${files[@]}"; do
    if [[ -s $f && $(tail -c1 "$f" | od -An -c | tr -d ' ') != '\n' ]]; then echo "$f: no final newline"; rc=1; fi
  done
  return $rc
}
configure() {  # <build dir> <cmake args...>
  local dir=$1
  shift
  cmake -S . -B "$dir" -G Ninja -DIMGUI_ACCESSKIT_IMGUI_DIR="${IMGUI_DIR:-}" "$@" >/dev/null
}
# GCC 14, strict warnings as errors, every module, the GoogleTest suites.
build() {
  CXX=g++-14 configure build/gcc -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    && cmake --build build/gcc \
    && ctest --test-dir build/gcc --output-on-failure
}
# clang 19 with AddressSanitizer (leak checking on; full malloc stacks so tools/lsan.supp matches) and
# UndefinedBehaviorSanitizer, every module.
asan() {
  local san="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
  CXX=clang++-19 configure build/asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="$san" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
    && cmake --build build/asan \
    && ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1:fast_unwind_on_malloc=0 \
       LSAN_OPTIONS=suppressions="$PWD/tools/lsan.supp":print_suppressions=0 UBSAN_OPTIONS=print_stacktrace=1 \
       ctest --test-dir build/asan --output-on-failure
}
# libFuzzer (with ASan and UBSan) on the CEF tree mirror, configured as a cef-only consumer would be:
# no SDL3, Dear ImGui, AccessKit or Pango.
fuzz() {
  CXX=clang++-19 configure build/fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo -DIMGUI_ACCESSKIT_BRIDGE=OFF \
      -DIMGUI_ACCESSKIT_TEXT=OFF -DIMGUI_ACCESSKIT_FUZZ=ON \
    && cmake --build build/fuzz || return 1
  mkdir -p build/fuzz-corpus build/fuzz-findings
  build/fuzz/fuzz_cef_tree -max_total_time="$FUZZ_SECONDS" -rss_limit_mb=2048 -timeout=30 -print_final_stats=1 \
    -artifact_prefix=build/fuzz-findings/cef_tree- build/fuzz-corpus fuzz/corpus/cef_tree
}
licence() {
  [[ -x build/gcc/text_test ]] || build >/dev/null || return 1
  tools/licences.sh build/gcc/bridge_test build/gcc/text_test build/gcc/cef_tree_test
}

declare -A RESULT
fail=0
for j in "${JOBS[@]}"; do
  echo "=== $j"
  start=$SECONDS
  if "$j"; then RESULT[$j]="PASS $((SECONDS - start))s"; else RESULT[$j]="FAIL $((SECONDS - start))s"; fail=1; fi
done
echo "=== summary"
for j in "${JOBS[@]}"; do echo "$j: ${RESULT[$j]}"; done
exit $fail
