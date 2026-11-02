# Windows agent and drive-mapping qualification

The portable coordinator/protocol are tested on CentOS Stream 9 with mock
processes, devices and synthetic FLACs. Windows API compilation, real named-pipe
tests, PnP mapping and ImgDrive integration were unavailable in this session.
Do not treat mock results as proof of real mounting/eject behavior.

## Findings and evidence limits

Confirmed from source: the old backend invoked `imgdrive.exe -m:D <CUE>` and
accepted any optical drive it could verify empty. It did not identify that drive
as an ImgDrive device or enumerate other optical drives after mounting. It then
polled only D: for the expected TOC. Unmount invoked `-u <CUE>`, independently of
the requested letter. That combination explains the false verification wait
and why an image on another drive can be removed by a record named D:.

[ImgDrive's vendor CLI documentation](https://www.yubsoft.com/imgdrive/help.html)
explicitly supports letter, numeric-ID and tag selectors. `-m:d` is its letter
example; numeric IDs are a separate namespace. There is no evidence that D must
be converted to an array index. Why the installed build accepted an unavailable
D: and mounted elsewhere is still **unconfirmed**: collect version/help and actual
before/after device data. The wrapper now refuses an unverified/physical D: before
invocation and detects an unexpected target instead of silently succeeding.

The PnP classifier checks the device/ancestor driver service named `ImgDrive`,
case-insensitively; it does not infer identity from a friendly CD-ROM name.
Qualification must verify the installed driver's actual service ancestry. If
its service differs or interfaces cannot be read without elevation, selection
fails closed. Do not relax the classifier to accept all empty optical drives.
PnP IDs persist in records; Windows device numbers are used only for the live
interface-to-letter join, never as persistent identity or vendor numeric IDs.

## Build and automated tests

Use the existing MSYS2 **UCRT64** shell and dependencies:

```sh
cmake -S . -B build-windows -G Ninja
cmake --build build-windows
ctest --test-dir build-windows --output-on-failure
ctest --test-dir build-windows -R '^(windows-process|windows-agent-ipc)$' -V
```

Keep `flaccd.exe`, `flaccd-agent.exe` and the required runtime DLLs available.
Building target `flaccd` also builds its agent. No WDK/IDE or security setting
changes are needed. Never run these tests as administrator to mask access errors.

`windows-process` retains its existing real argument/Unicode/backslash/timeout
coverage. `windows-agent-ipc` uses a separate current-user test endpoint and its
own process as an agent helper. It tests startup, concurrent clients, duplicate
instance rejection, structured errors, status/drives, refusal of unmanaged
unmount, stop and restart without mounting any real ImgDrive image. Its mock
executable is never invoked. A small isolated temporary test cache is retained
for diagnostics; it never deletes a live production agent's resources.

Linux `imgdrive-backend` now additionally tests physical/occupied-drive refusal,
automatic selection, unexpected actual letters, TOC mismatch, external eject,
missed notifications, restart debounce, unknown/missing/replaced devices,
modified CUE preservation, concurrent agent requests, status rows and strict
IPC framing (including truncation, excess bytes, Unicode and invalid arguments).
The existing BIN/CUE and Windows process tests remain enabled.

## Capture the installed environment first

In PowerShell, replace all example paths/letters with the actual configuration:

```powershell
$imgdrive = 'C:\Actual\ImgDrive.exe'
$flac = 'Y:\temp\music\Zelda.flac'
(Get-Item -LiteralPath $imgdrive).VersionInfo | Format-List FileVersion,ProductVersion
& $imgdrive -l
Get-CimInstance Win32_CDROMDrive | Select-Object Drive,Name,PNPDeviceID
Get-PnpDevice -Class SCSIAdapter | Format-List Status,FriendlyName,InstanceId
.\build-windows\flaccd.exe drives --imgdrive $imgdrive
```

Record any `-l` popup's ID/letter, but do not parse/assume its numeric ID equals a
Windows CDROM number. Record the exact installed help (GUI or documented help
option for that build). `flaccd drives` prints native letters, PnP IDs, observed
media state, whether the device was identified as ImgDrive, and service ancestry.
If there are no verified ImgDrive devices, retain this output and `agent.log`;
stop there rather than trying arbitrary physical letters.

Keep ImgDrive **Hide Empty Drives disabled**. The current adapter requires a
visible letter and joins it to its CDROM interface. Hidden/device-removed media
is uncertain and will retain resources, rather than guessing that a drive vanished
because an image was safely released. D: may be a physical device; use the actual
empty ImgDrive device such as E: or `--drive auto`.

## Actual Windows 10/11 integration tests

1. Save original FLAC SHA256 and, if present, adjacent user CUE SHA256. Keep
   another optical drive/image occupied to verify it is never affected.
2. Try `mount $flac --drive D: --imgdrive $imgdrive` when D: is physical or occupied.
   Expect immediate rejection, no vendor mount invocation and no generated CUE.
3. Mount with the verified empty ImgDrive letter:

   ```powershell
   .\build-windows\flaccd.exe mount $flac --drive E: --imgdrive $imgdrive 2> mount.log
   Get-CimInstance Win32_CDROMDrive | Select-Object Drive,Name,PNPDeviceID
   .\build-windows\flaccd.exe status
   ```

   Confirm requested/actual letters and PnP identity agree, ImgDrive received
   `-m:E`, and success follows optical readiness plus the expected TOC. Inspect
   stderr's before snapshot and cache `agent.log`. A ready-disc TOC mismatch must
   report expected/actual boundary/control information rather than waiting 30 s.
   A disc that never becomes ready still uses the configured safe timeout.
4. Compare the expected audio tracks, INDEX 01 starts, flags and lead-out with
   `flaccd-info`. Verify playback of first/last tracks and track skipping. Inspect
   INDEX 00/subchannel/sector fidelity with CD-aware tools separately: successful
   TOC confirmation does not prove every MMC command or index transition.
5. Close the CLI; the agent should persist and audio playback should continue.
   Use Explorer **Eject**. With a visible, empty mapped device, expect generated
   resources/state to disappear after about 5–10 idle seconds. Verify both CLI
   status and the file paths; user CUEs and the original FLAC must remain.
6. Repeat using `flaccd unmount E:`. Its result must describe the actual action:
   generated CUE removed, existing user CUE retained, and staged hard link/copy
   removed when applicable. No generic message may claim a generated CUE was reused.
7. Repeat with a missing adjacent CUE, an equivalent existing CUE, a conflicting
   CUE, a read-only source directory, cross-volume copy fallback and `--no-copy`.
   Check matching adjacent/staged basenames and relative FILE directives. No BIN
   should be generated; hashes of originals and existing CUEs must be unchanged.
8. Repeat with spaces/Unicode in both source and cache paths. Run two CLI clients
   concurrently on separate empty ImgDrive drives; unmount/eject one and confirm
   the other still works. Managed CUEs must not be shared between mounts.
9. Stop/restart the agent while mounted with `agent-stop`, followed by `status`.
   Mount resources must survive. Eject during the stopped interval; on restart,
   two confirmed empty observations should reconcile the missed event. Also
   test reboot recovery if ImgDrive restores mounts; do not delete resources
   merely because a device is absent during startup.
10. Test an uncertain mount (short timeout/vendor failure), an unknown device
    state, and a modified generated CUE. None permits automatic deletion. Inspect
    status/logs, restore known conditions, then request image-specific unmount.
    Never recursively clear the cache while an image might still use a resource.
11. If an installed build demonstrably mounts a different letter despite a valid
    selector, preserve before/after snapshots and logs. Expect a drive-selection
    error, a pending record showing the observed actual device, and retained
    resources. Use `unmount <actual-letter>:` after inspection; the requested
    unrelated letter must not trigger an unmount. Do not deliberately occupy or
    manipulate another application's drive to force this test.
12. Repeat on Windows 11 before claiming that platform was tested.

## Agent lifecycle, IPC and limitations

The default endpoint is `\\.\pipe\FlacCD-Agent-v1-<current-user-SID>`. The
matching per-user global mutex prevents duplicate servers across sessions.
Explicit protected DACLs allow only that SID; remote pipe clients are rejected,
and both ends check peer process ownership. A user with the same account is
already in the trust boundary; this does not defend against malicious code
running as that user. No network listener is opened.

The transport uses overlapped bounded reads/writes, at most 1 MiB per message.
A four-byte little-endian message length frames a versioned field vector;
`AgentProtocol.h/.cpp` implement the shared request/reply schema. Each field
vector starts with a four-byte count, followed by four-byte lengths and raw
UTF-8 field bytes. Requests begin with `FlacCD request 1`, then the CLI arguments;
NUL arguments are rejected. Replies begin with `FlacCD reply 1`, decimal exit
status, output, diagnostics, then nested status rows. Each row contains ID,
requested letter, observed actual letter, PnP ID, phase, strategy, source, CUE,
audio path, media state, and the persisted letter selector. A framed
`FlacCD ack 1` acknowledgment confirms receipt before the server disconnects.
Client paths are resolved before IPC so the agent's working directory cannot
change their meaning. This interface can be reused by a future GUI.

Concurrent clients queue behind serialized operations. Client startup has a
15-second connection deadline; a busy agent permits a longer queue wait. Request
reads/writes are bounded, replies allow lengthy mount operations, and a submitted
request is never replayed on disconnect. A timed-out client must inspect status:
the agent may have completed its operation. Mount/process/verification timeouts
retain resources. Stopping the agent does not unmount media. After acknowledgment,
the stop client waits briefly for process exit before returning.

`--imgdrive`/`--cache-dir` configure the agent at startup; a running agent refuses
conflicting configuration and suggests `agent-stop`. The agent persists until
stop/logout. There is no automatic logon task/registry entry: start any CLI
command after login (or manually start `flaccd-agent.exe`) to recover records.
Changing configuration while mounts remain in another cache does not migrate or
watch them; keep using the original cache until those mounts are detached.

The hidden window is top-level, not message-only, because media broadcasts go
to top-level windows in the active console session. Notifications trigger a full
identity-based recheck; periodic five-second scans cover missed/RDP notifications.
Long operations serialize reconciliation and can extend the normal 5–10-second
cleanup delay. Unknown media, disappearing identities, changed files or busy
handles defer cleanup and log the condition. Resources are removed only with
confirmed empty media and existing ownership validation.

Old records without device bindings are visible but not automatically unmounted
or cleaned: the old requested letter cannot prove where the disc mounted. Unmount
that exact image through ImgDrive, inspect all device states/resources manually,
and retire the old record only after confirming it is no longer used. Incomplete
preparations and partial copies also require inspection. A binding is persisted before invocation, and a separate observed binding is
recorded only after a matching TOC is seen. For a bound but never-confirmed
mount, explicit retry first issues image-specific unmount and requires all
visible optical devices accounted for and empty before cleanup. An unrelated
occupied drive can therefore defer this conservative recovery; it is never
ejected. Failed/uncertain unmounts also require explicit retry. No automatic
orphan pruning or power-loss durability guarantee is offered.

Even matching TOCs cannot prove image identity if another application races to
replace media with an identical disc. Do not manipulate the same managed image
through another program concurrently. The image-specific vendor unmount avoids
unconditional ejection of unrelated media. Numeric ImgDrive ID mapping, hidden
unlettered devices and arbitrary driver-service naming are not guessed.

Relevant primary references:

- [ImgDrive selectors and unmount semantics](https://www.yubsoft.com/imgdrive/help.html)
- [Microsoft media notifications and console/RDP limits](https://learn.microsoft.com/en-us/windows/win32/devio/detecting-media-insertion-or-removal)
- [Named-pipe ACLs](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights)
- [Live storage device numbers are not persistent](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-ioctl_storage_get_device_number)

## Changed components

- `OpticalDrive.h/.cpp`, `ProcessRunner.h`, `WindowsImgDriveHost.h/.cpp`: native
  inventory, driver ancestry, device binding and TOC diagnostics.
- `ImgDriveBackend.h/.cpp`, `MountStateStore.h/.cpp`: requested/actual mapping,
  immutable device sidecars, strict selection, safe eject cleanup and messages.
- `AgentProtocol.h/.cpp`, `MountAgent.h/.cpp`: portable framing, typed status rows,
  serialized commands and reconciliation policy.
- `WindowsAgentIpc.h/.cpp`, `FlaccdAgent.cpp`, `Flaccd.cpp`: restricted named pipes,
  single-instance agent, hidden notification window, startup and CLI client.
- `ImgDriveBackendTests.cpp`, `WindowsAgentTests.cpp`, `CMakeLists.txt`: portable
  regressions and conditional native IPC tests/targets.
- `README.md`, this guide: current architecture, CLI and qualification limits.

Existing metadata/layout parsing, decoding, export tools, Windows process quoting
and external research repositories remain unchanged.
