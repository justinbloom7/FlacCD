# Build a Windows installer entirely on CentOS Stream 9

FlacCD can cross-compile its Windows applications and compile the existing Inno
Setup installer on Linux. Wine runs **ISCC**, not ImgDrive or its driver. Linux
packaging does not replace the Windows VM validation listed below.

The native Windows/MSYS2 `flaccd-installer` target still runs its existing
CMD/PowerShell smoke checks. On Linux the same target depends on the audited
deployment and invokes the same installer script under Wine. The installer AppId,
publisher **Evenstar Systems**, per-user scope, Explorer integration, ImgDrive
checks, restoration default, upgrade logic and safe uninstall are unchanged.

## One-time prerequisites

Use the pinned LLVM-MinGW UCRT x64 toolchain and dependency instructions in
[WindowsCrossCompilation.md](WindowsCrossCompilation.md). In addition, install
Wine and its shared data, the `winepath` wrapper, and optionally Xvfb for CI.
These one-time package installation commands are for an administrator; do not run
normal builds as root:

```bash
sudo dnf install dnf-plugins-core epel-release
sudo dnf config-manager --set-enabled crb
sudo dnf install wine-core wine-common wine-filesystem wine-fonts \
   cmake ninja-build pkgconf-pkg-config binutils curl tar xz python3 \
   xorg-x11-server-Xvfb
```

