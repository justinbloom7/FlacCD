#!/usr/bin/env bash
set -euo pipefail
sourceRoot="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
toolsRoot="$sourceRoot/build-windows-tools"
archiveDirectory=""
headless=false
installPackages=false
while (($#)); do
   case "$1" in
      --tools-dir|--archive-dir)
         (($# >= 2)) || { printf 'Missing value for %s\n' "$1" >&2; exit 1; }
         if [[ "$1" == --tools-dir ]]; then toolsRoot="$2"; else archiveDirectory="$2"; fi
         shift 2 ;;
      --headless) headless=true; shift ;;
      --install-system-packages) installPackages=true; shift ;;
      --help|-h)
         printf '%s\n' 'Run on CentOS Stream 9: scripts/SetupWindowsBuild.sh [--install-system-packages] [--tools-dir DIR] [--archive-dir DIR] [--headless]' \
            'Downloads pinned LLVM-MinGW, Wine WoW64 and Inno Setup into a managed local directory.' \
            'Only --install-system-packages uses sudo; normal setup/builds run as your user.'
         exit 0 ;;
      *) printf 'Unknown option: %s\n' "$1" >&2; exit 1 ;;
   esac
done
if $installPackages; then
   # Administrative changes require the caller's explicit option.
   sudo dnf install -y dnf-plugins-core epel-release
   sudo dnf config-manager --set-enabled crb
   sudo dnf install -y wine-core wine-common wine-filesystem wine-fonts cmake ninja-build \
      pkgconf-pkg-config binutils curl tar xz python3 xorg-x11-server-Xvfb
fi
for program in cmake pkg-config objdump; do
   command -v "$program" >/dev/null || {
      printf 'Missing %s. Run setup with --install-system-packages, or ask an administrator to install prerequisites.\n' "$program" >&2
      exit 1
   }
done
command -v ninja >/dev/null || command -v ninja-build >/dev/null || { printf 'Ninja is required.\n' >&2; exit 1; }
command=(cmake "-DFLACCD_TOOLS_ROOT=$toolsRoot")
if [[ -n "$archiveDirectory" ]]; then command+=("-DFLACCD_ARCHIVE_DIRECTORY=$archiveDirectory"); fi
command+=(-P "$sourceRoot/cmake/SetupWindowsBuild.cmake")
if $headless || [[ -z "${DISPLAY:-}" ]]; then
   command -v xvfb-run >/dev/null || { printf 'No display: install Xvfb or run from a graphical session.\n' >&2; exit 1; }
   exec xvfb-run -a "${command[@]}"
fi
exec "${command[@]}"
