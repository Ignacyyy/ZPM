# Zielina Package Manager (ZPM)

> A modern, unified package management interface for Debian/Fedora/openSUSE (apt, dnf, zypper) — combining APT, Flatpak, and Snap into one fast, user-friendly tool.

---

## Installation

### Option 1 — Install from the internet (recommended)

```bash
sudo bash -c "$(curl -fsSL https://raw.githubusercontent.com/Zielina-Konrad-productions/ZPM/main/INETINSTALL.sh)"
```

### Option 2 — Manual install
```bash
go to ZPM directory
Grant execute permissions
execute INSTALL.sh inside ZPM directory with sudo, or as root
done :)
```

This installs ZPM to `/opt/ZPM` and creates symbolic links in `/usr/bin/`.

After installation, run `zhome` to see the full guide.

**Requirements:** `git`, `curl`, `wget`, `sudo`,`python3`... installed with the ZPM

---

## Commands

| Command        | Description                                   |
|----------------|-----------------------------------------------|
| `zpm`          | Main interface — run `zpm --help` for details |
| `zhome`        | Display home page                             |
| `zinst`        | Install a package                             |
| `zrm`          | Remove a package                              |
| `zupgr`        | Update ZPM itself                             |
| `zupd`         | Update system packages                        |
| `zlist`        | List installed packages                       |
| `zsearch`      | Search for a package                          |
| `zclean`       | Clean package cache                           |
| `zinfo`        | Show package information                      |
| `zuninstall`   | Uninstall ZPM                                 |
| `zrun`         | Run programs using ZPM                        |

---

```bash
# Update ZPM
sudo zupgr

# Uninstall ZPM
sudo zuninstall
```

---

## Info

| Property          | Value                                        |
|-------------------|----------------------------------------------|
| Version           | 2.1.0                                        |
| Supported distros | Debian, Fedora, openSUSE, (zypper, apt, dnf) |
| Installation path | `/opt/ZPM`                                   |
| Commands location | `/usr/bin/z*` (symbolic links)               |
| Language          | C++                                          |

---
## License

[MIT](LICENSE)

Copyright (c) 2026 Ignacyyy & Ry3ball
