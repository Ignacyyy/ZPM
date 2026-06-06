#!/bin/bash
# ───────── root check ─────────
if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root. Use: sudo"
    exit 1
fi

set -e

# Upewnij się, że katalog na pliki binarne istnieje
mkdir -p /opt/ZPM/bin

cd /opt/ZPM/src
echo "---------------------------Building ZPM---------------------------"

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
    
    # Kompilacja przy użyciu sprawnego GCC 14 z C++20
    g++ "$file" -std=c++20 -O2 -I /opt/ZPM/src/common -o "$out"
    
    # Przenoszenie i linkowanie
    mv -f "$out" /opt/ZPM/bin/
    ln -sf "/opt/ZPM/bin/$out_name" "/usr/bin/$out_name"
    
    echo "Installed $out_name"
done

echo "-----------------------------DONE---------------------------------"
