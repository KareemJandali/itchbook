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

build_with() {   # $1 dir, $2 cc, $3 cxx, $4 build type
    local dir=$1 cc=$2 cxx=$3 type=$4
    if ! command -v "$cxx" >/dev/null 2>&1; then
        echo "  SKIP: $cxx not installed"
        return 0
    fi
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

# Clang's sanitizer runtime is a separate package and is often absent on a dev
# box. Release still exercises the whole front end, which is what the second
# compiler is here for.
step "clang, Release (front end; sanitizer runtime not required)"
build_with build-verify-clang clang clang++ Release

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
    for s in census-report phase9-report phase10-report phase10-8-report paper-report; do
        if ! python3 "scripts/$s.py" --check; then
            echo "  FAILED: $s --check against tracked files only"
            echo "  (it may pass in your working tree off an untracked artifact)"
            exit 1
        fi
    done
    # The paper's figures and its HTML build are generated too, and they decay
    # the same way. paper-figures.sh also refuses an ORPHAN -- a committed
    # figure whose artifact is not committed -- which is the figure-shaped
    # version of the bug that took CI red four times in phase 11.
    if ! ./scripts/paper-figures.sh --check; then
        echo "  FAILED: paper figures against tracked files only"
        exit 1
    fi
    if ! python3 scripts/paper-html.py --check; then
        echo "  FAILED: paper HTML is stale (run scripts/paper-html.py)"
        exit 1
    fi
) || fail=1

step "the regression gate"
check ./scripts/regression-gate.sh >/dev/null

echo
if [[ $fail -ne 0 ]]; then
    echo "verify-local FAILED — do not push"
    exit 1
fi
echo "verify-local PASSED (the long tail — differentials, adversarial matrix,"
echo "rate sweep — still runs only in CI)"
