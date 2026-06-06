#!/bin/bash
# ───────── root check ─────────
if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root. Use: sudo"
    exit 1
fi
set -e
cd /opt/ZPM/src
echo "---------------------------Building ZPM---------------------------"

# Sprawdź czy g++ obsługuje C++20
if ! g++ -std=c++20 -x c++ /dev/null -o /dev/null 2>/dev/null; then
    echo "ERROR: g++ does not support C++20. Please install GCC 10 or newer."
    exit 1
fi

for file in *.cpp; do
    [ -e "$file" ] || continue
    name="${file%.cpp}"
    # ───────── special case ─────────
    if [[ "$name" == "ZPM" ]]; then
        out_name="zpm"
    else
        out_name="$name"
    fi
    out="/tmp/$out_name"
    echo "Compiling $file -> $out_name"
    g++ "$file" -std=c++20 -O2 -I /opt/ZPM/src/common -o "$out"
    mv -f "$out" /opt/ZPM/bin/
    ln -sf /opt/ZPM/bin/$out_name /usr/bin/$out_name
    echo "Installed $out_name"
done
echo "-----------------------------DONE---------------------------------"
