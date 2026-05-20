#!/bin/bash
set -euo pipefail

# ── CONSTANTS ─────────────────────────────────────────────────────────────────
readonly LOG="/tmp/ZMP_INETINSTALL.log"
readonly TMP="/tmp/ZMP_INETINSTALL_$$"
readonly TARGET="/opt/ZPM"
readonly GITHUB_REPO="Zielina-Konrad-productions/ZPM"
readonly REQUIRED_DEPS=(curl git wget python3 g++ sudo)

# ── LOGGING ───────────────────────────────────────────────────────────────────
exec > >(tee -a "$LOG") 2>&1
echo "===== ZPM Internet Installer ====="

# ── CLEANUP TRAP ──────────────────────────────────────────────────────────────
cleanup() {
    local exit_code=$?
    rm -rf "$TMP"

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
    # ask <variable_name> <prompt>
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
    local_wait=0
    while fuser /var/lib/dpkg/lock-frontend >/dev/null 2>&1; do
        info "Waiting for dpkg lock... (${local_wait}s)"
        sleep 2
        local_wait=$((local_wait + 2))
        if [ "$local_wait" -ge 60 ]; then
            die "dpkg lock held for over 60s. Another process may be using apt."
        fi
    done

    info "Updating package lists..."
    apt-get update -y >> "$LOG" 2>&1 \
        || die "apt-get update failed. Check your internet connection."

    info "Installing dependencies..."
    apt-get install -y "${REQUIRED_DEPS[@]}" >> "$LOG" 2>&1 \
        || die "Failed to install dependencies."

    ok "Dependencies installed successfully."
else
    warn "Skipping dependency installation. The build may fail if packages are missing."
fi

# ── FETCH LATEST VERSION ──────────────────────────────────────────────────────
echo ""
info "Fetching latest version from GitHub..."

LATEST=$(curl -fsSL "https://api.github.com/repos/${GITHUB_REPO}/releases/latest" \
    | grep '"tag_name"' \
    | cut -d '"' -f4) || die "Failed to reach GitHub API. Check your internet connection."

if [ -z "${LATEST:-}" ]; then
    die "Could not determine latest version. The GitHub API response was empty or malformed."
fi

ok "Latest version: $LATEST"

# ── PREPARE TMP DIR ───────────────────────────────────────────────────────────
mkdir -p "$TMP" || die "Failed to create temp directory: $TMP"
cd "$TMP" || die "Failed to enter temp directory: $TMP"

# ── DOWNLOAD ──────────────────────────────────────────────────────────────────
TARBALL="${LATEST}.tar.gz"
DOWNLOAD_URL="https://github.com/${GITHUB_REPO}/archive/refs/tags/${LATEST}.tar.gz"

info "Downloading $TARBALL..."
if ! curl -fsSL "$DOWNLOAD_URL" -o "$TARBALL"; then
    die "Download failed. URL: $DOWNLOAD_URL"
fi

# Verify the tarball is not empty/corrupt
if [ ! -s "$TARBALL" ]; then
    die "Downloaded archive is empty or corrupt: $TARBALL"
fi

# ── EXTRACT ───────────────────────────────────────────────────────────────────
info "Extracting archive..."
tar -xzf "$TARBALL" >> "$LOG" 2>&1 || die "Failed to extract archive: $TARBALL"

# Find extracted directory (GitHub names it ZPM-<version>)
EXTRACTED_DIR=$(find "$TMP" -maxdepth 1 -type d -name "ZPM-*" | head -n1)
if [ -z "$EXTRACTED_DIR" ]; then
    die "Extraction produced no ZPM-* directory. Archive may be corrupt."
fi

ok "Extracted to: $EXTRACTED_DIR"
cd "$EXTRACTED_DIR" || die "Failed to enter extracted directory."

# ── INSTALL TO TARGET ─────────────────────────────────────────────────────────
info "Installing to ${TARGET}..."

# Remove existing installation
if [ -d "$TARGET" ]; then
    info "Existing installation found. Removing ${TARGET}..."
    rm -rf "$TARGET" || die "Failed to remove existing installation: $TARGET"
    ok "Old installation removed."
fi

mkdir -p "$TARGET" || die "Failed to create target directory: $TARGET"
cp -r . "$TARGET/" || die "Failed to copy files to $TARGET"
ok "Files copied to $TARGET"

# ── BIN PERMISSIONS ───────────────────────────────────────────────────────────
if [ -d "$TARGET/bin" ] && [ -n "$(ls -A "$TARGET/bin" 2>/dev/null)" ]; then
    info "Setting executable permissions on binaries..."
    find "$TARGET/bin" -type f -exec chmod +x {} + \
        || warn "Could not set permissions on some binaries."
else
    warn "No bin/ directory found (or it is empty). No binaries to configure."
fi

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

# ── ARM RECOMPILATION ─────────────────────────────────────────────────────────
echo ""
echo "===== ZPM ARM Compatibility ====="

rec="n"
ARCH=$(uname -m)
if [[ "$ARCH" == arm* ]] || [[ "$ARCH" == aarch64* ]]; then
    info "ARM architecture detected ($ARCH). Recompilation is necessary."
    ask rec "Recompile ZPM for ARM?"
else
    info "Architecture: $ARCH (non-ARM). Skipping recompilation."
fi

if [ "$rec" = "y" ]; then
    BUILD_SCRIPT="$TARGET/src/build.sh"
    if [ ! -f "$BUILD_SCRIPT" ]; then
        die "build.sh not found at: $BUILD_SCRIPT"
    fi
    if [ ! -x "$BUILD_SCRIPT" ]; then
        chmod +x "$BUILD_SCRIPT" || die "Could not make build.sh executable."
    fi
    info "Recompiling ZPM (this may take a while)..."
    cd "$TARGET/src" || die "Failed to enter $TARGET/src"
    if bash build.sh >> "$LOG" 2>&1; then
        ok "Recompilation complete."
    else
        warn "Recompilation failed. Check the log for details: $LOG"
        warn "The pre-compiled binaries (if any) will be used instead."
    fi
    cd "$TARGET" || true
fi

# ── WRITE VERSION ─────────────────────────────────────────────────────────────
VERSION_CLEAN="${LATEST#v}"
echo "$VERSION_CLEAN" > "$TARGET/VERSION.txt" \
    || warn "Could not write VERSION.txt"
rm -f "$TARGET/PREVERSION.txt" 2>/dev/null || true

# ── DONE ──────────────────────────────────────────────────────────────────────
info "Cleaning up temporary files..."
rm -rf "$TMP"

echo ""
echo "Installation complete!"
echo "type zhelp to gain more info about ZPM"
printf  "Version : %-34s\n" "$VERSION_CLEAN"
printf  "Path    : %-34s\n" "$TARGET"
printf  "Log     : %-34s\n" "$LOG"

