# Windows x64 cross-compilation from CentOS Stream 9

The entire installer can now also be built on Linux: see
[LinuxWindowsRelease.md](LinuxWindowsRelease.md). The Windows-side packaging
instructions below remain available as an alternative; Windows runtime testing
is still a separate release gate.

Compile the application and its tests on Linux; run Windows runtime tests in
the VM. Inno Setup compilation can run on Linux using the release guide above,
or on Windows using the alternative commands below. No Windows compiler, Visual Studio, WDK, Wine, or MSYS2 installation
is needed for compilation. ImgDrive is installed separately on the Windows VM;
its application and driver are never included in our payload.

## Prerequisites and pinned toolchain

Host packages: CMake 3.20+ (the existing 3.31.8 works), Ninja, pkg-config
(`pkgconf-pkg-config`), GNU binutils with `pei-x86-64` support, curl, tar, xz, and
CA certificates. Native regression tests additionally use GCC/G++, flac-devel,
and the `flac` reference decoder. These are ordinary CentOS packages; have an
administrator install missing packages. Do not replace the system compiler.

Use the **Linux x86_64 UCRT** LLVM-MinGW **20240619** archive, containing Clang
18.1.8, LLD, libc++, compiler-rt, windres and the x64 Windows headers/libraries:

```bash
mkdir -p build-tools
curl -fL https://github.com/mstorsjo/llvm-mingw/releases/download/20240619/llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64.tar.xz \
   -o build-tools/llvm-mingw.tar.xz
printf '%s  %s\n' \
   27d33157cc252c29ad6f777a96a0d94176fea1b534ff09b5071485def143b90e \
   build-tools/llvm-mingw.tar.xz | sha256sum -c -
tar -xf build-tools/llvm-mingw.tar.xz -C build-tools
flaccdToolchain="$PWD/build-tools/llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64"
flaccdPrefix="$PWD/build-cross-prefix"
flaccdToolchainFile="$PWD/cmake/toolchains/LlvmMingwUcrt.cmake"
"$flaccdToolchain/bin/x86_64-w64-mingw32-clang++" --version
objdump -i
```

The checksum above was recorded from the tested release archive. The newer
20251104 Ubuntu 22.04 archive failed on this stock CentOS host because it needs
`GLIBCXX_3.4.30`, absent from GCC 11's host libstdc++. The pinned Ubuntu 20.04
archive runs here without changing system libraries. Do not substitute a Windows
ZIP, an MSVCRT archive, or ARM64/i686 target libraries.

