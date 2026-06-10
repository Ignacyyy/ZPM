#!/bin/bash
set -euo pipefail

readonly TARGET="/opt/ZPM"
readonly LOG="/var/log/ZPM_INETINSTALL.log"
readonly GITHUB_REPO="Zielina-Konrad-productions/ZPM"
readonly REQUIRED_DEPS_APT=(curl git wget python3 g++ sudo zip unzip sed gawk coreutils nano)
readonly REQUIRED_DEPS_ZYPPER=(curl git wget python3 gcc-c++ sudo zip unzip sed gawk coreutils nano)
readonly REQUIRED_DEPS_DNF=(curl git wget python3 gcc-c++ sudo zip unzip sed gawk coreutils nano)

die()  { echo "ERROR: $*" >&2; exit 1; }
info() { echo "[*] $*"; }
ok()   { echo "[+] $*"; }
warn() { echo "[!] WARNING: $*"; }

if [ "$(id -u)" -ne 0 ]; then
    die "This script must be run as root. Use: sudo $0"
fi

touch "$LOG" || die "Cannot write log: $LOG"
chmod 0600 "$LOG" || true
exec > >(tee -a "$LOG") 2>&1

echo "===== ZPM Internet Installer ====="

TMP=""
STAGING=""
BACKUP=""

cleanup() {
    local exit_code=$?
    if [ -n "${TMP:-}" ] && [ -d "$TMP" ]; then
        rm -rf "$TMP"
    fi
    if [ -n "${STAGING:-}" ] && [ -d "$STAGING" ]; then
        rm -rf "$STAGING"
    fi
    if [ "$exit_code" -ne 0 ]; then
        echo ""
        echo "Installation FAILED (exit code: $exit_code)"
        echo "See full log: $LOG"
    fi
    exit "$exit_code"
}
trap cleanup EXIT

ask() {
    local _var="$1" _prompt="$2" _answer
    while true; do
        read -rp "$_prompt [y/n] " _answer
        case "$_answer" in
            [yY]) printf -v "$_var" "y"; return ;;
            [nN]) printf -v "$_var" "n"; return ;;
            *) echo "  Please answer y or n." ;;
        esac
    done
}

detect_pm() {
    if [ -f /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        case "${ID_LIKE:-} ${ID:-}" in
            *debian*|*ubuntu*) echo "apt";    return ;;
            *suse*|*opensuse*) echo "zypper"; return ;;
            *fedora*|*rhel*|*centos*|*rocky*|*alma*) echo "dnf"; return ;;
        esac
    fi
    command -v apt-get &>/dev/null && echo "apt"    && return
    command -v zypper  &>/dev/null && echo "zypper" && return
    command -v dnf     &>/dev/null && echo "dnf"    && return
    echo "unknown"
}

