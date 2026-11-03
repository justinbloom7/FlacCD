# Mount recovery and optional login restoration

## Root cause and implemented recovery

The cache is an ownership journal, not a live ImgDrive mount registry. It survives
process exit and reboot intentionally: deleting CUE/backing files on CLI/agent
exit could break a still-mounted disc. Previously `ImgDriveBackend::mount()`
rejected a drive whenever a saved record matched its letter or device identity,
without reconciling first. Agent startup took only the first empty-media
observation; timer cleanup normally required another observation five seconds
later. Unmounting/pending phases, missing PnP identities and unknown media were
retained. Thus the first post-login Explorer request could encounter the stale
record before cleanup and report it as mounted. A saved `mounted` marker did not
prove live media. These are confirmed source-code findings; the exact Windows
device/IOCTL result after your reboot still requires logs from that machine.

Recovery now runs before a mount record can block a new mount, and on status or
drive-list requests. It takes two independent observations separated by 500 ms
through the host adapter, under the cache lock. Automatic recovery requires:

- Resources and a previously confirmed device binding in mounted, unmounting or
  detached phase.
- The same identified ImgDrive PnP device reporting empty media both times.
- No other visible virtual drive containing media, and no unknown/unidentified
  optical device that might still use the backing files.
- A final live-device check and all existing file identity/content/shared-resource
  ownership checks before deletion.

A recovered entry is removed and a requested mount can proceed immediately,
without waiting for background timer ticks. Recovery does not issue a vendor
unmount command and never automatically remounts an ejected image. Background
notification/timer reconciliation retains its five-second debounce and now uses
the same conservative resource-use checks. A failed vendor unmount can later be
recovered if independent observations establish absence; its exit code alone
never authorizes deletion.

Windows media checks recognize both ERROR_NOT_READY and ERROR_NO_MEDIA_IN_DRIVE;
other failures remain unknown. Missing PnP identity is **unknown**, not proof of
absence. A drive letter reused by a different device cannot authorize cleanup.
Ready media is preserved. A matching TOC is reported as TOC-compatible, not as a
unique image identifier; a different/unreadable TOC is explicitly diagnosed.

The existing ImgDrive `-l` integration has no verified machine-readable image-path
contract (some versions show a GUI list). Consequently FlacCD cannot positively
identify every arbitrary image or invisible/unlettered device. If another virtual
drive is occupied, the saved device disappeared/changed identity, or status is
unavailable, resources are retained and diagnostics explain why. Inspect
`flaccd status`, `flaccd drives`, and `agent.log` in the cache. Do not manually
delete backing files while a live/pending image may reference them. Supporting
additional positively identified absence cases requires a verified ImgDrive
image-query interface; FlacCD does not guess or parse undocumented GUI output.

An interrupted mount that never reached confirmed binding remains uncertain:
ImgDrive may still finish asynchronously after its launcher exits. Incomplete
preparations and changed ownership records remain available for inspection.
Missing original music does not prevent cleanup of an otherwise confirmed-empty
mount; it does prevent a new mount. User-owned external CUEs are always retained.

## Crash-safe journal and resource handling

Metadata is written to an exclusively created temporary file, flushed, then
published by same-filesystem rename. Windows uses write-through publication;
Linux also syncs the destination directory. CUE writes and completed compressed
copies are flushed before being recorded as usable. Known immutable records
are not rewritten in place. Interrupted `.pending-*` writes stay outside active
mount directories and cannot masquerade as a complete phase marker.

After resources pass ownership checks and are removed, metadata directories are
atomically renamed from `mount-*` to `.retired-mount-*` before metadata deletion.
A crash during metadata deletion therefore cannot leave a half-deleted active
record blocking future requests. Inactive pending/retired remnants are preserved,
not recursively swept. These changes improve crash consistency; they do not
promise recovery from arbitrary filesystem corruption or storage hardware that
does not honor flushes. Malformed legacy records remain protected for inspection.

## ImgDrive reboot persistence: vendor evidence

