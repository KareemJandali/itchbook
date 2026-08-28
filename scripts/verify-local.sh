#!/usr/bin/env bash
#
# verify-local.sh — everything CI checks that a laptop can check, in one command.
#
# This exists because of a specific failure. Phase 10's last commit went to CI
# red on a clang-only error -- `variable set but not used`, which gcc does not
# warn about and `-Werror` turns into a build failure -- after a local run that
# built with gcc three times and clang zero times. The gcc job in CI exists
# precisely to catch the reverse case, and the reason the repo builds with two
# compilers is that neither one sees everything. A local gate that uses one
# compiler is a local gate that finds out on push.
#
# BOTH COMPILERS, and clang without sanitizers when its runtime is missing: the
# point of the second compiler here is its FRONT END, and a clang build that
# cannot link is still a clang build that has parsed every line.
#
# It then earned its keep a second time, on a machine CI does not have. The
# first run of this script on macOS failed both builds on a line CI had passed
# green since phase 10.6:
#
#     std::printf("%" PRIu64, books.books());     // books() returns size_t
#
# On Linux x86-64 size_t and uint64_t are the same type, so that line is not
# merely tolerated, it is CORRECT and -Wformat is silent. On macOS uint64_t is
# `unsigned long long` and size_t is `unsigned long`, and -Werror=format
# rejects it. Two compilers catch what differs by front end; only two data
# models catch what differs by platform. RUN THIS ON YOUR OWN MACHINE, not
# just on the one CI happens to use -- there is now a macos-build-and-test job
# in CI because of this, but the local run is what found it.
#
# What this does NOT do is the long tail: the full-day differentials, the
# adversarial matrix, the rate sweep. Those live in CI and take minutes. This is
# the fast gate -- compile clean on both, all unit tests on both, every
# generated document still matching its artifacts.
#
# Usage: scripts/verify-local.sh
set -uo pipefail
cd "$(dirname "$0")/.."

fail=0
step() {
    printf '\n=== %s\n' "$1"
}
check() {
    if "$@"; then return 0; fi
    echo "  FAILED: $*"
    fail=1
    return 1
}

# What the compiler SAYS IT IS, not what it is spelled. On macOS /usr/bin/g++
# is a shim for Apple clang, so a script that runs `g++` and then `clang++` and
# prints two headings has run one front end twice while claiming two. That is a
# gate asserting coverage it does not have, which is the failure this whole
# script exists to prevent -- so it reports the identity and says so.
#
# The identity comes from the PREDEFINED MACROS, not from `--version`. A first
# attempt compared version strings and did not fire, because gcc prints the name
# it was invoked as -- `clang++ (Ubuntu 13.3.0)` when reached through a symlink
# -- so two spellings of one compiler produced two different strings. The macros
# describe the front end itself and cannot be fooled by the path. Apple clang
# also defines __GNUC__ (as 4.2.1), so __clang__ has to be tested first.
cxx_ident=""
identify() {
    local out
    out=$(echo | "$1" -E -dM -x c++ - 2>/dev/null) || { echo "unknown"; return; }
    if grep -q '__clang__' <<<"$out"; then
        printf 'clang %s' "$(grep '__clang_version__' <<<"$out" | cut -d'"' -f2)"
    else
        printf 'gcc %s.%s.%s' \
            "$(awk '/define __GNUC__ /{print $3}' <<<"$out")" \
            "$(awk '/define __GNUC_MINOR__ /{print $3}' <<<"$out")" \
            "$(awk '/define __GNUC_PATCHLEVEL__ /{print $3}' <<<"$out")"
    fi
}

build_with() {   # $1 dir, $2 cc, $3 cxx, $4 build type
    local dir=$1 cc=$2 cxx=$3 type=$4
    if ! command -v "$cxx" >/dev/null 2>&1; then
        echo "  SKIP: $cxx not installed"
        cxx_ident=""
        return 0
    fi
    cxx_ident=$(identify "$cxx")
    echo "  $cxx is: $cxx_ident"
    CC=$cc CXX=$cxx cmake -S . -B "$dir" -DCMAKE_BUILD_TYPE="$type" >/dev/null 2>&1
    local out
    out=$(cmake --build "$dir" -j 2>&1)
    if [[ $? -ne 0 ]]; then
        echo "$out" | grep -E "error:|warning:" | head -10
        echo "  FAILED: $cxx $type build"
        fail=1
        return 1
    fi
    if echo "$out" | grep -qE "warning:"; then
        echo "$out" | grep -E "warning:" | head -10
        echo "  FAILED: $cxx emitted warnings"
        fail=1
        return 1
    fi
    echo "  $cxx $type: clean"
    ctest --test-dir "$dir" 2>&1 | grep -E "tests passed|tests failed" | sed 's/^/  /'
    if ! ctest --test-dir "$dir" >/dev/null 2>&1; then
        echo "  FAILED: $cxx unit tests"
        fail=1
    fi
}

