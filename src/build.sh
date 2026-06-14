#!/bin/bash
set -euo pipefail

# 1. FORCE THE SYSTEM DEFAULT COMPILER
CXX_CMD="g++"

# Check if compiler exists
if ! command -v "$CXX_CMD" &>/dev/null; then
    echo "ERROR: Compiler '$CXX_CMD' is not installed!"
    exit 1
fi

# Extract the major version number (e.g., "14" from "14.3.0" or "7" from "7.5.0")
GCC_VERSION=$($CXX_CMD -dumpversion | cut -d. -f1)

echo "Using system compiler: $($CXX_CMD --version | head -n 1)"

# Validate that the compiler is modern enough for C++20 (GCC 10 or newer)
if [ "$GCC_VERSION" -lt 10 ]; then
    echo "ERROR: Your system g++ version ($GCC_VERSION) is too old!"
    echo "ZPM requires GCC 10 or newer for full C++20 compliance."
    echo "Please update your system compiler."
    exit 1
fi

# 2. CREATE TARGET DIRECTORY FOR BINARIES
mkdir -p /opt/ZPM/bin
BUILD_TMP=$(mktemp -d /tmp/zpm-build-XXXXXX)
cleanup() {
    rm -rf "$BUILD_TMP"
}
trap cleanup EXIT

detect_jobs() {
    if [[ -n "${ZPM_BUILD_JOBS:-}" ]]; then
        if [[ "$ZPM_BUILD_JOBS" =~ ^[0-9]+$ && "$ZPM_BUILD_JOBS" -gt 0 ]]; then
            echo "$ZPM_BUILD_JOBS"
            return
        fi
        echo "Ignoring invalid ZPM_BUILD_JOBS value: $ZPM_BUILD_JOBS" >&2
    fi

    if command -v nproc &>/dev/null; then
        nproc
        return
    fi

    getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2
}

compile_one() {
    local file="$1"
    local name="${file%.cpp}"
    local out_name="$name"
    local out
    local extra_libs=()

    if [[ "$name" == "ZPM" ]]; then
        out_name="zpm"
    fi

    out="$BUILD_TMP/$out_name"
    echo "Compiling $file -> $out_name"

    if [[ "$name" == "ztui" ]]; then
        extra_libs=(-lftxui-component -lftxui-dom -lftxui-screen)
    fi

    "$CXX_CMD" "$file" -std=c++20 -O2 -I /opt/ZPM/src/common -o "$out" -pthread "${extra_libs[@]}"
    chmod 755 "$out"
}

# 3. COMPILATION LOOP
cd /opt/ZPM/src
echo "---------------------------Building ZPM---------------------------"

JOBS=$(detect_jobs)
if [[ ! "$JOBS" =~ ^[0-9]+$ || "$JOBS" -lt 1 ]]; then
    JOBS=1
fi
echo "Parallel build jobs: $JOBS"

running=0
build_failed=0

for file in *.cpp; do
    [ -e "$file" ] || continue

    while (( running >= JOBS )); do
        if ! wait -n; then
            build_failed=1
        fi
        running=$((running - 1))
    done

    if (( build_failed != 0 )); then
        break
    fi

    compile_one "$file" &
    running=$((running + 1))
done

while (( running > 0 )); do
    if ! wait -n; then
        build_failed=1
    fi
    running=$((running - 1))
done

if (( build_failed != 0 )); then
    echo "ERROR: One or more ZPM components failed to compile."
    exit 1
fi

for built in "$BUILD_TMP"/*; do
    [ -f "$built" ] || continue
    mv -f "$built" /opt/ZPM/bin/
    echo "Installed $(basename "$built")"
done

echo "-----------------------------DONE---------------------------------"