Yubsoft's [ImgDrive 2.0.2 release notes](https://www.yubsoft.com/news5.html) document
an automount feature. The vendor says the driver saves the last mounted images
to the registry during shutdown; enabling automount adds an entry under the
current user's `Software\Microsoft\Windows\CurrentVersion\Run`. The documented
commands are:

```text
imgdrive.exe -automount
imgdrive.exe -automount0
```

The first restores the previous images after startup; the second restores and
then exits ImgDrive. The current [CLI help](https://www.yubsoft.com/imgdrive/help.html)
lists mount/unmount/list switches but does not specify a current automount
configuration API or a machine-readable image query. These are vendor-documented
capabilities, not tests performed against your installed ImgDrive build.

Before relying on this feature, inspect your installed version's Options
(Ctrl+S)/help for its automount setting and test it in the VM. The exact current
option labels, edition support, persisted FLAC/CUE handling, mapped-network-drive
readiness, and external-eject semantics remain unverified. Saving at shutdown
does not guarantee that the latest mount survives power loss or a hard reset.
Retained CUE paths must still exist when restoration runs. A delayed ImgDrive
automount could race FlacCD's confirmed-empty recovery, so test startup ordering
before enabling it. FlacCD does not set ImgDrive's automount options or startup
entries; its separate opt-in startup entry is described below.

## Optional FlacCD login restoration — implemented, opt-in

Enable from the permanently installed copy, as the normal Windows user:

```text
"%LOCALAPPDATA%\Programs\FlacCD\flaccd.exe" restore enable
"%LOCALAPPDATA%\Programs\FlacCD\flaccd.exe" restore status
```

`enable` captures currently confirmed mounts and remembers future successful
mounts, including layout choice, source path, device identity and copy policy.
For old mounts it chooses a matching layout and conservatively forbids a new
compressed copy unless that mount already used one. Reissuing enable is an
explicit rearm decision for suspended entries. No login entry is installed by
default. Disable ImgDrive's own automount before using FlacCD restoration.

Other commands (run from the installed directory, or use its full EXE path):

```text
flaccd restore run [--timeout 1..300]
flaccd restore forget "Y:\Music\Album.flac"
flaccd restore disable
```

Options `--cache-dir DIR` and `--imgdrive PATH.exe` select a custom configuration.
Configuration commands can adopt this executable's existing custom startup
configuration; foreign/edited startup values are preserved. `disable` works
without discovering ImgDrive or starting the agent. It retains desired entries
but disables restoration; `forget` removes only the named intent, leaving active
media and resources untouched. `status` reports enabled state, startup ownership,
and desired/suspended paths. It does not start an agent or restore anything.

Enabling creates only the current user's `FlacCD.Restore` value under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. Its fully quoted REG_SZ
command starts the existing EXE with `restore run --startup`; it does not invoke
CMD/PowerShell. The entry contains explicit cache/ImgDrive paths. Windows documents
[a 260-character Run-command limit](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys);
overlong commands are rejected without truncation. Choose shorter local
installation/cache paths if necessary. Startup mode hides only its own console,
never an inherited terminal, and writes a bounded `restore.log` (up to 1 MiB) in
the managed cache. CLI runs also print results. Each enabled run replaces the log.

The startup helper is serialized against duplicate helpers and has a 60-second
default retry deadline (manual `run --timeout` accepts up to 300 seconds). It calls
one `restore-step` at a time through the existing current-user IPC, checking fresh
state each time. Readiness/missing-file retries wait one second. IPC connection,
queue and response waits respect the helper deadline. A lost/uncertain response
terminates the helper rather than replaying a possibly submitted mount. Blocking
filesystem operations on a custom remote cache still depend on OS timeouts;
prefer the default local cache. Network music can become available during the
bounded retries. Missing/moved sources are reported and retained for a future
login; FlacCD does not scan for substitutes or move the original music.

Restoration requires the remembered PnP device (whose current letter may differ)
to be identified and empty. Occupied/unknown media is not replaced. A possibly
already-mounted image is not duplicated. Existing managed media is skipped only
after validating its saved binding and TOC; TOC equality is not a unique image
identifier. Other untracked ready virtual media or a pending mount blocks
restoration. A changed/missing device identity is reported rather than guessed.
No raw BIN or second decoder is introduced; normal mount/CUE staging is reused.

Before submitting a vendor mount, FlacCD atomically suspends that desired entry.
Only a confirmed successful mount clears the fence and updates its active mount
ID/current session. A crash or timeout during invocation therefore cannot trigger
blind automatic replay at the next login. Repeated/concurrent successful calls
are idempotent. Suspended/uncertain records require inspection and explicit
`restore enable` to rearm; existing pending resources still cannot be replaced.

### Eject, session and maintenance semantics

The desired list is separate from active ownership records. Versioned snapshots
live under `<cache>/restore/`, retain a previous generation for inspection and
publish atomically. A corrupt newest snapshot fails closed; it never falls back
to old intent that might resurrect an ejected disc. Interrupted unpublished
snapshots are ignored/preserved. Unknown cache contents are not swept.

A [Windows logon SID](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens)
identifies the interactive session. A confirmed empty-media observation in the
same login removes the corresponding desired mount before artifact cleanup:
Explorer eject is honored even when resources subsequently cannot be deleted.
A different login preserves intent during reboot cleanup, allowing restoration.
Explicit FlacCD unmount removes intent before issuing the vendor command, even
if unmount subsequently fails. Graceful `agent stop` suspends entries because
ejects while monitoring is stopped cannot be reliably observed. It retains all
active media, state and backing files. `restore enable` explicitly rearms; an
installer upgrade rearms only mounts that are still confirmed live after its
maintenance gap. Uninstall disables the owned startup registration and retains
the data. A fresh installation never opts the user in automatically.
Installer maintenance also pauses/drains an existing login helper before file
replacement. Suspension is checked again before an agent is started, so a
draining helper cannot automatically restart an agent that was just stopped.

The helper exits when finished; the existing mount/eject agent continues normal
monitoring. No second always-running daemon, privileged service, scheduled task
or administrator requirement is introduced. Multiple simultaneous interactive
logins for the same user remain outside the existing single per-user agent model.

An external eject immediately followed by a hard reset before the agent observes
it is fundamentally ambiguous: no persisted intent can prove that missed event.
The opt-in list may still request restoration in that case. Wait for confirmed
eject cleanup before reboot when that intent matters. Explicit `flaccd unmount`
records cancellation first. Ordinary monitored ejects and graceful-stop gaps
have the conservative behavior above.

### Windows restoration acceptance tests

These require the installed Windows build and ImgDrive; they have not been run
by the Linux development agent. Disable vendor automount for this qualification.

```bash
cmake --build build-windows
ctest --test-dir build-windows -R '^(imgdrive-backend|windows-agent-ipc|windows-restore-startup)$' --output-on-failure
ctest --test-dir build-windows --output-on-failure
cmake --build build-windows --target flaccd-installer
```

1. Verify a fresh install has `restore status` disabled and no `FlacCD.Restore`
   Run value. Existing Explorer mount/eject, shell registration and smoke tests
   must keep working. The native startup test uses an isolated HKCU fixture,
   never the real Run key, and checks unrelated values remain unchanged.
2. Mount Zelda.flac, run `restore enable` from the installed EXE, and check its
   desired path and fully quoted per-user Run command. Reissue enable: no
   duplicate entry or elevation prompt. Repeat with spaces and Unicode paths.
3. Reboot with the CD mounted. Wait for the login helper to finish, inspect
   `restore.log`, and verify one restored CD with the expected TOC/playback.
   Run `restore run` twice: no additional disc or resource duplication.
4. Eject in Explorer, wait for confirmed cleanup, and reboot. The desired entry
   must be gone and the disc must stay ejected. Repeat with explicit CLI unmount
   immediately before reboot; intent is persisted before unmount execution.
5. Run `agent stop` while mounted: resources remain, restore status shows
   suspended. Eject while stopped and restart the agent: it must not remount.
   Run `restore enable` to explicitly rearm, then `restore run` or log in again.
6. Delay ImgDrive/network readiness, disconnect a mapped music drive, and move a
   source file. The helper must exit at its bounded deadline with actionable
   logs, retain intent, never substitute another file, and restore when the
   original path returns during a later run/login. Test multiple albums too.
7. Put unrelated media in the preferred virtual drive, change PnP identity, or
   make device status unknown. No image may be replaced and no uncertain resource
   removed. Start two helpers together: only one may perform restoration.
8. Hard reset during restoration. If invocation was uncertain, the corresponding
   entry must stay suspended, resources retained, and no automatic replay occur.
   Corrupt the newest desired snapshot in a test cache: fail closed, no fallback
   to an earlier snapshot. Never perform destructive fault tests on user music.
9. Upgrade while restoration is enabled and discs remain mounted. Confirm the
   owned startup entry remains, live mounts are rearmed, and unknown entries
   remain suspended. Uninstall: startup registration disappears while desired
   data and backing resources remain. Test explicit custom cache/ImgDrive paths.
10. Run `restore disable` with ImgDrive absent: it must not start an agent or
    require ImgDrive. Reboot and verify no restoration. Edit the startup value
    to a different command: configuration/removal must preserve it and report
    the conflict rather than overwriting it.

## Tests and Windows qualification

Portable fake-host tests cover reboot/restart remount, confirmed missing media,
genuine ready media, different TOCs, unknown status, changed PnP identities,
another occupied virtual drive, readiness changes between observations, missing
source FLAC, generated/user CUE and staged-copy cleanup, repeated recovery,
interrupted vendor calls, interrupted metadata writes, and no automatic restore
after eject. Existing resource modification, sharing, agent stop/restart and
external-eject cases remain enabled. Desired-mount tests additionally cover
capture, future mounts, login changes, concurrent helpers, multiple albums,
source return, same-session eject removal, stop/rearm, corrupt snapshots,
pre-invocation uncertainty fences, and startup command ownership/length limits.
A previous test expecting indefinite
retention after a failed unmount now requires cleanup when later observations
independently prove absence.

On Windows 10/11 with the installed ImgDrive version:

```bash
./build-windows/flaccd.exe agent stop
cmake --build build-windows
ctest --test-dir build-windows -R '^(imgdrive-backend|windows-agent-ipc)$' --output-on-failure
ctest --test-dir build-windows --output-on-failure
```

1. Mount through Explorer, stop the agent without unmounting, and restart it.
   A genuine mounted disc and its backing files must remain usable.
2. Mount, reboot with vendor automount disabled, and immediately mount again
   through Explorer. Confirm stale-state recovery, correct tracks, fresh owned
   CUE, no duplicate record, and normal eject cleanup. Collect actual PnP IDs and
   media-state logs before/after reboot. Repeat with explicit and automatic drive
   selection and staged-copy/hard-link strategies.
3. Repeat external eject while the agent is stopped, status/recovery twice, and
   remount. The original FLAC/user CUE must remain unchanged. No restoration is
   attempted unless the user explicitly mounts again.
4. Repeat controlled VM hard resets at preparation, invocation, confirmed mount,
   and cleanup phases. Confirm uncertain operations preserve resources and emit
   an actionable diagnostic, while confirmed-empty completed mounts recover.
5. Insert a different image on the recorded device, change drive identities or
   letters, make ImgDrive unavailable, and occupy another virtual drive. No
   unrelated drive may be ejected and no possibly live backing files deleted.
6. Separately test vendor automount on/off, normal restart versus hard reset,
   external eject before shutdown, and delayed network/ImgDrive readiness. Record
   the ImgDrive version/settings and verify that startup does not race cleanup.

Real reboot, Windows media-error mappings, driver readiness, and ImgDrive
automount have not been tested in this Linux development session.
