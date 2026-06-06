#!/bin/bash
set -euo pipefail

# 1. DETECT BEST AVAILABLE C++20 COMPILER
# List of potential modern compiler binaries from GCC 15 down to 10
POSSIBLE_COMPILERS=("g++-15" "g++-14" "g++-13" "g++-12" "g++-11" "g++-10" "g++")
CXX_CMD=""

for cmd in "${POSSIBLE_COMPILERS[@]}"; do
    # Check if the compiler binary exists in the system PATH
    if command -v "$cmd" &>/dev/null; then
        # Perform a test compilation to verify true C++20 compliance
        if "$cmd" -std=c++20 -x c++ /dev/null -o /dev/null 2>/dev/null; then
            CXX_CMD="$cmd"
            break # Found a valid modern compiler, stop searching
        fi
    fi
done

# Fallback error if no C++20 capable compiler is detected
if [ -z "$CXX_CMD" ]; then
    echo "ERROR: No compiler supporting the C++20 standard was found!"
    echo "If you are on openSUSE, please ensure gcc13-c++ or gcc14-c++ is installed."
    exit 1
fi

echo "Selected compiler for C++20: $CXX_CMD"

# 2. CREATE TARGET DIRECTORY FOR BINARIES
mkdir -p /opt/ZPM/bin

# 3. COMPILATION LOOP
cd /opt/ZPM/src
echo "---------------------------Building ZPM---------------------------"

for file in *.cpp; do
    [ -e "$file" ] || continue
    name="${file%.cpp}"
    
    # Handle naming mapping for the main manager executable
    if [[ "$name" == "ZPM" ]]; then
        out_name="zpm"
    else
        out_name="$name"
    fi
    
    out="/tmp/$out_name"
    echo "Compiling $file -> $out_name"
    
    # Compile using the dynamically detected modern compiler binary
    $CXX_CMD "$file" -std=c++20 -O2 -I /opt/ZPM/src/common -o "$out"
    
    # Deploy compiled binaries to the system location
    mv -f "$out" /opt/ZPM/bin/
    echo "Installed $out_name"
done

echo "-----------------------------DONE---------------------------------"