step "gcc, Debug + ASan/UBSan"
build_with build-verify-gcc gcc g++ Debug

# Phase 12.8's report is a Python program, so ctest cannot reach it. Its join,
# its sign gates and its gap-overlap census are tested against traces built by
# hand with defects planted in them -- a live run cannot test a refusal, because
# it produces whatever it produces and a silently wrong join still looks like a
# table of plausible latencies.
#
# It reads the fixture tests/test_tick_to_trade.cpp writes, which is how the C++
# structs and the Python reader are checked against each other rather than
# against two copies of a constant. ctest has run by now, so the fixture exists.
# The results document is generated from artifacts, never typed. --check
# regenerates and diffs, so a stale table fails rather than being noticed.
step "phase 12.8 results document matches its artifacts"
if python3 scripts/phase12-8-results.py --check; then
    :
else
    echo "  FAILED: docs/phase12-8-results.md is stale"
    fail=1
fi

step "phase 12.8 report: join, sign gates, census"
if python3 tests/test_report_join.py > /tmp/vl-report-join.txt 2>&1; then
    grep -c "^  ok" /tmp/vl-report-join.txt | sed 's/^/  checks passed: /'
else
    grep -E "^  FAIL|failure" /tmp/vl-report-join.txt | head -10
    echo "  FAILED: phase 12.8 report tests"
    fail=1
fi

# Clang's sanitizer runtime is a separate package and is often absent on a dev
# box. Release still exercises the whole front end, which is what the second
# compiler is here for.
step "clang, Release (front end; sanitizer runtime not required)"
gcc_ident=$cxx_ident
build_with build-verify-clang clang clang++ Release

if [[ -n $cxx_ident && $cxx_ident == "$gcc_ident" ]]; then
    echo
    echo "  NOTE: both invocations resolved to the SAME compiler:"
    echo "    $cxx_ident"
    echo "  This run exercised ONE front end, not two -- on macOS /usr/bin/g++"
    echo "  is a shim for Apple clang. Two build types is still worth having,"
    echo "  but real gcc is only covered by CI's gcc job. Install it with"
    echo "  \`brew install gcc\` and re-run with CXX=g++-14 for the second front end."
fi

# A GENERATED DOCUMENT MUST BE REPRODUCIBLE FROM TRACKED FILES ALONE, and that
# is checked in a copy containing only tracked files rather than in the working
# tree.
#
# Running the --check scripts here would pass on a document backed by a file
# that is untracked or ignored, because the file is sitting right there. That is
# not hypothetical: docs/phase10-results.md was made to read
# validation/tsc-offset.json, which was then gitignored as machine-specific. The
# check passed locally four times running and CI failed every one of them,
# because CI had no such file. Standing rule 7 says numbers reach a document
# from committed artifacts; an artifact nobody commits cannot back a committed
# document.
#
# `git ls-files` is the right set rather than HEAD: it is what a commit would
# contain, including edits not yet committed, so this tests what pushing would
# actually produce.
step "generated documents reproduce from TRACKED FILES ONLY"
tracked=$(mktemp -d)
trap 'rm -rf "$tracked"' EXIT
git ls-files -z | while IFS= read -r -d '' f; do
    mkdir -p "$tracked/$(dirname "$f")"
    cp "$f" "$tracked/$f"
done
(
    cd "$tracked" || exit 1
    for s in census-report phase9-report phase10-report phase10-8-report; do
        if ! python3 "scripts/$s.py" --check; then
            echo "  FAILED: $s --check against tracked files only"
            echo "  (it may pass in your working tree off an untracked artifact)"
            exit 1
        fi
    done
    # The paper is three generated things -- figures, tables, rendered page --
    # and paper-build.sh is the only place their order is written down. Checking
    # them individually here would let this gate pass in an order the build
    # cannot reproduce, which is the bug it exists to catch.
    if ! ./scripts/paper-build.sh --check; then
        echo "  FAILED: the paper does not reproduce from tracked files"
        exit 1
    fi
) || fail=1

step "the regression gate"
check ./scripts/split-replay-gate.sh >/dev/null
check ./scripts/regression-gate.sh >/dev/null

echo
if [[ $fail -ne 0 ]]; then
    echo "verify-local FAILED — do not push"
    exit 1
fi
echo "verify-local PASSED (the long tail — differentials, adversarial matrix,"
echo "rate sweep — still runs only in CI)"