EPEL 9 provides [Wine 8.0](https://packages.fedoraproject.org/pkgs/wine/wine/epel-9.html).
Its pure-64-bit configuration cannot install even the x64 Inno 7 compiler:
[Inno's own setup program](https://github.com/jrsoftware/issrc/blob/is-7_1_1/Projects/Src/Setup.MainFunc.pas#L3575-L3579)
explicitly checks for x86/WoW64 support. Do not assume
that installing only the EPEL loader is sufficient. A newer WoW64-capable Wine
is required for the complete Linux-only bootstrap below.

The pinned example uses **Wine 10.0 amd64-wow64** from the independent
[Kron4ek Wine-Builds project](https://github.com/Kron4ek/Wine-Builds), built from
vanilla Wine sources. This is a third-party binary distribution, not an official
WineHQ binary; review that provenance before using it. It supports running x86
programs without Linux 32-bit libraries. Alternatively, build a WoW64-enabled
Wine from [upstream sources](https://dl.winehq.org/wine/source/) or supply an
already validated distribution build. Do not mix loaders and wineservers.

```bash
mkdir -p build-tools
curl -fL https://github.com/Kron4ek/Wine-Builds/releases/download/10.0/wine-10.0-amd64-wow64.tar.xz \
   -o build-tools/wine-10.0-amd64-wow64.tar.xz
printf '%s  %s\n' \
   aeebbbf239e548f0136f1cd72a2e109dd9a572a8f703da3c09fa40d74d5c255f \
   build-tools/wine-10.0-amd64-wow64.tar.xz | sha256sum -c -
tar -xf build-tools/wine-10.0-amd64-wow64.tar.xz -C build-tools
```

```bash
flaccdWine="$PWD/build-tools/wine-10.0-amd64-wow64/bin/wine"
flaccdWinepath="$PWD/build-tools/wine-10.0-amd64-wow64/bin/winepath"
unset WINESERVER WINELOADER
"$flaccdWine" --version
test -x "$flaccdWinepath"
export WINEPREFIX="$PWD/build-wine-prefix"
export WINEARCH=win64
export WINEDLLOVERRIDES='mscoree,mshtml,winegstreamer='
"$flaccdWine" wineboot -u
```

Use a dedicated prefix, not your normal `~/.wine`. The optional GStreamer
integration is disabled to avoid prefix initialization hangs; it is not needed
for packaging. Mono and Gecko are not needed
by the compiler. Wine changes registry/data only inside this prefix. Initial
setup may need a working display; on a headless machine run the initialization,
Inno installation and release commands under `xvfb-run -a` instead.

Install the complete Inno compiler distribution (ISCC also needs adjacent DLLs
and support files), not just ISCC.exe. The example pins the official **7.1.1 x64**
archive; the SHA-256 below was recorded from that download:

```bash
mkdir -p build-tools
curl -fL https://github.com/jrsoftware/issrc/releases/download/is-7_1_1/innosetup-7.1.1-x64.exe \
   -o build-tools/innosetup-7.1.1-x64.exe
printf '%s  %s\n' \
   8a5a6711c3766a6e5fb74f280ba0e3551e719ce94538d901fdbd1f8f9920b3f0 \
   build-tools/innosetup-7.1.1-x64.exe | sha256sum -c -
"$flaccdWine" "$PWD/build-tools/innosetup-7.1.1-x64.exe" \
   /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- '/DIR=C:\InnoSetup'
flaccdIscc="$WINEPREFIX/drive_c/InnoSetup/ISCC.exe"
test -f "$flaccdIscc"
```

The [vendor downloads page](https://jrsoftware.org/isdl.php) identifies the x64
edition. Keep the version/hash fixed in CI and archive the downloaded compiler
installer; do not fetch an unversioned `latest` URL. A future compiler upgrade
needs a fresh packaging and Windows VM validation run. Wine version and compiler
fingerprint are recorded in release logs/receipts.

## One-command release driver

From the repository root, after the cross-toolchain and Wine setup above:

```bash
cmake \
   "-DFLACCD_LLVM_MINGW_ROOT=$PWD/build-tools/llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64" \
   "-DFLACCD_ISCC=$flaccdIscc" \
   "-DFLACCD_WINE_PREFIX=$WINEPREFIX" \
   "-DFLACCD_WINE_EXECUTABLE=$flaccdWine" \
   "-DFLACCD_WINEPATH_EXECUTABLE=$flaccdWinepath" \
   -P cmake/BuildWindowsRelease.cmake
```

For headless CI, prefix that command with `xvfb-run -a`. All operations after the
one-time OS setup run as an ordinary user. No system PATH or Windows association
is modified by building. Applications are not automatically launched under Wine.

Optional `-D` settings:

| Setting | Purpose |
|---|---|
| `FLACCD_RELEASE_ROOT` | Build/output root; defaults to `build-release-windows` |
| `FLACCD_WINDOWS_DEPENDENCY_PREFIX` | Reuse an existing dependency prefix built by the same toolchain |
| `FLACCD_FLAC_SOURCE_ARCHIVE` | Offline, hash-verified FLAC source archive |
| `FLACCD_WINEPATH_EXECUTABLE` | Explicit Linux `winepath` wrapper |
| `FLACCD_WINDOWS_OBJDUMP` | Explicit host GNU objdump with x64 PE support |

The driver checks prerequisite paths/programs, builds missing dependencies using
the pinned dependency project, configures Release/Ninja, builds the Windows
programs and tests, runs only host-labelled tests, stages and audits deployment,
creates the ZIP and validation bundle, then invokes ISCC under Wine. Compiler
paths, source/manifest paths and output paths are converted with `winepath -w`
inside the explicit prefix; no shell command is constructed.

Build and prefix locks serialize repeated/concurrent use. Repeated releases
reuse the CMake/Ninja dependency and application builds. An explicit dependency
prefix is validated, not silently replaced with host libraries. Use a new root
and dependency prefix when changing the toolchain or dependency versions. The
script is reproducible in its inputs/process; executable timestamps and Inno
compression are not promised to be byte-for-byte reproducible.

A failure returns nonzero. ISCC has a ten-minute timeout; path conversion has a
one-minute timeout. Failed compilation cannot publish a partial installer. A
previous complete installer may remain after failure: check the command exit
status and `ReleaseStatus.txt`, not just existence of an old file.

## Output and checks

Default output root: `build-release-windows/`:

- `Release.log`, `ReleaseStatus.txt`, `ReleaseArtifacts.txt`.
- `WindowsBuild/installer/Release/FlacCD-<version>-windows-x64-setup.exe`.
- Adjacent `.sha256`, `.build.txt`, and `.iscc.log` receipt/log files.
- `WindowsBuild/deploy/Release/FlacCD/` and its audited ZIP.
- `WindowsBuild/deploy/Release/WindowsValidation/` for Windows CTest.

The version comes from CMake's project version; do not edit a second version in
the installer script. The receipt hashes ISCC, the installer script, and the
payload import report. The final installer PE/bootstrap imports, including bounded parsing of its
delayed system-DLL imports, are checked before publication. Application audits
continue to reject delayed imports. Inno defaults to an x86 bootstrap even when
its compiler and application payload are x64; the existing installer still
restricts installation to x64 Windows. This separate bootstrap is not linked
with the application libraries. Payload audits include transitive non-system DLL imports,
static FLAC/runtime policy, executable inventory, third-party notices and
exclusion of development tools/drivers. Inspect `RuntimeDependencies.txt` and
`RuntimePolicy.txt`; the pinned UCRT build currently requires only Windows system
DLLs/API-set contracts. ImgDrive is never bundled.

To package an already configured cross-build without the full driver:

```bash
cmake -S . -B build-cross-windows \
   "-DFLACCD_ISCC=$flaccdIscc" \
   "-DFLACCD_WINE_PREFIX=$WINEPREFIX" \
   "-DFLACCD_WINE_EXECUTABLE=$flaccdWine" \
   "-DFLACCD_WINEPATH_EXECUTABLE=$flaccdWinepath"
cmake --build build-cross-windows --target flaccd-installer
```

Keep its existing toolchain/cache configuration. This compiles the installer;
it does not install FlacCD into Wine or change the persistent mount store.

Native regression and packaging-mock checks (Python 3 generates synthetic PE
fixtures; it is not a FlacCD runtime dependency):

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`wine-installer-packaging` uses mock Wine/ISCC programs and real small PE files;
it needs no Wine installation. It checks Unicode/space paths, literal arguments,
explicit prefix isolation, owned binary manifests, checksums, compiler/path
conversion failure, missing/corrupt output, retry behavior and preservation of a
previous good installer. The `pe-delay-imports` test also exercises PE32/PE64 descriptors, legacy VA
addresses, invalid flags, truncated tables, out-of-section pointers and missing
name/descriptor terminators. Actual ISCC compilation is a separate release check.

## Windows VM release gate

Transfer the installer plus its hash, audited deployment and validation bundle.
Run the relocation and CTest commands from `WindowsCrossCompilation.md`. Use full
paths to Windows CMake/CTest if they are not on PowerShell's PATH. No Windows
compiler is needed. Retain logs for both Windows 10 and 11 x64 where available.

Use a VM snapshot before installing and record the exact installer SHA-256:

1. Run Windows CTest; then run the existing clean-PATH CMD/PowerShell deployment
   smoke check. Confirm no application DLL is loaded from MSYS2/developer PATH.
2. Install as a normal user without elevation, with ImgDrive present. Check the
   Evenstar Systems publisher, one per-user ARP entry and Explorer's command;
   keep the default FLAC player unchanged.
3. Mount a FLAC with embedded CUE, including spaces/Unicode. Play first and last
   tracks, skip tracks, check the expected audio TOC, and verify no BIN is made.
4. Eject in Explorer. Confirm only owned resources are cleaned and the original
   FLAC and any user CUE are unchanged.
5. Mount again and reboot. Confirm login restoration is enabled by default and
   the CD remounts. Test reboot/crash recovery when ImgDrive media is absent.
   Explicit eject/unmount must remove restore intent.
6. Upgrade an existing installation with an active managed mount; check that the
   agent stops safely, mount resources remain, obsolete owned binaries are
   replaced and ARP stays single. Also test an explicitly disabled restoration
   preference: the upgrade must preserve it.
7. Uninstall with and without active mounts. Check Explorer/startup registration
   cleanup and retention of user data/cache/backing resources when required.
8. In a clean snapshot without ImgDrive, verify the installer gives its advisory
   and does not install or redistribute ImgDrive; confirm actionable mount errors.

Wine compilation does not validate Explorer, Windows registry semantics, ImgDrive,
notifications, reboot restoration, or uninstall behavior. Those require the VM.
Unsigned release installers may trigger SmartScreen; code signing is separate
and is not implemented here.

## Troubleshooting and licensing

- `winepath` missing: install `wine-common` as well as `wine-core`.
- Inno setup reports an unsupported Windows version in a pure-64-bit Wine:
  its setup checks for x86 support, not just an OS version number. Use the pinned
  WoW64 build; changing the reported version cannot add missing emulation.
- Prefix/architecture error: initialize a dedicated win64 prefix. Do not change
  WINEARCH for an existing win32 prefix; create a new directory instead.
- Missing kernel32/NLS/user32/helper DLLs: use a complete matched Wine package
  installation including `wine-filesystem` and resolved dependencies. A Wine
  executable copied alone is insufficient.
- No display or a dialog hangs: use Xvfb even for silent setup. Test prefix setup
  first, and retain the Inno compiler log. Do not increase timeouts blindly.
- ISCC cannot load: install the whole x64 compiler distribution in the selected
  prefix; check `FLACCD_ISCC`, the loader and compiler log. The x86 compiler may
  need Wine components unavailable in a pure 64-bit EPEL installation.
- A build failure leaves older successful artifacts in place intentionally;
  `ReleaseStatus.txt` stays INCOMPLETE and the driver returns nonzero.

Wine is a build-host dependency under its upstream LGPL terms, not a bundled
FlacCD runtime. The [Inno Setup license](https://jrsoftware.org/files/is/license.txt)
permits use/redistribution subject to its conditions; its copyright/attribution
must be retained. The vendor also publishes a
[commercial licensing policy](https://jrsoftware.org/isorder.php), including CI
use in commercial contexts. This project's personal use does not imply a license
for every future distribution context. We do not redistribute the Inno compiler,
Wine runtime, ImgDrive application, or ImgDrive driver. libFLAC and LLVM runtime
notices stay in the deployment. No paid signing infrastructure is introduced.

## Verification for this change

On CentOS Stream 9, the pinned LLVM-MinGW/libFLAC dependency and full Windows
application build succeeded. Inno Setup 7.1.1 x64 was installed and executed in
a user-local Wine 10.0 amd64-wow64 prefix. The complete release driver succeeded
on repeated runs, producing the installer, ZIP, validation bundle, checksums and
receipts. All five application import closures required only Windows system
DLLs/UCRT contracts; the installer bootstrap's normal and delayed imports also
resolved to system DLLs. The native suite passed 27/27 tests.

The EPEL Wine 8 pure-64-bit bootstrap attempt was rejected by Inno's WoW64 check;
it is not a verified alternative. Testing used an existing graphical session,
not a headless Xvfb run. Windows VM installation, real ImgDrive integration,
Windows 10/11 reboot/restore, upgrades and uninstall of this newly built installer
remain unverified here; use the release gate above. The developer's earlier
successful Windows cross-build tests do not establish those installer results.
