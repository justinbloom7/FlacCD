# Windows deployment without a development environment

The deployment is a folder/ZIP containing all five end-user programs:
`flaccd.exe`, `flaccd-agent.exe`, `flaccd-info.exe`, `flaccd-export.exe`, and
`flaccd-bin-cue.exe`. Keep the folder together. ImgDrive is installed separately;
FlacCD does not bundle or install its executable or driver.

No application logic changes are involved. Linux builds retain their existing
link behavior. Windows project libraries are explicitly static, avoiding separate
C++ exception/allocator runtimes across FlacCD's own DLL boundaries.

## Build and package using MSYS2 UCRT64

From your existing UCRT64 development shell:

```sh
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows
ctest --test-dir build-windows --output-on-failure
cmake --build build-windows --target flaccd-package
cmake --build build-windows --target flaccd-audit
cmake --build build-windows --target flaccd-smoke
```

This produces:

- `build-windows/deploy/Release/FlacCD/`: deployable application folder.
- `build-windows/deploy/Release/FlacCD-0.1.0-windows-64bit.zip`: ZIP on a 64-bit
  target (version/bitness follow the configured project/toolchain).

Use `flaccd-deploy` instead of `flaccd-package` if you only want the folder.
For a multi-configuration generator add `--config Release`. An unspecified
single-configuration build type uses `deploy/Default`; Release is recommended.
These targets run each time requested, so dependency changes are rescanned.
`flaccd-audit` verifies an existing staged folder without rebuilding/replacing it.
Do not point deployment output at an installed/running application directory.

Packaging copies only the required non-system DLLs beside the executables. It
never copies to System32, changes global PATH, or bundles MSYS2/Git Bash. The
build directory's individual executables are **not** the delivery artifact: run
from the audited folder or extracted ZIP. No CMake, compiler or MSYS2 tools are
required on the destination machine.

## Runtime policy

For MinGW, options are target-local and apply to every end-user executable:

- `FLACCD_MINGW_STATIC_RUNTIME=ON` (default) uses `-static-libgcc` and
  `-static-libstdc++`.
- `FLACCD_MINGW_STATIC_WINPTHREAD=ON` (default) asks the selected target compiler
  for `libwinpthread.a`. If present, a compile/link probe exercises C++ threads
  and mutexes, then inspects the probe PE imports for runtime DLL leakage. A
  successful audit enables scoped whole-archive linking of that
  archive, avoiding library-order omissions. No target binary is run by the
  probe, so cross-compilation is supported.
- If the archive is absent, the probe fails, or its PE imports cannot confirm
  static runtime selection, winpthreads remains shared and is
  bundled **if the actual import graph requires it**. If static GCC/C++ runtime
  linking is disabled, static winpthreads is not attempted, avoiding inconsistent
  thread runtimes between a shared libstdc++ and the application.

MinGW now defaults to `FLACCD_PREFER_STATIC_FLAC=ON`. Discovery reads the target
pkg-config metadata, selects **`libFLAC.a`**, and never treats `libFLAC.dll.a` as
a static implementation. The latter is an import library and still requires
`libFLAC.dll`, regardless of `-static-libgcc`/`-static-libstdc++`.

The selected archive and headers come from the target pkg-config directories
(e.g. the same UCRT64 prefix). Its imported CMake target carries `FLAC__NO_DLL`
and private dependencies from `pkg-config --static`, preferring real Ogg/thread
archives where available. A compiler/linker probe references decoder, encoder and
metadata APIs, checking header usage, architecture/link compatibility and private
symbols with the selected toolchain. On Windows the probe PE imports are also
checked to reject a disguised import archive. The target binary is never run by
configuration, so MinGW cross-compilation remains supported.

Configure prints `FLAC: verified static archive ...` only after the probe passes.
Otherwise it reports shared fallback and leaves a diagnostic log in
`CMakeFiles/FlacCDFlacProbe.log`. `FLACCD_PREFER_STATIC_FLAC=OFF` explicitly chooses
the existing pkg-config shared selection. An optional
`FLACCD_STATIC_FLAC_ARCHIVE=/actual/compatible/libFLAC.a` override still must pass
the probe; use an archive built for the same target CRT/toolchain/header package,
not a differently named import library or an unrelated MinGW installation.