install_dependencies() {
    local pm="$1"
    case "$pm" in
        apt)    DEPS=("${REQUIRED_DEPS_APT[@]}") ;;
        zypper) DEPS=("${REQUIRED_DEPS_ZYPPER[@]}") ;;
        dnf)    DEPS=("${REQUIRED_DEPS_DNF[@]}") ;;
        *)      die "Unsupported package manager: $pm" ;;
    esac

    echo ""
    echo "====== ZPM Dependencies ======="
    printf '  - %s\n' "${DEPS[@]}"
    echo ""

    ask dep "Install dependencies?"
    if [ "$dep" != "y" ]; then
        warn "Skipping dependency installation. Build may fail if packages are missing."
        return
    fi

    case "$pm" in
        apt)
            export DEBIAN_FRONTEND=noninteractive
            local apt_wait=0
            if command -v fuser &>/dev/null; then
                while fuser /var/lib/dpkg/lock-frontend >/dev/null 2>&1; do
                    info "Waiting for dpkg lock... (${apt_wait}s)"
                    sleep 2
                    apt_wait=$((apt_wait + 2))
                    [ "$apt_wait" -ge 60 ] && die "dpkg lock held for over 60s."
                done
            fi
            info "Updating package lists..."
            apt-get update -y >> "$LOG" 2>&1 || die "apt-get update failed."
            info "Installing dependencies..."
            apt-get install -y "${DEPS[@]}" >> "$LOG" 2>&1 || {
                warn "APT installation interrupted. Trying to recover..."
                dpkg --configure -a >> "$LOG" 2>&1 || true
                apt-get install -f -y >> "$LOG" 2>&1 || true
                die "Failed to install dependencies even after recovery attempt."
            }
            ;;
        zypper)
            info "Refreshing repositories..."
            zypper --non-interactive refresh >> "$LOG" 2>&1 || die "zypper refresh failed."
            info "Installing dependencies..."
            zypper --non-interactive install -y "${DEPS[@]}" >> "$LOG" 2>&1 || {
                warn "Zypper transaction failed. Running verification/repair..."
                zypper --non-interactive verify -y >> "$LOG" 2>&1 || true
                die "Failed to install dependencies on Zypper."
            }
            ;;
        dnf)
            info "Installing dependencies..."
            dnf install -y "${DEPS[@]}" >> "$LOG" 2>&1 || {
                warn "DNF transaction failed. Cleaning metadata..."
                dnf clean all >> "$LOG" 2>&1 || true
                die "Failed to install dependencies on DNF."
            }
            ;;
    esac

    ok "Dependencies installed successfully."
}

fetch_release() {
    TMP="$(mktemp -d /tmp/ZPM_INETINSTALL.XXXXXX)"

    info "Fetching latest version from GitHub..."
    LATEST=$(curl -fsSL --connect-timeout 10 --max-time 30 \
        "https://api.github.com/repos/${GITHUB_REPO}/releases/latest" \
        | grep -m1 '"tag_name":' \
        | sed -E 's/.*"tag_name":[[:space:]]*"([^"]+)".*/\1/') \
        || die "Failed to reach GitHub API. Check your internet connection."

    [ -n "${LATEST:-}" ] || die "Could not determine latest version."
    [[ "$LATEST" =~ ^v?[A-Za-z0-9._-]+$ ]] || die "Unsafe release tag from GitHub: $LATEST"
    ok "Latest version: $LATEST"

    local tarball="$TMP/${LATEST}.tar.gz"
    local url="https://github.com/${GITHUB_REPO}/archive/refs/tags/${LATEST}.tar.gz"

    info "Downloading ${LATEST}.tar.gz..."
    curl -fsSL --retry 3 --retry-delay 1 --connect-timeout 10 --max-time 180 \
        "$url" -o "$tarball" || die "Download failed. URL: $url"
    [ -s "$tarball" ] || die "Downloaded archive is empty or corrupt: $tarball"

    info "Extracting archive..."
    tar -xzf "$tarball" -C "$TMP" >> "$LOG" 2>&1 || die "Failed to extract archive."

    EXTRACTED_DIR=$(find "$TMP" -mindepth 1 -maxdepth 1 -type d -name "ZPM-*" | head -n1)
    [ -n "$EXTRACTED_DIR" ] || die "Extraction produced no ZPM-* directory."
    [ -f "$EXTRACTED_DIR/src/build.sh" ] || die "Archive is missing src/build.sh."
    [ -f "$EXTRACTED_DIR/src/common/main.h" ] || die "Archive is missing src/common/main.h."

    ok "Extracted to: $EXTRACTED_DIR"
}

preserve_config() {
    if [ -f "$TARGET/zielina.conf" ]; then
        cp -f "$TARGET/zielina.conf" "$STAGING/zielina.conf" \
            || die "Could not preserve existing zielina.conf"
    elif [ -f "$STAGING/.zielina.conf.default" ] && [ ! -f "$STAGING/zielina.conf" ]; then
        cp -f "$STAGING/.zielina.conf.default" "$STAGING/zielina.conf" \
            || die "Could not install default zielina.conf"
    fi
}