[LLVM-MinGW documents](https://github.com/mstorsjo/llvm-mingw#readme) its UCRT
selection and incompatibility with static objects built by other toolchains.
UCRT is included in Windows 10/11. Rebuild all Windows dependencies with this
same toolchain; do not reuse MSYS2 GCC archives even if both target UCRT.

## Build Windows dependencies

A separate dependency project downloads and SHA-256 verifies **libFLAC 1.4.3**,
then cross-compiles and installs its static C library to the isolated prefix.
The [upstream release](https://github.com/xiph/flac/releases/tag/1.4.3) publishes
the source checksum. Ogg encapsulation, command-line tools, C++ wrappers,
examples, tests and docs are not needed. `WITH_OGG=OFF` avoids a libogg dependency;
FlacCD supports native FLAC, not Ogg FLAC. The dependency's GCC-specific libssp
protection options are disabled rather than introducing a foreign GCC runtime.
FLAC and LLVM runtime license notices are staged with the deployment.

```bash
cmake -S dependencies/windows -B build-cross-deps -G Ninja \
   -DCMAKE_TOOLCHAIN_FILE="$flaccdToolchainFile" \
   -DFLACCD_LLVM_MINGW_ROOT="$flaccdToolchain" \
   -DFLACCD_WINDOWS_DEPENDENCY_PREFIX="$flaccdPrefix"
cmake --build build-cross-deps
```

For offline builds, first obtain `flac-1.4.3.tar.xz` and add
`-DFLACCD_FLAC_SOURCE_ARCHIVE=/absolute/path/flac-1.4.3.tar.xz` to configuration.
The same pinned checksum is enforced. After changing compiler/toolchain, use new
dependency build, install-prefix and application build directories.
The prefix records the compiler's location and fingerprint; application
configuration rejects a prefix built with another recorded toolchain.

## Configure, compile and test on Linux

```bash
cmake -S . -B build-cross-windows -G Ninja \
   -DCMAKE_TOOLCHAIN_FILE="$flaccdToolchainFile" \
   -DFLACCD_LLVM_MINGW_ROOT="$flaccdToolchain" \
   -DFLACCD_WINDOWS_DEPENDENCY_PREFIX="$flaccdPrefix" \
   -DCMAKE_BUILD_TYPE=Release
cmake --build build-cross-windows --parallel 4
ctest --test-dir build-cross-windows -L host --output-on-failure
```

All five user executables, Windows tests/probes, and VERSIONINFO resources are
built. CompanyName remains **Evenstar Systems**, ProductName **FlacCD**. The
configuration compile/link probes verify Windows x64/UCRT and static libFLAC;
none execute target code. The toolchain isolates pkg-config and CMake library,
header and package discovery from Linux installations. Host programs (CMake,
Ninja, pkg-config and GNU objdump) remain host programs.

Only the `host`-labelled CMake script tests may be run in the cross-build tree.
**Do not run unrestricted CTest there:** the remaining tests are Windows
executables. No emulator is configured automatically. Run the full portable
suite separately with the existing native build:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## Stage and audit

```bash
cmake --build build-cross-windows --target flaccd-package flaccd-validation
cmake --build build-cross-windows --target flaccd-audit
```

Outputs under `build-cross-windows/deploy/Release/`:

- `FlacCD/`: the end-user deployment, documentation, licenses, version and audits.
- `FlacCD-0.1.0-windows-64bit.zip`: audited end-user ZIP (version follows CMake).
- `WindowsValidation/`: cross-built tests/probes, test scripts and a relocatable
  CTest template. This is a developer artifact, never an installer payload.

The LLVM-MinGW applications and tests use target-scoped `-static` to link
libc++, compiler-rt, libunwind and threading support. GCC/MSYS2 keeps its existing
runtime policy. The PE audit recursively resolves non-system DLL imports using
explicit target directories, rejecting unresolved, conflicting or wrong-bitness
DLLs. It never relies on host PATH. Inspect `RuntimeDependencies.txt`,
`RuntimePolicy.txt`, and `imports/*.imports.txt`. A shared dependency, if added
later, must be bundled and audited, not assumed absent.

For the tested toolchain/dependency combination, all five applications and the
test bundle import **only Windows system DLLs and UCRT API-set contracts**:
no libFLAC.dll, libc++.dll, libwinpthread-1.dll, GCC runtime DLLs, or MSYS2 DLLs.
This is an import-table result, not proof of successful Windows execution.

Transfer both `FlacCD/` and `WindowsValidation/` to the VM, preserving their
sibling relationship. For example, from Linux:

```bash
scp -r build-cross-windows/deploy/Release/FlacCD \
   build-cross-windows/deploy/Release/WindowsValidation user@windows-vm:FlacCD-Cross/
```

An OpenSSH server is optional; a VM shared folder or removable ZIP works too.
No network access is required by FlacCD itself.

## Windows CTest, smoke testing and installer

On the Windows VM, install CMake/CTest and Inno Setup 7. For the repeated PE audit
use a Windows-hosted **GNU objdump** supporting x64 PE; the existing MSYS2 UCRT64
binutils is suitable as a validation tool. No Windows C/C++ compiler is used.
The deployed application itself must also be tested without MSYS2 on PATH.

In PowerShell, adjust the transfer folder and tool paths:

```powershell
cmake -P C:/FlacCD-Cross/WindowsValidation/RelocateWindowsValidation.cmake
ctest --test-dir C:/FlacCD-Cross/WindowsValidation --output-on-failure
cmake "-DpayloadDirectory=C:/FlacCD-Cross/FlacCD" `
   "-DpeObjdump=C:/msys64/ucrt64/bin/objdump.exe" `
   "-Discc=C:/Program Files/Inno Setup 7/ISCC.exe" `
   -P C:/FlacCD-Cross/WindowsValidation/source/cmake/PackageCrossWindows.cmake
```

Relocation substitutes Windows paths and Windows CMake into the original
generated CTest definitions, retaining fixtures, expected failures and boundary
tests. It does not recompile anything. Cross validation omits the optional
external `flac` reference-decoder test because no Windows decoder is bundled;
native Linux tests provide that bit-exact reference comparison. It also omits
compiler-specific GNU build-fixture tests. The Windows process, shell, agent,
restore and version-resource tests remain included.

`PackageCrossWindows.cmake` re-audits the transferred binaries, runs the existing
clean-PATH CMD/PowerShell smoke checks, and invokes the existing Inno installer
script. It preserves the validated **direct Smoke.cmd invocation**. The version
is read from the cross-built payload's `BuildVersion.txt`. The resulting installer
is under `C:/FlacCD-Cross/installer/`. The original Windows-native
`flaccd-installer` target continues to work unchanged.

Before release, validate on Windows 10 and 11 x64:

1. Run relocated CTest and retain its output; check VERSIONINFO publisher/product.
2. Complete the clean-PATH smoke checks; launch from ordinary CMD and PowerShell.
3. Install/upgrade per-user, mount using Explorer with spaces/Unicode paths, play
   first/last tracks and skip tracks. Confirm ImgDrive recognizes an audio CD.
4. Eject and verify owned CUE cleanup; verify existing user CUE/FLAC preservation.
5. Reboot/login with restoration enabled and confirm remounting; test explicit
   disable, agent stop/restart, and stale-state recovery.
6. Uninstall following `WindowsInstaller.md`, preserving active backing resources.

The Linux cross-build does not validate actual Windows process, registry, device,
ImgDrive or installer behavior. Those tests remain necessary even when linking
and import auditing succeed.

## Troubleshooting

- Missing `GLIBCXX_*`/`GLIBC_*` while launching clang: wrong Linux-hosted archive;
  use the pinned one. Do not copy host runtime DLLs into the Windows deployment.
- Missing `flac.pc`: build dependencies first, pass the same absolute prefix, and
  use a fresh application build tree. Ambient Linux PKG_CONFIG_PATH is cleared.
- UCRT/x64 probe fails: check the archive and architecture. Do not disable the
  probe to link mismatched dependencies.
- Static FLAC probe fails: inspect `CMakeFiles/FlacCDFlacProbe.log`; rebuild FLAC
  with this LLVM-MinGW toolchain rather than linking `libFLAC.dll.a` or Linux .a.
- PE inspection fails: `objdump -i` must list `pei-x86-64`; configure
  `-DFLACCD_WINDOWS_OBJDUMP=/absolute/path/to/GNU/objdump` if needed.
- Upstream FLAC 1.4.3 emits a CMake minimum-version deprecation warning with newer
  CMake; it is not a compile failure. The project does not patch upstream sources.
- VM paths changed: rerun `RelocateWindowsValidation.cmake` before CTest. Keep the
  transferred folder named `FlacCD` for the packaging script.
- Windows reports a locked agent during an upgrade: use `flaccd agent stop` and
  retry, preserving mounted resources. Do not force-kill as routine maintenance.