If static FLAC is unavailable/impractical, the deployment target bundles
`libFLAC.dll` and all non-system imports recursively. Static FLAC can also have
shared private dependencies; the final import audit, not archive names or a
successful build alone, determines which DLLs must be shipped. Native Linux keeps
its existing pkg-config linkage. No decoder, parser or mount behavior is changed.

`RuntimePolicy.txt` records the selected FLAC linkage/archive and runtime policy.
`RuntimeDependencies.txt` records the **actual** graph, PE format and SHA256 for
every executable and bundled DLL. `RuntimeOrigins.txt` records where bundled
DLLs came from. `imports/*.imports.txt` contains raw `objdump -p` output for every
staged binary; `source-imports/` retains the initial source inspection. These
reports distinguish OS-supplied imports from bundled non-system dependencies.

After packaging, show the actual selected policy and final dependency graph:

```bat
type build-windows\deploy\Release\FlacCD\RuntimePolicy.txt
type build-windows\deploy\Release\FlacCD\RuntimeDependencies.txt
```

A `static` FLAC policy is enforced by the final graph audit: any remaining
`libFLAC*.dll` import fails deployment. In shared fallback, the graph must resolve
and bundle that DLL and all of its non-system dependencies before publication.

Expected categories, **not a verified import list for your build**:

| Dependency | Policy |
|---|---|
| GCC unwinder (`libgcc_s_*.dll`) | Static in FlacCD by default; may still be needed by a third-party DLL |
| C++ library (`libstdc++-6.dll`) | Static in FlacCD by default; any third-party import is bundled |
| `libwinpthread-1.dll` | Static when the archive/probe supports it; otherwise bundled if imported |
| `libFLAC*.dll` | Absent when static selection/probe succeeds; otherwise bundled with its dependencies |
| `libogg*.dll` | Static when a compatible private archive is selected; otherwise bundled if imported |
| `KERNEL32.dll`, `ADVAPI32.dll`, `USER32.dll`, `SHELL32.dll`, `OLE32.dll`, `SETUPAPI.dll`, `CFGMGR32.dll`, etc. | Windows system DLLs; not bundled |
| `ucrtbase.dll`, Windows CRT API-set contracts | Supplied by Windows 10/11; not bundled |
| `msys-2.0.dll`, `cygwin1.dll` | Rejected: indicates an unintended POSIX-emulation build/dependency |

A small explicit Windows system-DLL list and API-set contract patterns classify
OS imports. An unfamiliar dependency fails unresolved rather than silently being
called a system DLL. In particular, redistributables such as `vcruntime140.dll`
are not treated as guaranteed Windows components. Toolchains targeting newer OS
APIs still require qualification on the oldest supported Windows version.

## Dependency discovery and audit guarantees

The scanner uses target-capable **GNU objdump** (`CMAKE_OBJDUMP` by default),
never `ldd`, shell commands, or execution of target binaries. It recursively
follows normal imports and PE export forwarders, resolves names case-insensitively,
and requires a consistent PE architecture. Nonzero delay-load import tables
cause an explicit failure: this audit will not silently certify a graph it cannot
fully decode. Manually loaded plugins/COM components are outside static import
analysis; Windows system components and the separately launched ImgDrive remain
external prerequisites.

Source DLL search directories come from the selected compiler, its runtime
queries, pkg-config's FLAC library directories/sibling `bin`, the application
output directories, and optional explicit target directories. The scanner does
**not** resolve DLLs from ambient PATH or guessed host system directories.
Different DLLs with the same imported name cause a conflict error. Identical
copies are accepted. After copying, a second recursive audit searches **only the
staged folder**, so development files cannot mask a missing transitive DLL.

Useful CMake cache options, with actual paths for your environment:

```sh
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "-DFLACCD_WINDOWS_RUNTIME_DIRS=C:/TargetDeps/bin;C:/OtherTargetDeps/bin" \
  "-DFLACCD_WINDOWS_OBJDUMP=C:/Actual/Toolchain/bin/objdump.exe"
```

An unresolved DLL error names the import and searched directories. Add its
**target-architecture** directory with `FLACCD_WINDOWS_RUNTIME_DIRS`; do not copy
random DLLs from other installations or alter global PATH. Runtime DLLs are
copied automatically on the next deploy/package invocation. If static-link
policy is not satisfied by direct executable imports, deployment fails instead
of claiming those imports disappeared. Third-party DLLs may still legitimately
import the same redistributables.

The script stages into a marked temporary folder, audits before publication,
and refuses to replace an unowned directory. Failed staging retains the prior
completed output. Package creation re-audits the folder and publishes a ZIP only
after successful archiving. A deployment lock serializes deploy/audit/package.
Stop an agent running from the old output before replacing that output.

Available standard MSYS2 license directories are copied under `licenses/`.
Additional notices can be supplied with the semicolon-separated
`FLACCD_WINDOWS_LICENSE_DIRS` cache option. Redistribution must retain the notices
and comply with the versions actually linked/bundled, including static runtimes;
automatic discovery is not a blanket license-compliance guarantee. ImgDrive's
proprietary application/driver is not included.

## Linux-to-Windows cross-compilation

Use your existing MinGW CMake toolchain and target libFLAC/pkg-config setup:

```sh
cmake -S . -B build-cross -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/actual/path/to/mingw-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  "-DFLACCD_WINDOWS_RUNTIME_DIRS=/actual/windows-target-prefix/bin"
cmake --build build-cross --target flaccd-package
```

The toolchain must select Windows/MinGW and target FLAC libraries, not host Linux
libFLAC. Supply a target-aware pkg-config wrapper using `PKG_CONFIG_EXECUTABLE`
where needed. `CMAKE_OBJDUMP`/`FLACCD_WINDOWS_OBJDUMP` must read target PE files;
compiler runtime queries use the selected compiler. No Windows VM/Wine is needed
to stage and inspect imports. Runtime tests still require Windows. Native Linux
builds have no Windows runtime flags or deployment targets.

For MSVC builds, MinGW link flags are not applied. The PE deployment targets still
require GNU objdump and explicitly configured redistributable DLL directories;
MSVC redistributable selection/licensing remains the distributor's responsibility.

## Graceful stop before rebuilding

Stop the agent that is using the old executable **before** building/linking it:

```bat
flaccd agent stop
flaccd agent stop --timeout 120
cmake --build build-windows
```

The first successful command waits for complete process exit and release of the
image file. Repeating it while no agent exists is a successful no-op; it does
not need ImgDrive discovery or start a new process. Stop does not unmount discs,
delete their CUE/audio files, or discard ownership records. Monitoring pauses;
restart with `flaccd status` and saved mounts/ejects are reconciled.

For the **first upgrade from a binary that lacks the new syntax**, run the old
`flaccd agent-stop` from your existing working UCRT64 shell before rebuilding.
The alias remains supported in the new CLI. Do not solve a locked executable by
changing global PATH or making forced termination the normal build workflow.

If shutdown times out, the command returns nonzero and reports the process ID
when known. It does not force-kill the agent. Finish/wait for ongoing work and
retry with a longer timeout; do not assume the executable is unlocked after an
error. See [WindowsAgentTesting.md](WindowsAgentTesting.md) for lifecycle tests.

## Verify from normal cmd.exe and PowerShell

The native `flaccd-smoke` target (also registered as `windows-deployment-smoke`
in native MinGW CTest) first builds/audits the isolated folder, then starts all
five executables under system CMD and PowerShell with development directories
removed from their **process-only** PATHs. Four utilities use `--help`; the agent
gets intentionally invalid startup arguments and must return its expected error
without starting a server. It needs no mounted disc/ImgDrive operation and does
not alter script-execution policy. Logs are written to `deploy/Release/Smoke/`.
Cross builds can package/audit on Linux, but runtime smoke tests require Windows.

