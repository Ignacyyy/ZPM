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
    echo "Please update your compiler or use update-alternatives to switch to a newer version."
    exit 1
fi

# 2. CREATE TARGET DIRECTORY FOR BINARIES
mkdir -p /opt/ZPM/bin

# 3. COMPILATION LOOP
cd /opt/ZPM/src
echo "---------------------------Building ZPM---------------------------"

for file in *.cpp; do
    [ -e "$file" ] || continue
    name="${file%.cpp}"
    
    if [[ "$name" == "ZPM" ]]; then
        out_name="zpm"
    else
        out_name="$name"
    fi
    
    out="/tmp/$out_name"
    echo "Compiling $file -> $out_name"
    
    $CXX_CMD "$file" -std=c++20 -O2 -I /opt/ZPM/src/common -o "$out"
    
    mv -f "$out" /opt/ZPM/bin/
    echo "Installed $out_name"
done

echo "-----------------------------DONE---------------------------------"
