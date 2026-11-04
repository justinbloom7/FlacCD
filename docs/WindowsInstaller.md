# FlacCD Windows installer

FlacCD installs for the current user on Windows 10/11 **x64**. The default is
`%LOCALAPPDATA%\Programs\FlacCD`; elevation, a service, and global PATH changes
are not required. Windows ARM64 is outside this installer's supported target.
The Start Menu entry opens this usage document in Notepad. There is no GUI yet.

## Building a release

Install [Inno Setup 6.3 or newer](https://jrsoftware.org/isinfo.php) on the Windows
build machine. It is a build prerequisite, not a runtime dependency. In MSYS2
UCRT64, stop the development agent before rebuilding (skip this command on a
first build, or use the installed copy's CLI if that owns the running agent):

```bash
./build-windows/flaccd.exe agent stop
```

The developer uses Inno Setup **7**, installed under `C:/Program Files`:

```bash
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release "-DFLACCD_ISCC=C:/Program Files/Inno Setup 7/ISCC.exe"
cmake --build build-windows
ctest --test-dir build-windows --output-on-failure
cmake --build build-windows --target flaccd-installer
```

The explicit ISCC path is optional if CMake finds it; specify it for Inno Setup 7
if automatic discovery does not find your installation. The target requires a
native Windows x64 build environment; see the
[README](../README.md#building-on-windows-with-msys2-ucrt64) for the MSYS2 UCRT64
packages and end-user installation prerequisites. It creates
`build-windows/installer/Release/FlacCD-0.1.0-windows-x64-setup.exe` for the current
version. Change `project(FlacCD VERSION ...)` in `CMakeLists.txt` for future
MAJOR.MINOR.PATCH releases; keep the installer's AppId stable across upgrades.
Installer metadata, filename and version comparison use that CMake version.
`cmake/ProductMetadata.cmake` supplies publisher **Evenstar Systems** and product
**FlacCD** to installer/ARP metadata and all five executable VERSIONINFO resources.
Each executable has its own FileDescription; `windows-version-metadata` checks
the actual Windows resources using PowerShell's FileVersionInfo API.

`flaccd-installer` depends on `flaccd-smoke`, which stages all five end-user
executables, resolves runtime DLLs, audits direct/transitive PE dependencies, and
executes them under CMD/PowerShell without MSYS2 in PATH. The validated direct
`Smoke.cmd` invocation is unchanged. The installer re-audits and locks the staged
folder through ISCC compilation. It packages the stage rather than scanning an
MSYS2 installation or maintaining another DLL list. License/runtime reports and
usage documents are included; intermediate import dumps are excluded. Additional
executables, drivers, development archives, scripts and nested DLLs are rejected.
No ImgDrive executable or driver is bundled.

`installer-payload` tests packaging validation on Linux and Windows with synthetic
fixtures. When ISCC is found on native Windows, `windows-installer` also builds
the installer through the same smoke-tested target during CTest. If ISCC is not
found, normal application builds/tests continue to work; invoking the installer
target reports how to configure the missing compiler.

## Install and use

Run the setup EXE as your ordinary Windows user. It registers **Mount as Audio CD**
using `flaccd.exe shell install`, preserving the default FLAC application. On
Windows 11 the verb may be under **Show more options**. The path is registered
after files are installed; spaces and Unicode use Inno's Unicode process/file
APIs and the existing tested CLI quoting. A registration failure is reported and
returns a nonzero setup exit code; installed files remain available for repair
with `flaccd.exe shell install` from a terminal.

ImgDrive must be installed separately from https://www.yubsoft.com/imgdrive/.
Setup checks App Paths in HKCU/HKLM, 32/64-bit views, standard Program Files
locations, and `FLACCD_IMGDRIVE`. Detection is advisory: custom/PATH-only installs
may not be detected, and finding an EXE does not prove the driver works. No
ImgDrive command is executed by detection. Setup still permits installation if
ImgDrive is missing. For a custom path configure `FLACCD_IMGDRIVE` for your user,
or use the CLI's `--imgdrive` option. Explorer uses the normal automatic detection.

From CMD in the installed directory:

```text
flaccd.exe mount "Y:\your music\Zelda.flac"
flaccd.exe status
flaccd.exe unmount F:
flaccd.exe shell status
flaccd.exe agent stop
```

Use the actual drive reported by FlacCD. See `WindowsAgentTesting.md` and
`WindowsShellTesting.md` beside this file for device and Explorer diagnostics.

## Upgrade, repair and uninstall

Run a newer installer under the same Windows user. The stable AppId reuses the
installation location and uninstall record. Same-version reinstall is supported;
older-version installation is refused. An Explorer registration owned by another
FlacCD location blocks install; first run `shell uninstall` from that location.
Modified/unowned entries are preserved, with an error instead of replacement.
Upgrades explicitly retain the same AppId, use `UsePreviousAppDir=yes` and
`UninstallLogMode=append`, and never launch the old uninstaller. This updates the
existing per-user Add/Remove Programs entry instead of adding another product.
Legacy owned Explorer commands are recognized and migrated for visible errors.

Each new installer records the names and SHA-256 hashes of its installed binaries
in `FlacCDFiles.txt`, with the manifest hash in its own uninstall registry entry.
After new files are installed, an upgrade removes an obsolete top-level EXE/DLL
only if the old manifest is unchanged, the new payload no longer contains that
name, and the file still matches the old hash. Modified, unreadable or locked
files are retained with a log message. User files/directories/cache are never
swept. Installations predating manifests retain untracked obsolete files on their
first upgrade; ownership is not guessed. Later upgrades can clean tracked files.

Before replacing binaries, setup calls the installed `agent stop --timeout 30`.
The existing IPC waits for graceful exit, releasing the EXE lock. Failure aborts
preparation; no taskkill, forced termination, automatic Restart Manager shutdown,
or automatic agent restart is used. Pause new CLI/Explorer mount requests during
maintenance: the installer is not a cross-process mount barrier. If another
process reopens a binary, cancel/retry after closing it rather than ignoring a
file-in-use error. Do not force a reboot-time replacement.

FlacCD login restoration is enabled by default when no saved preference exists.
Use `flaccd restore disable` to opt out; upgrades preserve this preference.
Registration can precede ImgDrive installation; the login helper discovers
ImgDrive with bounded retries. No ImgDrive driver is bundled. When enabled, `agent stop` suspends desired entries during the maintenance
gap. After upgrading, `restore install-default` rearms only currently confirmed live
mounts; missing/uncertain entries stay suspended. Uninstall runs `restore disable`
before stopping the agent, removing only its owned HKCU startup value while
retaining the desired list and backing resources. Custom cache configuration is
adopted from this executable's owned startup command. No new helper EXE, service,
driver or installer dependency is introduced. See `MountRecovery.md`.
Maintenance uses `restore pause` to suspend intent and wait for a running
short-lived helper to exit before replacing/removing either EXE. A timeout aborts
maintenance rather than force-terminating anything. Older installations without
a restore startup entry continue using the existing agent-only shutdown path.

Uninstall through Windows Apps or `unins000.exe`. After confirmation, uninstall
stops the agent and calls `shell uninstall` before deleting installed files.
Shutdown/owned-registration removal failures abort before file removal. A verb
pointing to a different install is retained. Cancelling the confirmation prompt
does not stop the agent or remove the Explorer command.

**Active discs remain mounted.** Neither upgrade nor uninstall unmounts discs,
decodes a BIN, deletes music, deletes adjacent/staged backing resources, or
recursively clears `%LOCALAPPDATA%\FlacCD`. Mount state, ownership records, logs
and caches remain for reconciliation. Eject monitoring pauses while the agent is
stopped. After upgrade/reinstall, a normal mount/status/drives command restarts
the agent and reconciles saved state, including discs ejected while stopped.
For a complete cleanup, unmount managed discs using FlacCD **before uninstall**.
Otherwise reinstall FlacCD later to reconcile them; do not delete backing files
while a mounted disc may still need them. Cancellation after preparation can
leave the agent stopped; the next normal CLI request restarts it.

Only installed files are tracked by Inno's uninstall log. There are no wildcard
cache removal or association registry entries in the installer. Existing files
unrelated to the installation are not swept from the installation directory.
Use `/LOG="C:\path\setup.log"` (or the uninstaller's `/LOG`) for maintenance
diagnostics. Installers are not code-signed in this milestone; Windows may show
an unknown-publisher warning. No driver/signing/system-security changes are made.

## Windows VM acceptance tests

These tests require real Windows and have not been executed by the Linux agent.

1. Build Release, run the entire CTest suite, and build `flaccd-installer`.
   Check the clean-PATH CMD and PowerShell smoke logs. In a VM without MSYS2 in
   PATH, install as a standard user: no UAC elevation, expected LocalAppData
   location, one uninstall entry and a working Start Menu document shortcut.
2. Test with ImgDrive absent (helpful advisory, installation completes), present
   via App Paths/standard directory, and custom `FLACCD_IMGDRIVE`. Confirm setup
   never tries to install or redistribute the proprietary driver.
3. Verify all staged EXEs/DLLs and license docs installed, no compiler/test tools.
   Launch installed CLI tools from clean CMD/PowerShell. Verify Explorer mounting,
   playback, track skipping, external eject cleanup, and unchanged FLAC default.
4. Test a Windows user profile/path and music filenames containing spaces and
   Unicode. Test Explorer paths with shell metacharacters as in shell tests.
5. Mount a disc, record hashes/identities of its original FLAC and backing files
   and cache/state. Upgrade while the agent is running. Confirm graceful stop,
   no file lock failure, one uninstall entry, correct registration, preserved
   media/resources, and reconciliation after the next normal CLI request.
6. Repeat same-version repair and an incremented-version upgrade. Confirm an
   older installer refuses downgrade. Make a foreign/modified Explorer entry:
   setup must preserve it and report failure, without silently replacing it.
7. Cancel uninstall at confirmation: agent and registration stay intact. Then
   uninstall with a disc mounted: agent exits, owned Explorer entry disappears,
   installed files are removed, music/cache/backing state remain unchanged.
   Reinstall and reconcile. Repeat after an eject while the agent was stopped.
8. Simulate a stop failure/timeout or a binary held open: setup/uninstall must
   report failure and preserve resources; never force-terminate anything. Test
   unrelated user CUEs, other installs' registrations and unrelated files in the
   application/cache directories are retained.
9. With no mounts, uninstall normally; verify menu/Start Menu/uninstall entries
   disappear, default FLAC application and ImgDrive remain, and an unrelated
   sentinel in the retained cache is unchanged.
10. Inspect all five EXEs' Properties > Details and the setup EXE. Company/publisher
    must be **Evenstar Systems**, ProductName **FlacCD**, and versions must match
    the configured release. Check each FileDescription and ARP Publisher too.
    Run `ctest --test-dir build-windows -R windows-version-metadata -V`.
11. Install two test releases with the same AppId: include a harmless extra DLL
    in the first test payload and omit it in the second. An unchanged tracked
    DLL must disappear; repeat with a modified DLL and modified manifest and
    verify retention. An unrelated sentinel DLL absent from the old manifest
    must remain. Verify one ARP entry, correct new versions and Explorer command,
    intact active mounts, cache/state, and recovery after agent restart.

The packaging implementation follows Inno's
[per-user privilege mode](https://jrsoftware.org/ishelp/topic_setup_privilegesrequired.htm),
[pre-install hook](https://jrsoftware.org/ishelp/topic_scriptevents.htm), and
[uninstall abort semantics](https://jrsoftware.org/ishelp/topic_isxfunc_abort.htm).

Restoration default validation on Windows:

1. In a new user profile, install and check `flaccd restore status`: enabled with an owned login registration. Mount an album, log out/in or reboot, and verify restoration.
2. Run `flaccd restore disable`, upgrade, and verify it remains disabled with no login registration.
3. Upgrade an enabled installation and verify its owned login command points to the installed EXE.
4. Install without ImgDrive: installation succeeds; install ImgDrive later, mount a disc, and verify restoration on the next login.
