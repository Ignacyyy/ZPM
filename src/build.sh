#!/bin/bash
set -euo pipefail

# 1. FORCE THE SYSTEM DEFAULT COMPILER
CXX_CMD="g++"

echo "Using system compiler: $($CXX_CMD --version | head -n 1)"

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
