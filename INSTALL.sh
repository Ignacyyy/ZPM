#!/bin/bash
set -euo pipefail

# ── CONSTANTS ─────────────────────────────────────────────────────────────────
readonly LOG="/tmp/ZPM_INSTALL.log"
readonly TARGET="/opt/ZPM"
readonly REQUIRED_DEPS=(curl git wget python3 g++ sudo zip)

# ── LOGGING ───────────────────────────────────────────────────────────────────
exec > >(tee -a "$LOG") 2>&1
echo "===== ZPM Manual Installer ====="

# ── CLEANUP TRAP ──────────────────────────────────────────────────────────────
cleanup() {
    local exit_code=$?

    if [ "$exit_code" -ne 0 ]; then
        echo ""
        echo "Installation FAILED (exit code: $exit_code)$(printf '%*s' $((14 - ${#exit_code})) '')"
        echo "See full log: $LOG$(printf '%*s' $((30 - ${#LOG})) '')"
    fi

    exit "$exit_code"
}
trap cleanup EXIT

# ── HELPER FUNCTIONS ──────────────────────────────────────────────────────────
die() {
    echo "ERROR: $*" >&2
    exit 1
}

info() {
    echo "[*] $*"
}

ok() {
    echo "[+] $*"
}

warn() {
    echo "[!] WARNING: $*"
}

ask() {
    local _var="$1"
    local _prompt="$2"
    local _answer
    while true; do
        read -rp "$_prompt [y/n] " _answer
        case "$_answer" in
            [yY]) printf -v "$_var" "y"; return ;;
            [nN]) printf -v "$_var" "n"; return ;;
            *) echo "  Please answer y or n." ;;
        esac
    done
}

# ── ROOT CHECK ────────────────────────────────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
    die "This script must be run as root. Use: sudo $0"
fi

# ── OS CHECK ──────────────────────────────────────────────────────────────────
if ! command -v apt-get &>/dev/null; then
    die "apt-get not found. This installer supports Debian/Ubuntu-based systems only."
fi

# ── CONFIRM INSTALL ───────────────────────────────────────────────────────────
ask confirm "Start installation of ZPM?"
if [ "$confirm" != "y" ]; then
    echo "Installation cancelled."
    exit 0
fi

# ── SOURCE DIR ────────────────────────────────────────────────────────────────
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── DEPENDENCIES ──────────────────────────────────────────────────────────────
echo ""
echo "====== ZPM Dependencies ======="
echo "Required packages:"
for dep in "${REQUIRED_DEPS[@]}"; do
    echo "  - $dep"
done
echo ""

ask dep "Install dependencies?"

if [ "$dep" = "y" ]; then
    export DEBIAN_FRONTEND=noninteractive

    # ── WAIT FOR APT LOCK ──
    apt_wait=0
    while fuser /var/lib/dpkg/lock-frontend >/dev/null 2>&1; do
        info "Waiting for dpkg lock... (${apt_wait}s)"
        sleep 2
        apt_wait=$((apt_wait + 2))
        if [ "$apt_wait" -ge 60 ]; then
            die "dpkg lock held for over 60s. Another process may be using apt."
        fi
    done

    info "Updating package lists..."
    apt-get update -y >> "$LOG" 2>&1 \
        || die "apt-get update failed. Check your internet connection."

    info "Installing dependencies..."
    apt-get install -y "${REQUIRED_DEPS[@]}" > "$LOG" 2>&1 \
        || die "Failed to install dependencies."

    ok "Dependencies installed successfully."
else
    warn "Skipping dependency installation. The build may fail if packages are missing."
fi

# ── INSTALL TO TARGET ─────────────────────────────────────────────────────────
echo ""
info "Installing to ${TARGET}..."

# Remove existing installation
if [ -d "$TARGET" ]; then
    info "Existing installation found. Removing ${TARGET}..."
    rm -rf "$TARGET" || die "Failed to remove existing installation: $TARGET"
    ok "Old installation removed."
fi

mkdir -p "$TARGET" || die "Failed to create target directory: $TARGET"
cp -r "$SRC_DIR/." "$TARGET/" || die "Failed to copy files to $TARGET"
ok "Files copied to $TARGET"

# ── BIN PERMISSIONS ───────────────────────────────────────────────────────────
if [ -d "$TARGET/bin" ] && [ -n "$(ls -A "$TARGET/bin" 2>/dev/null)" ]; then
    info "Setting executable permissions on binaries..."
    find "$TARGET/bin" -type f -exec chmod +x {} + \
        || warn "Could not set permissions on some binaries."
else
    warn "No bin/ directory found (or it is empty). No binaries to configure."
fi

# ── RECOMPILATION ─────────────────────────────────────────────────────────────
echo ""
echo "===== ZPM Compilation ====="

BUILD_SCRIPT="$TARGET/src/build.sh"

if [ ! -f "$BUILD_SCRIPT" ]; then
    die "build.sh not found at: $BUILD_SCRIPT"
fi

if [ ! -x "$BUILD_SCRIPT" ]; then
    chmod +x "$BUILD_SCRIPT" || die "Could not make build.sh executable."
fi

info "Recompiling ZPM (this may take a while)..."
cd "$TARGET/src" || die "Failed to enter $TARGET/src"

if bash build.sh; then
    ok "Recompilation complete."
else
    echo "ERROR: Compilation failed! Check the log for details: $LOG"
    rm -rf "$TARGET"
    exit 1
fi

cd "$TARGET" || true

# ── SYMLINKS ──────────────────────────────────────────────────────────────────
info "Updating symlinks in /usr/bin..."

# Remove stale symlinks pointing to old TARGET/bin
find /usr/bin -maxdepth 1 -type l | while read -r link; do
    target_link=$(readlink "$link" 2>/dev/null || true)
    if [[ "$target_link" == "${TARGET}/bin/"* ]]; then
        rm -f "$link" || warn "Could not remove old symlink: $link"
    fi
done

# Create new symlinks
if [ -d "$TARGET/bin" ] && [ -n "$(ls -A "$TARGET/bin" 2>/dev/null)" ]; then
    for bin_file in "$TARGET/bin"/*; do
        [ -f "$bin_file" ] || continue
        bin_name=$(basename "$bin_file")
        ln -sf "$bin_file" "/usr/bin/${bin_name}" \
            || warn "Could not create symlink for: $bin_name"
    done
    ok "Symlinks created."
else
    warn "bin/ is empty — no symlinks created."
fi

# ── CLEAN PREVERSION STATE ────────────────────────────────────────────────────
rm -f "$TARGET/PREVERSION.txt" 2>/dev/null || true
rm -rf "$SRC_DIR"
cd ~
# ── DONE ──────────────────────────────────────────────────────────────────────
echo ""
echo "Installation complete!"
echo "type zhelp to gain more info about ZPM"
printf "Path    : %-34s\n" "$TARGET"
printf "Log     : %-34s\n" "$LOG"
