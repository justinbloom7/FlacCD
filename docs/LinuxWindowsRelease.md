# Build the Windows release on Linux

**Run these scripts on CentOS Stream 9, not in Windows PowerShell.** They produce
Windows 10/11 x64 applications and the existing Inno Setup installer. Windows is
needed for integration testing, not for compilation or packaging.

## Two commands

From the repository root, prepare the prerequisites once:

```bash
scripts/SetupWindowsBuild.sh --install-system-packages
```

Then build the release whenever needed:

```bash
scripts/BuildWindows.sh
```

`--install-system-packages` explicitly permits the setup script to run `sudo dnf`
for one-time OS packages and enable CRB/EPEL. Your administrator can do this
instead; if those packages are already installed, omit the option. **Do not run
the entire setup or build script with sudo.** Downloads, Wine setup and normal
builds run as your user. Neither script changes global PATH.

Both commands work from other directories when invoked by their full paths.
Spaces and Unicode in paths are supported. `--help` lists the options. With no
DISPLAY they automatically use `xvfb-run`; pass `--headless` to force Xvfb in CI.
Xvfb must be installed before a headless setup/build can start.

## What prerequisite setup does

[SetupWindowsBuild.sh](../scripts/SetupWindowsBuild.sh) checks CMake, Ninja,
pkg-config and GNU objdump, then runs `cmake/SetupWindowsBuild.cmake`. Its optional
OS package step installs the build utilities, Python 3 for native test fixtures,
Wine host libraries/shared data/fonts, and Xvfb. The existing CMake 3.31.8 works;
the scripts do not upgrade the system compiler.

Setup downloads and SHA-256 verifies these pinned distributions:

| Tool | Version | Purpose |
|---|---|---|
| LLVM-MinGW UCRT, Linux x86_64 | 20240619 / Clang 18.1.8 | Windows x64 C/C++ compiler, linker, resources and runtime |
| Wine amd64-wow64 | 10.0 | Runs the installer compiler and its bootstrap |
| Inno Setup x64 | 7.1.1 | Compiles the existing `installer/FlacCD.iss` |

The default managed tool directory is `build-windows-tools/`. Archive URLs and
hashes are defined in `cmake/SetupWindowsBuild.cmake`; there are no floating
`latest` downloads. Completed downloads are cached under `Downloads/`.

Wine uses a separate `WinePrefix-10.0-Inno-7.1.1/` inside that directory, never
`~/.wine`. Setup disables optional Mono, Gecko and GStreamer integration for
these compiler processes, initializes a win64 prefix, and silently installs the
complete Inno distribution there, including ISCC's supporting DLLs. It does not
install FlacCD, ImgDrive, or a driver. It writes `Setup.log` and atomically
publishes `WindowsBuildSettings.cmake` only after preparation succeeds.

Repeated setup reuses owned tool directories and the installed compiler.
Checksum mismatches fail rather than being ignored. Existing unowned tool,
prefix or settings files are preserved; choose a fresh `--tools-dir` instead of
overwriting an earlier manual setup. Incomplete setup can be retried. Do not move
a prepared tool directory after building dependencies: their provenance includes
the compiler path.