Do not claim the binaries are standalone based on successful linking or an MSYS2
shell run. The strongest test is a clean Windows 10/11 VM without MSYS2, MinGW or
Git installed. Copy/extract the ZIP there, install ImgDrive separately, then test.

On a development machine, first stop any **old development agent**. Otherwise a
packaged CLI can talk to that existing per-user agent and falsely appear to work.
Confirm the running process image path, not just its filename.

In a fresh normal `cmd.exe` session (replace paths):

```bat
set "PATH=%SystemRoot%\System32;%SystemRoot%"
cd /d C:\FlacCD-Test\FlacCD
flaccd-info.exe --help
flaccd-export.exe --help
flaccd-bin-cue.exe --help
flaccd.exe --help
flaccd.exe drives --imgdrive "C:\Actual\ImgDrive.exe"
flaccd.exe status
```

This PATH assignment affects only this test shell, not the user's global PATH.
Run metadata inspection and a small export using an actual test FLAC as well;
help alone does not exercise every code path. Mount/play/unmount through the
packaged CLI to exercise the adjacent packaged agent. Use the drive letters and
checks in [WindowsAgentTesting.md](WindowsAgentTesting.md).

In a separate normal PowerShell session:

```powershell
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
Set-Location 'C:\FlacCD-Test\FlacCD'
.\flaccd-info.exe --help
.\flaccd-export.exe --help
.\flaccd-bin-cue.exe --help
.\flaccd.exe drives --imgdrive 'C:\Actual\ImgDrive.exe'
Get-CimInstance Win32_Process -Filter "Name='flaccd-agent.exe'" |
  Select-Object ExecutablePath,CommandLine
Get-Process flaccd-agent | ForEach-Object {
  $_.Modules | Select-Object ModuleName,FileName
}
.\flaccd.exe agent stop
```

Check that the agent is the packaged executable and every non-system loaded DLL
comes from the deployment folder, not a compiler/MSYS2/Git directory. The module
inventory can require matching process bitness/access rights. An MSYS2 process
already running in the user session is not a substitute for this check.

Retain the dependency reports, OS version, toolchain version and results from both
shells. Test on Windows 10 as well as Windows 11 before claiming both were verified.

## Validation in this Linux development session

Native project builds/tests run on CentOS Stream 9. The `windows-deployment` test
uses host GNU binutils to create genuine PE32+/PE32 fixtures without any Windows
headers/runtime. It tests recursive and forwarded dependencies, system/runtime
classification, mixed-case lookup, all five staged executable names, absent
transitive DLLs, isolated re-audit, conflicting DLLs, architecture mismatch,
static-policy violations, prior-output preservation, ownership and ZIP creation.
It does not execute those fixtures or prove actual MinGW runtime link selection.
The fixture test is registered when suitable host GNU binutils are available.

The static-FLAC CMake tests use native stub archives with a mandatory private
Ogg-like symbol and `FLAC__NO_DLL` header check. They cover selected static
linkage, missing/incompatible archives, missing private dependencies, explicit
shared selection, and rejection of a `.dll.a` override. They do not prove real
UCRT64 libFLAC ABI/decoding behavior; run the normal audio/metadata tests there.

No MinGW compiler or actual Windows FlacCD binaries were available here. The
actual final FlacCD PE import tables, real static FLAC/winpthreads probes, cmd.exe/PowerShell
startup, loaded-module paths and clean-Windows behavior therefore remain
**unverified until the commands above are run with the Windows build**. The
package reports will supply the exact remaining DLL list for that build.

Primary references:

- [GCC link options](https://gcc.gnu.org/onlinedocs/gcc/Link-Options.html)
- [MSYS2 UCRT64 FLAC package and runtime dependencies](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-flac)
- [MSYS2 winpthreads runtime package](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-libwinpthread)
