#!/usr/bin/env bash
set -euo pipefail
sourceRoot="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
toolsRoot="$sourceRoot/build-windows-tools"
releaseRoot="$sourceRoot/build-release-windows"
headless=false
extraArguments=()
while (($#)); do
   case "$1" in
      --tools-dir|--release-dir)
         (($# >= 2)) || { printf 'Missing value for %s\n' "$1" >&2; exit 1; }
         if [[ "$1" == --tools-dir ]]; then toolsRoot="$2"; else releaseRoot="$2"; fi
         shift 2 ;;
      --headless) headless=true; shift ;;
      --) shift; extraArguments=("$@"); break ;;
      --help|-h)
         printf '%s\n' 'Run on Linux: scripts/BuildWindows.sh [--tools-dir DIR] [--release-dir DIR] [--headless] [-- -DNAME=value ...]' \
            'Builds the Windows applications, audited deployment and Inno installer using prepared tools.' \
            'Run scripts/SetupWindowsBuild.sh first. No administrator privileges are needed.'
         exit 0 ;;
      *) printf 'Unknown option: %s\n' "$1" >&2; exit 1 ;;
   esac
done
[[ -f "$toolsRoot/WindowsBuildSettings.cmake" ]] || {
   printf 'Build settings missing. Run scripts/SetupWindowsBuild.sh --tools-dir "%s" first.\n' "$toolsRoot" >&2
   exit 1
}
for argument in "${extraArguments[@]}"; do
   [[ "$argument" == -D* ]] || { printf 'Only CMake -D settings may follow --.\n' >&2; exit 1; }
done
export WINEDLLOVERRIDES='mscoree,mshtml,winegstreamer='
command=(cmake "-DFLACCD_BUILD_SETTINGS=$toolsRoot/WindowsBuildSettings.cmake"
   "-DFLACCD_RELEASE_ROOT=$releaseRoot" "${extraArguments[@]}" -P "$sourceRoot/cmake/BuildWindowsRelease.cmake")
if $headless || [[ -z "${DISPLAY:-}" ]]; then
   command -v xvfb-run >/dev/null || { printf 'No display: install Xvfb or run from a graphical session.\n' >&2; exit 1; }
   exec xvfb-run -a "${command[@]}"
fi
exec "${command[@]}"
