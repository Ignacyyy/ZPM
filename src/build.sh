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

# 1. Automatyczne szukanie najlepszego kompilatora (od GCC 15 w dół do g++)
CXX_CMD=""
for version in 15 14 13 12 11 10; do
    if command -v "g++-$version" &>/dev/null; then
        CXX_CMD="g++-$version"
        break
    fi
done

# Jeśli nie znaleziono g++-wersja, użyj domyślnego g++
if [ -z "$CXX_CMD" ]; then
    CXX_CMD="g++"
fi

echo "Selected compiler: $CXX_CMD"

# 2. Wykrywanie wersji kompilatora i ustawianie flag
# Sprawdzamy, czy wybrany kompilator bez problemu łyka C++20
if $CXX_CMD -std=c++20 -x c++ /dev/null -o /dev/null 2>/dev/null; then
    STD_FLAG="-std=c++20"
    FS_FLAG=""
    echo "Compiler supports C++20 mode."
# Jeśli nie, sprawdzamy czy obsługuje C++17
elif $CXX_CMD -std=c++17 -x c++ /dev/null -o /dev/null 2>/dev/null; then
    STD_FLAG="-std=c++17"
    echo "Compiler is too old for C++20. Falling back to C++17 mode."
    
    # Starsze GCC (np. 7.x, 8.x) wymagają jawnego linkowania biblioteki filesystem
    if $CXX_CMD -std=c++17 -x c++ /dev/null -o /dev/null -lstdc++fs 2>/dev/null; then
        FS_FLAG="-lstdc++fs"
    else
        FS_FLAG=""
    fi
else
    echo "ERROR: Your compiler ($CXX_CMD) is too ancient. Please install GCC 7 or newer."
    exit 1
fi

# 3. Główna pętla kompilacji
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
    
    # Kompilacja z dynamicznie dobranymi flagami ($STD_FLAG oraz $FS_FLAG)
    $CXX_CMD "$file" $STD_FLAG -O2 -I /opt/ZPM/src/common -o "$out" $FS_FLAG
    
    # Przenoszenie i linkowanie
    mv -f "$out" /opt/ZPM/bin/
    ln -sf "/opt/ZPM/bin/$out_name" "/usr/bin/$out_name"
    
    echo "Installed $out_name"
done

echo "-----------------------------DONE---------------------------------"