EPEL's Wine 8 pure-64-bit configuration is **not** the selected loader. Inno 7's
[own installer checks for x86/WoW64 support](https://github.com/jrsoftware/issrc/blob/is-7_1_1/Projects/Src/Setup.MainFunc.pas#L3575-L3579),
even when installing the x64 compiler. The selected Wine distribution comes from
the independent [Kron4ek Wine-Builds project](https://github.com/Kron4ek/Wine-Builds),
built from vanilla Wine sources. It is a third-party binary distribution, not
an official WineHQ binary. Review that provenance before running setup.

## What the build script does

[BuildWindows.sh](../scripts/BuildWindows.sh) reads the prepared settings and
calls the existing `cmake/BuildWindowsRelease.cmake` driver. It:

1. Checks prerequisites and acquires the release lock.
2. Builds pinned static libFLAC 1.4.3 with the same UCRT toolchain.
3. Configures Release/Ninja and builds all Windows applications, tests and
   VERSIONINFO resources, preserving Evenstar Systems metadata.
4. Runs only host-labelled tests in the cross-build tree.
5. Streams the existing staging, recursive PE dependency audit, ZIP and Windows
   validation-bundle targets.
6. Runs the existing Inno script under Wine, translating paths with `winepath`.
7. Verifies the installer PE and its normal/delayed system imports, then publishes
   the completed installer with checksums, logs and build receipts.

libFLAC and the LLVM C++ runtimes are static for the pinned build. Linux library
and pkg-config searches are isolated from the Windows prefix. Windows system
DLLs/UCRT contracts are not bundled. ImgDrive remains an independently installed
Windows dependency. The installer script, AppId, per-user scope, Explorer
integration, restoration default, upgrade and safe-uninstall behavior are reused.
The native Windows/MSYS2 installer target still runs its existing smoke checks.

The build is incremental. It returns nonzero on any failed step and cannot
publish an incomplete installer as a new release. Previous successful artifacts
may remain after failure: check exit status and `ReleaseStatus.txt`. Locks
serialize repeated use of a release directory and the compiler prefix. Reproducible
inputs/process do not imply byte-identical output: timestamps can differ.

## Outputs

After a successful build, the last message identifies the installer to copy to
Windows, for example:

```text
-- Install this file on Windows: /path/to/FlacCD/build-release-windows/WindowsBuild/installer/Release/FlacCD-0.1.0-windows-x64-setup.exe
```

Run that `setup.exe` on Windows. `flaccd-agent.exe` is a supporting application
included by the installer, not the installer itself.

Default root: `build-release-windows/`:

- `Release.log`, `ReleaseStatus.txt`, `ReleaseArtifacts.txt`.
- `WindowsBuild/installer/Release/FlacCD-<version>-windows-x64-setup.exe`.
- Adjacent `.sha256`, `.build.txt`, `.iscc.log` files.
- `WindowsBuild/deploy/Release/FlacCD/` and its audited ZIP.
- `WindowsBuild/deploy/Release/WindowsValidation/` for Windows CTest.

An exact `vMAJOR.MINOR.PATCH` Git tag on HEAD supplies the version (for example,
`v0.1.1` produces `FlacCD-0.1.1-windows-x64-setup.exe`). Untagged builds, source
archives or builds without Git use the `0.1.1` fallback in
`cmake/ProjectVersion.cmake`. The build script reconfigures each run, so retagging
takes effect on the next build. Multiple release tags on HEAD are rejected.
Receipts hash ISCC, the Inno
script and the payload audit. Inno's default x86 bootstrap installs the x64
application payload on x64 Windows; it is a separate executable, not a library
mixed into the application. Application audits still reject delayed imports;
only the installer bootstrap has the additional delayed-system-import parser.

## Options and offline caches

```bash
scripts/SetupWindowsBuild.sh --tools-dir "$PWD/build-custom-tools" \
   --archive-dir /path/to/archive-cache
scripts/BuildWindows.sh --tools-dir "$PWD/build-custom-tools" \
   --release-dir "$PWD/build-custom-release" --headless
```

A prepopulated archive cache avoids tool downloads. It must contain the exact,
hash-matching files named by setup:

- `llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64.tar.xz`
- `wine-10.0-amd64-wow64.tar.xz`
- `innosetup-7.1.1-x64.exe`

Missing files are downloaded; corrupt files cause an error and are preserved for
inspection. To avoid downloading the libFLAC source during the build:

```bash
scripts/BuildWindows.sh -- \
   -DFLACCD_FLAC_SOURCE_ARCHIVE=/absolute/path/flac-1.4.3.tar.xz
```

Only CMake `-D` settings may follow `--`; these are passed as arguments, never
executed as shell text. Advanced callers can still invoke the CMake release
driver directly. Existing manual cross-builds and Windows/MSYS2 builds are not
migrated or modified by these scripts.

## Testing and Windows validation

Native Linux regression tests remain separate:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests cover wrapper quoting/headless forwarding, archive checksums and ownership,
Wine/ISCC invocation failures, partial/corrupt output, and PE delay-import bounds.
They do not require actual ImgDrive devices. **Do not run unrestricted CTest in
the Linux cross-build tree**: the remaining tests are Windows executables.

Use [WindowsCrossCompilation.md](WindowsCrossCompilation.md) to transfer the
installer/deployment/test bundle and run Windows CTest. Before release, test in
a Windows 10/11 x64 VM:

1. Clean-PATH CMD/PowerShell startup without MSYS2 runtime DLLs.
2. Per-user install, publisher, one ARP entry and Explorer command; preserve the
   user's default FLAC player.
3. Mount embedded-CUE FLACs with spaces/Unicode; play first/last tracks and skip
   tracks. Confirm the audio TOC and absence of an exported BIN.
4. Explorer eject and cleanup of only owned resources; preserve user FLAC/CUEs.
5. Reboot/crash recovery and default login remount; honor explicit eject/unmount.
6. Upgrade with an active mount and with restoration explicitly disabled;
   preserve backing resources and the user's preference.
7. Uninstall with/without active mounts; preserve cache/user data as required.
8. Install without ImgDrive; check the advisory and actionable mount errors.

Compilation under Wine is not Windows integration verification. Headless Xvfb
and this newly generated installer's VM lifecycle also require their own checks.
Unsigned installers may trigger SmartScreen; signing is not implemented.

## Troubleshooting and licenses

- Missing host utilities/libraries: rerun setup with the explicit package option,
  or ask an administrator. Normal builds need no root privileges.
- No DISPLAY: install Xvfb or use a graphical session. `--headless` forces Xvfb.
- Setup failure: inspect `build-windows-tools/Setup.log`; retain the matching
  Wine loader/server and complete Inno distribution. Do not increase timeouts
  or substitute EPEL's pure-64-bit loader blindly.
- Unowned/corrupt tools: use a fresh directory or supply the correct cached
  archive. The scripts do not recursively remove arbitrary existing tools.
- Compiler moved: use a fresh release/dependency prefix. The provenance guard
  deliberately rejects a different compiler location.
- Release failure: inspect `Release.log` and the installer `.iscc.log`.

Wine is an LGPL build-host dependency. The [Inno Setup license](https://jrsoftware.org/files/is/license.txt)
permits use/redistribution under its conditions; retain its notices and review
the vendor's [commercial licensing policy](https://jrsoftware.org/isorder.php),
including commercial CI use. Wine, ISCC, ImgDrive and its driver are not bundled
with FlacCD. Existing libFLAC/LLVM runtime notices remain in the deployment.