build_staging() {
    local build_script="$STAGING/src/build.sh"
    [ -f "$build_script" ] || die "build.sh not found at: $build_script"
    chmod +x "$build_script"

    info "Recompiling ZPM in staging directory..."
    (cd "$STAGING/src" && bash build.sh) || die "Compilation failed. Existing installation was not touched."
    ok "Recompilation complete."
}

update_symlinks() {
    info "Updating symlinks in /usr/bin..."

    find /usr/bin -maxdepth 1 -type l | while read -r link; do
        local target_link
        target_link=$(readlink "$link" 2>/dev/null || true)
        if [[ "$target_link" == "${TARGET}/bin/"* ]]; then
            rm -f "$link" || warn "Could not remove old symlink: $link"
        fi
    done

    if [ ! -d "$TARGET/bin" ]; then
        warn "bin/ is missing; no symlinks created."
        return
    fi

    for bin_file in "$TARGET/bin"/z*; do
        [ -f "$bin_file" ] || continue
        local bin_name link
        bin_name=$(basename "$bin_file")
        link="/usr/bin/${bin_name}"

        if [ -e "$link" ] && [ ! -L "$link" ]; then
            warn "Refusing to overwrite non-symlink: $link"
            continue
        fi

        ln -sfn "$bin_file" "$link" || warn "Could not create symlink for: $bin_name"
    done
    ok "Symlinks updated."
}

rollback() {
    if [ -n "${BACKUP:-}" ] && [ -d "$BACKUP" ]; then
        rm -rf "$TARGET"
        mv "$BACKUP" "$TARGET" || true
    fi
}

commit_staging() {
    if [ -L "$TARGET" ]; then
        die "Refusing to replace symlink: $TARGET"
    fi

    if [ -e "$TARGET" ]; then
        BACKUP="$(mktemp -d /opt/ZPM.backup.XXXXXX)"
        rmdir "$BACKUP"
        mv "$TARGET" "$BACKUP" || die "Could not move existing installation to backup."
    fi

    mv "$STAGING" "$TARGET" || {
        rollback
        die "Could not activate new installation."
    }
    STAGING=""

    update_symlinks || {
        rollback
        die "Could not update symlinks."
    }

    VERSION_CLEAN="${LATEST#v}"
    echo "$VERSION_CLEAN" > "$TARGET/VERSION.txt" || warn "Could not write VERSION.txt"
    rm -f "$TARGET/PREVERSION.txt" 2>/dev/null || true

    if [ -n "${BACKUP:-}" ]; then
        rm -rf "$BACKUP"
        BACKUP=""
    fi
}

PM=$(detect_pm)
case "$PM" in
    apt)    info "Detected package manager: APT (Debian/Ubuntu)" ;;
    zypper) info "Detected package manager: Zypper (openSUSE/SLES)" ;;
    dnf)    info "Detected package manager: DNF (Fedora/RHEL/Rocky/Alma)" ;;
    *)      die "Unsupported system. ZPM requires apt, zypper, or dnf." ;;
esac

ask confirm "Start installation of ZPM?"
if [ "$confirm" != "y" ]; then
    echo "Installation cancelled."
    exit 0
fi

install_dependencies "$PM"
fetch_release

echo ""
info "Preparing staging installation..."
STAGING="$(mktemp -d /opt/ZPM.install.XXXXXX)"
cp -a "$EXTRACTED_DIR/." "$STAGING/" || die "Failed to copy files to staging directory."
preserve_config
build_staging
commit_staging

echo ""
echo "Installation complete!"
echo "type zhome to gain more info about ZPM"
printf "Version : %-34s\n" "${VERSION_CLEAN:-unknown}"
printf "Path    : %-34s\n" "$TARGET"
printf "Log     : %-34s\n" "$LOG"
