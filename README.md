# Zielina Package Manager (ZPM)

> A modern, unified package management interface for Debian/Ubuntu, Fedora/RHEL-family, and openSUSE/SLES systems. ZPM wraps the native package manager (`apt`, `dnf`/`dnf5`, or `zypper`) and can also work with Flatpak and Snap when they are available.

The preferred interface is now:

```bash
zpm <command> [options]
```

Legacy `z*` commands are still installed for compatibility, but new examples and docs should use `zpm <command>`.

---

## Installation

### Install from the internet

```bash
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Zielina-Konrad-productions/ZPM/main/INETINSTALL.sh)"
```

### Manual install

```bash
git clone https://github.com/Zielina-Konrad-productions/ZPM.git
cd ZPM
chmod +x INSTALL.sh
sudo ./INSTALL.sh
```

The installer copies ZPM to `/opt/ZPM`, rebuilds the C++ binaries, and creates symlinks in `/usr/bin/`.

After installation, run:

```bash
zpm home
```

`zhome` still works, but `zpm home` is the recommended form.

### Requirements

The installer can install the required dependencies for supported systems. ZPM currently expects:

- `apt`, `dnf`/`dnf5`, or `zypper`
- `curl`, `git`, `wget`, `python3`, `sudo`
- `g++`/`gcc-c++` with C++20 support
- `zip`, `unzip`, `sed`, `gawk`, `coreutils`, `nano`

---

## Quick Start

```bash
# Show wrapper help
zpm --help

# Show command-specific help
zpm install --help
zpm remove --help

# Install or remove packages
sudo zpm install firefox
sudo zpm remove vlc
sudo zpm remove vlc --purge

# Search, inspect, list, and run
zpm search editor
zpm info firefox
zpm list --flatpak
zpm run firefox

# Update packages and ZPM itself
sudo zpm update --full --yes
sudo zpm upgrade

# Clean caches
sudo zpm clean
```

---

## Commands

| Recommended command | Short form | Legacy executable | Description |
|---------------------|------------|-------------------|-------------|
| `zpm home` | - | `zhome` | Show ZPM guide pages and configuration info |
| `zpm tui` | `zpm ztui` | `ztui` | Open the ZPM terminal UI |
| `zpm install <pkg...>` | `zpm inst` | `zinst` | Install packages from native PM / Flatpak / Snap |
| `zpm remove <pkg...>` | `zpm rm` | `zrm` | Remove packages from native PM / Flatpak / Snap |
| `zpm update` | `zpm upd` | `zupd` | Update system packages |
| `zpm upgrade` | `zpm upgr` | `zupgr` | Upgrade ZPM itself |
| `zpm list` | `zpm ls` | `zlist` | List installed packages |
| `zpm search <query>` | - | `zsearch` | Search for packages |
| `zpm info <pkg...>` | - | `zinfo` | Show package information |
| `zpm clean` | - | `zclean` | Clean package caches and unused data |
| `zpm uninstall` | - | `zuninstall` | Uninstall ZPM |
| `zpm run <pkg>` | - | `zrun` | Find and launch an installed program |

Most commands support `--help` and `--version`. For example:

```bash
zpm update --help
zpm list --help
```

---

## Common Options

| Command | Useful options |
|---------|----------------|
| `zpm install` | `--dry-run` |
| `zpm remove` | `--purge` / `-p` for APT, `--dry-run` |
| `zpm update` | `--full` / `-f`, `--yes` / `-y`, `--reboot` / `-r`, `--shutdown` / `-s`, `--dry-run` |
| `zpm upgrade` | `--force` / `-f`, `--experimental` / `-ex`, `--dry-run` |
| `zpm list` | `--native` / `-n`, `--flatpak` / `-f`, `--snap` / `-s`, `--no-pager` |
| `zpm search` | `--native` / `-n`, `--flatpak` / `-f`, `--snap` / `-s` |
| `zpm clean` | `--dry-run` |
| `zpm home` | `-p1`, `-p2`, `-p3`, `--edit-config` / `-ed` |

Use `sudo` for commands that change the system: install, remove, update, upgrade, clean, uninstall, and `zpm home --edit-config`. Dry-run modes do not modify packages or files.

---

## Package Sources

ZPM detects the native package manager automatically:

- Debian/Ubuntu: `apt`
- Fedora/RHEL-family: `dnf` or `dnf5`
- openSUSE/SLES: `zypper`

When Flatpak or Snap is installed, ZPM can include them in install, remove, search, list, info, clean, and run workflows. Install/remove commands present a source selection menu when a package can be handled by more than one source.

---

## Files and Paths

| Path | Purpose |
|------|---------|
| `/opt/ZPM` | ZPM installation directory |
| `/opt/ZPM/bin` | Compiled ZPM binaries |
| `/usr/bin/zpm` | Recommended wrapper command |
| `/usr/bin/z*` | Legacy compatibility symlinks |
| `/opt/ZPM/zielina.conf` | Runtime configuration |
| `/tmp/z*.log` | Runtime logs for package operations |
| `/var/log/ZPM_INSTALL.log` | Manual installer log |
| `/var/log/ZPM_INETINSTALL.log` | Internet installer log |

---

## Project Info

| Property | Value |
|----------|-------|
| Version | See `VERSION.txt` (`2.1.6` in this tree) |
| Main CLI | `zpm <command> [options]` |
| Legacy CLI | `zhome`, `zinst`, `zrm`, `zupd`, `zupgr`, `zlist`, `zsearch`, `zclean`, `zinfo`, `zuninstall`, `zrun` |
| Supported native PMs | `apt`, `dnf`/`dnf5`, `zypper` |
| Optional sources | Flatpak, Snap |
| Installation path | `/opt/ZPM` |
| Language | C++20 |

---

## License

[MIT](LICENSE)

Copyright (c) 2026 Ignacyyy & Ry3ball
