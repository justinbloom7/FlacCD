# Windows cross-compilation from CentOS Stream 9

For normal use, **run these on Linux**:

```bash
scripts/SetupWindowsBuild.sh --install-system-packages
scripts/BuildWindows.sh
```

Omit the setup package option when your administrator has already installed the
host packages. See [LinuxWindowsRelease.md](LinuxWindowsRelease.md) for script
options, outputs, caching, prerequisites and troubleshooting. The scripts cover
compilation and installer packaging; this document explains the cross-build
architecture and Windows validation.

## What is built and why

Setup pins LLVM-MinGW UCRT 20240619 (Clang 18.1.8). Its Ubuntu 20.04-hosted binary
runs on stock CentOS Stream 9; the newer Ubuntu 22.04-hosted 20251104 build needed
`GLIBCXX_3.4.30`, absent from the host GCC 11 runtime. No system compiler upgrade
is needed. UCRT is supplied by Windows 10/11.

The toolchain file `cmake/toolchains/LlvmMingwUcrt.cmake` selects Windows x64
compilers, windres and target headers/libraries. CMake, Ninja, pkg-config and GNU
objdump are Linux programs. CMake package searches and pkg-config are isolated
from Linux libraries. Configuration compile/link probes verify x64/UCRT and
static libFLAC; they do not execute Windows code.

`dependencies/windows` obtains SHA-256-pinned libFLAC 1.4.3 and builds it with
that same toolchain. Ogg encapsulation, command-line tools, examples and C++
wrappers are disabled. FlacCD supports native FLAC, so no libogg is needed.
LLVM-MinGW uses libc++/compiler-rt/libunwind; do not reuse MSYS2 GCC archives.
Dependency provenance records the compiler location/fingerprint and is checked
before application configuration. Rebuild in a fresh prefix after changing the
toolchain. [LLVM-MinGW's documentation](https://github.com/mstorsjo/llvm-mingw#readme)
explains its runtime/ABI choices.

All five user programs, Windows tests/probes, and VERSIONINFO resources are built.
CompanyName remains Evenstar Systems and ProductName FlacCD. Runtime linking is
scoped to the Windows targets; native Linux and MSYS2 build paths remain supported.
The existing deployment auditor recursively inspects PE imports without running
Windows programs, bundles required non-system DLLs if any, and rejects unresolved
or conflicting dependencies. ImgDrive is never included.

## Build directories and tests

| Location | Contents | Where tests run |
|---|---|---|
| `build/` | Native Linux programs | Linux, full CTest suite |
| `build-release-windows/WindowsBuild/` | Cross-built Windows EXEs | Only `-L host` CMake tests on Linux |
| Transferred `WindowsValidation/` | Windows EXEs and relocated CTest scripts | Windows VM, full bundled CTest suite |

The build script runs only the host-labelled tests in the cross tree. No Wine
emulator is configured for application tests. Wine is explicitly used for the
Inno compiler, not to claim that Windows/ImgDrive integration works.

## Transfer to the Windows VM

After `scripts/BuildWindows.sh` succeeds, copy these from
`build-release-windows/WindowsBuild/`:

- `installer/Release/FlacCD-<version>-windows-x64-setup.exe` and its `.sha256`.
- `deploy/Release/FlacCD/`.
- `deploy/Release/WindowsValidation/`.

Keep the last two directories as siblings, for example inside
`C:\Users\justin\repo\FlacCD-Cross`. A VM shared folder, removable ZIP or SCP
works; SSH is optional. The validation folder is a developer artifact and is
not an installer payload.

## Run tests on Windows

Windows needs CMake/CTest for validation, not a C++ compiler. PowerShell does not
include CMake. If your existing MSYS2 tools are installed, use their full paths:

```powershell
& C:/msys64/ucrt64/bin/cmake.exe -P C:/Users/justin/repo/FlacCD-Cross/WindowsValidation/RelocateWindowsValidation.cmake
& C:/msys64/ucrt64/bin/ctest.exe --test-dir C:/Users/justin/repo/FlacCD-Cross/WindowsValidation --output-on-failure
```

Replace the transfer directory with your actual path. A standalone Windows CMake
installation is also suitable; use its `cmake.exe` and `ctest.exe` paths instead.
FlacCD itself requires neither CMake nor MSYS2. Repeat relocation if the folder
moves. It substitutes Windows paths into the original generated CTest definitions,
retaining fixtures and expected failures without recompilation.

The bundle includes Windows process, shell, agent, restoration, metadata, layout
and sector/export tests. It omits Linux compiler fixtures and the optional
external `flac` reference-decoder test because that Windows decoder is not
bundled. Native Linux tests provide the reference bit-exact comparison.

Install the generated setup EXE and complete the lifecycle checklist in
[LinuxWindowsRelease.md](LinuxWindowsRelease.md): mount, play/skip, eject, reboot,
restore, upgrade and uninstall. Inspect `RuntimeDependencies.txt` and
`RuntimePolicy.txt`; then verify ordinary CMD/PowerShell startup without MSYS2
on PATH. Import auditing alone does not verify runtime behavior.

## Advanced/manual workflows

The scripts delegate to existing CMake infrastructure rather than replacing it:

- `cmake/BuildWindowsRelease.cmake`: full release orchestration.
- `flaccd-deploy`, `flaccd-audit`, `flaccd-package`: deployment and import audits.
- `flaccd-validation`: relocatable Windows test bundle.
- `flaccd-installer`: existing Inno script, through Wine on Linux or native on Windows.

Prepared tool paths are in `build-windows-tools/WindowsBuildSettings.cmake`.
Advanced CMake callers can use those values with the existing toolchain and
`FLACCD_WINDOWS_DEPENDENCY_PREFIX`. Keep Release builds separate from native
Linux/MSYS2 build directories. Missing dependencies should be repaired through
setup or a fresh prefix, not by adding host library directories.

The older `cmake/PackageCrossWindows.cmake` Windows-side packaging script is still
available for transferred deployments; it needs Windows GNU objdump and ISCC.
Native Windows packaging keeps the validated direct `Smoke.cmd` invocation,
without restoring a `cmd.exe /C` wrapper. Windows installer details remain in
[WindowsInstaller.md](WindowsInstaller.md).
