# ImgDrive backend: Windows qualification

The staging coordinator has Linux mock tests. The revised Windows filesystem
code and end-to-end mounting have **not been built or executed in this session**.
The user manually verified audio-CD playback and skipping with same-directory,
matching-basename CUE/FLAC pairs. A cache-only CUE referencing an absolute FLAC
elsewhere failed with the installed ImgDrive. These observations motivate the
backend policy; they do not qualify this new implementation, Unicode, Windows 11,
or every MMC command.

## Prerequisites and build

- Windows 10/11 with an independently installed, working ImgDrive and an empty
  virtual optical drive. Use its existing driver; FlacCD never installs one.
- Use the actual executable path or its registered App Paths/PATH discovery.
  `--imgdrive` overrides discovery, as does `FLACCD_IMGDRIVE`. There is no
  hardcoded installation directory.
- Keep the selected empty drive visible in ImgDrive. Missing/hidden drives or
  drives that cannot be verified empty are refused. Do not change Secure Boot,
  Memory Integrity, test-signing, or other security configuration for this test.
- A Windows C++20 build environment with CMake, Ninja, pkg-config, and libFLAC
  development files. For example, use an existing MSYS2 UCRT64 environment with
  matching tools/dependencies. No WDK is needed. Keep the relevant runtime DLLs
  available on PATH. Linux binaries cannot run as Windows executables.

From the configured Windows build environment:

```sh
cmake -S . -B build-windows -G Ninja
cmake --build build-windows
ctest --test-dir build-windows --output-on-failure
```

Windows builds additionally register `windows-process` (real CreateProcessW
against a local test helper, not ImgDrive) and `mount-cli-help`. The process
test covers literal Unicode/space/quote/backslash arguments, nonzero exit
status, missing/invalid executables, a noisy child, and timeout handling. Run
these before the real mount tests. They are not executed on Linux.

## Windows 10 integration checklist

Record the Windows build, ImgDrive version/executable path, drive letter,
exit codes, and stderr logs. Choose the existing ImgDrive drive; **F:** was
reported by the user's `-l` popup, but verify it on the actual machine.

In PowerShell, using the actual paths:

```powershell
$env:FLACCD_IMGDRIVE = 'C:\Actual\ImgDrive.exe'
$flac = 'Y:\temp\music\Zelda.flac' # Replace with your actual file
$before = (Get-FileHash -Algorithm SHA256 -LiteralPath $flac).Hash
.\build-windows\flaccd.exe --help
.\build-windows\flaccd.exe drives
.\build-windows\flaccd.exe mount $flac --drive F: --layout auto 2> mount.log
.\build-windows\flaccd.exe status
```

1. **Mount Zelda.flac from a writable directory with no adjacent CUE.**
   Confirm exit 0, `Zelda.cue` beside the actual original `Zelda.flac`, and
   `FILE "Zelda.flac" WAVE`. Confirm the log reports an adjacent CUE and no
   audio copy, all expected tracks/titles, and exact INDEX 00/01 timestamps.
   For the previously inspected eight-track Zelda fixture, track 2 had
   `08:08:12` and `08:08:27`; verify against your actual embedded metadata.
2. **Check Windows audio-CD recognition.** Verify an optical device in Device
   Manager and eight audio tracks in a CD-aware application. The wrapper checks
   readiness and TOC track numbers/audio flags/INDEX 01 starts/lead-out, but that
   alone does not establish complete MMC or subchannel fidelity.
3. **Play first and last tracks.** Verify expected audio, stereo, speed, and no
   truncation. Close FlacCD first: the disc must remain usable after its process
   exits, with the original FLAC and chosen adjacent/staged CUE retained.
4. **Skip between tracks.** Test forwards/backwards and track 2's stored pregap.
   For precise fidelity, use a CD-aware diagnostic/ripper to inspect INDEX 00/01
   and sector reads around boundaries; playback success does not prove those
   commands are exact.
5. **Unmount through FlacCD:**

   ```powershell
   .\build-windows\flaccd.exe unmount F: 2> unmount.log
   .\build-windows\flaccd.exe status
   ```

   Confirm the drive is empty and only the associated owned CUE/state entry was
   removed. `cache.owner` remains. A mounted unrelated image must not be ejected.
6. **Remount the same FLAC**, play again, and unmount. Repeated mount while a
   record already exists should report that state and issue no replacement
   mount. Unmount of a drive without a FlacCD record must be refused.
7. **Confirm no BIN was generated.** Inspect the printed cache path and music
   directory. An adjacent-CUE mount creates only small CUE/state files; staged
   fallback may hard-link or copy the compressed FLAC, and must log that choice.
   Existing export tools still generate BINs only when explicitly requested.
8. **Confirm original FLAC unchanged:**

   ```powershell
   $after = (Get-FileHash -Algorithm SHA256 -LiteralPath $flac).Hash
   if ($before -ne $after) { throw 'Original FLAC changed' }
   ```

## Staging and ownership qualification

Perform these with disposable FLAC fixtures and preserve logs:

1. **Equivalent existing CUE:** put an external `Zelda.cue` beside the source,
   with correct normalized layout and a WAVE FILE reference to that FLAC. Add
   harmless whitespace/comments. Hash the CUE, mount, play, unmount, and confirm
   the CUE hash is unchanged and the file remains.
2. **Conflicting existing CUE:** change an index in a disposable external CUE.
   Confirm the conflict is logged, the user CUE is unchanged, and a cache entry
   contains matching `Zelda.cue`/`Zelda.flac` with a relative FILE line. Verify
   successful playback and cleanup of only that entry on unmount.
3. **Read-only source directory:** use a location already restricted to read-only
   for this account, with no adjacent CUE. Do not change system security settings.
   Confirm actual creation denial triggers isolated staging. On a compatible
   same-volume filesystem, confirm the log reports a hard link and no copy.
4. **Copy fallback:** use an available source on another volume or filesystem
   that refuses cache hard links. Confirm the copy log includes both paths/size,
   staged FLAC SHA256 equals the original, and ImgDrive mounts only after the
   completed copy. Unmount must remove the staged copy, never the original.
5. **No-copy:** repeat case 4 with `--no-copy`. Expect a clear error, no copy,
   and no mount invocation. The option must still allow adjacent reuse/creation
   and successful hard-link staging.
6. **Concurrent mounts:** with two existing empty ImgDrive drives, mount the
   same FLAC in both. Confirm distinct CUE paths, unmount the adjacent-CUE mount,
   and verify the staged mount still plays before unmounting it.
7. **Changed ownership:** after a successful disposable adjacent mount, modify
   or replace its generated CUE. Unmount must preserve it and report why cleanup
   was refused. Do not edit the real album's CUE for this test.
8. **Recovery:** exit the CLI, restart it, inspect status, and unmount. Also
   qualify reboot recovery. If ImgDrive persists an image, all resources must
   remain; if it drops the image, an image-specific unmount may still be needed.
   If the vendor reports an already-absent image as an error, preserve resources
   for inspection rather than treating that result as confirmed detachment.

## Additional qualification

- Repeat with a FLAC filename/directory containing spaces, `&`, brackets, and
  non-ASCII characters (for example `C:\Music\音楽 é\Zelda album.flac`). Use
  an existing copy; never rename a mounted source. Confirm the UTF-8 CUE
  uses a relative matching FLAC basename and is accepted by ImgDrive. Test an ordinary UNC path
  separately if needed; network paths, long paths, and ImgDrive encoding support
  have not been runtime-qualified here.
- Put an unrelated image in another ImgDrive drive and confirm it remains
  mounted throughout FlacCD mount/unmount. An occupied selected drive must be
  refused before invoking a mount command.
- Test a missing/invalid executable and a short timeout. A timeout is uncertain:
  inspect `flaccd status` and ImgDrive, then run `flaccd unmount F:` with the
  correct executable configuration. Pending state/CUE must remain until
  successful image-specific unmount and an empty/missing target are confirmed.
- Close the mounting CLI or simulate an interrupted command. Start a new CLI
  and verify the persistent record is visible. The named mutex is released by
  Windows after process death. If mounting may have reached ImgDrive, retry
  image-specific unmount rather than deleting the CUE.
- Manually eject an owned image using ImgDrive, then inspect the stale record
  with `status` and retry FlacCD unmount. If the installed ImgDrive reports an
  already-absent image as failure, FlacCD deliberately retains the record/CUE;
  inspect it manually rather than deleting something potentially still in use.
- Test native/text layout conflict and source-less PREGAP/POSTGAP rejection;
  neither should create a mount or uncompressed audio.
- Repeat qualification on Windows 11 before claiming support was tested there.

## Known boundaries

`-l` is vendor-documented but has no documented machine-readable schema. The
user's installed build displays a popup (`1`, `F:`), not a console listing.
`flaccd drives` invokes that listing and may require closing its popup before
returning. If it emits stdout/stderr, that output is also shown; otherwise the
CLI points to the GUI. **Automatic drive selection is therefore disabled**:
pass `--drive F:` after inspecting the installed virtual-drive configuration.
There is no popup scraping and no assumption that drive 1 is free.

Commands use vendor-documented `-m:F <owned-cue>` and `-u <owned-cue>`, never
bare `-u` or `-u:F`. FlacCD does not automatically eject an unrelated image,
create drives, install/update ImgDrive, or change driver/security configuration.
A race with another application manipulating the same drive cannot be locked
through ImgDrive's documented CLI; avoid concurrent GUI/third-party changes.

Process diagnostics are bounded to 64 KiB; capture keeps draining so verbose
children cannot deadlock it. A timeout terminates only the process started by
FlacCD. Existing ImgDrive GUI/driver activity can outlive that process, so state
is retained. Timeouts apply separately to locking, process execution, and drive
confirmation; they are not one whole-command deadline.

`status` distinguishes recorded phase from current media presence. It cannot
prove current image identity after another program replaces the media, because
no documented image-identity query was verified. TOC confirmation at mount does
not verify INDEX 00 subchannels, sector bytes, ISRC, CD-TEXT, or PLAY AUDIO.
These require the manual tests above. Native first-track pregaps are retained
in the CUE; a backend TOC shift or other mismatch fails confirmation instead of
silently adjusting the layout.

Cache preparation is renamed into place before invocation. Phase markers are
append-only. Corrupt/partial markers, changed owned files, symlinks, and unexpected
files prevent cleanup and are preserved for inspection. Modified adjacent CUEs
do not prevent requesting unmount, but must survive cleanup with a diagnostic. A crash during preparation
can leave `.prepare-*` directories or `preparing` records (including partial
compressed copies) that were never passed to ImgDrive. Their ownership intent
is recorded, but without completed file identities automatic deletion is refused;
inspect them manually after confirming no image uses them. A crash
in the final small cleanup window can leave an incomplete entry requiring manual
inspection. Never recursively clear the cache while an image might use a CUE.
No automatic orphan pruning is attempted. This is not a power-loss durability
or adversarial same-user tampering guarantee.

Sources: [ImgDrive command documentation](https://yubsoft.com/imgdrive/help.html),
[ImgDrive features/Windows versions](https://www.yubsoft.com/imgdrive/),
[Microsoft CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw),
[READ TOC EX](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddcdrm/ni-ntddcdrm-ioctl_cdrom_read_toc_ex).

## Staging changed-file map

| Files | Responsibility |
|---|---|
| `apps/Flaccd.cpp` | `--no-copy`, strategy/resource logging and help |
| `include/flaccd/CueSheetWriter.h`, `src/CueSheetWriter.cpp` | Validated relative FLAC FILE output through the shared writer |
| `src/CueTextMetadata.cpp` | Accept optional UTF-8 BOM for external CUE reuse |
| `include/flaccd/mount/ImgDriveBackend.h`, `src/mount/ImgDriveBackend.cpp` | Existing/adjacent/staged selection and validation |
| `include/flaccd/mount/MountFileSystem.h`, `src/mount/MountFileSystem.cpp` | Mockable exclusive creation, hard links, bounded verified copies, file identity and deletion |
| `include/flaccd/mount/MountStateStore.h`, `src/mount/MountStateStore.cpp` | Persistent ownership, old-record recovery and conservative cleanup |
| `tests/ImgDriveBackendTests.cpp` | Strategy, failure, race, concurrent ownership and recovery coverage |
| `CMakeLists.txt` | Filesystem source, Windows API definitions and test threading |
| `README.md`, `WindowsImgDriveTesting.md` | CLI, policy, evidence limits and manual qualification |

The Windows process adapter and its argument round-trip tests are unchanged.
Portable track layouts, CDDA decoding, BIN/CUE exports, external repositories,
and Windows driver code are unchanged.

## Diagnosing the Windows argument round-trip regression

The original `Windows argv round trip` message came from one compound assertion
in `WindowsProcessTests.cpp`: no timeout, zero child exit code, and byte-equal
UTF-8 echo output. It did not identify which condition was false.

There is a deterministic output-mode defect in the original probe: UCRT opens
stdout in text mode, including when the inherited handle is a pipe. Writing
`'\n'` through `std::cout` therefore emits CRLF, while the expected string
contains literal LF. For the original six arguments, the expected payload has
46 bytes; text-mode translation produces 52 bytes if arguments arrive intact.
The first mismatch is at byte offset 10: expected `0a`, actual `0d 0a`.
These byte counts describe the source-level prediction, not a captured Windows
run from this Linux development environment.

The probe now calls `_setmode(_fileno(stdout), _O_BINARY)` before output, with
the same treatment for stderr. This fixes the probe's byte protocol. Production
capture still returns exactly what the child writes; it does **not** strip CR,
normalize newlines, change console code pages, or relax expected byte equality.

The original echo test is retained. A second mode dumps each raw UTF-16 `wmain`
argument as length-framed ASCII hex, without passing it through filesystem-path
conversion. The parent independently converts expected UTF-8 using strict Win32
Unicode APIs, compares count and each UTF-16 argument exactly, and reports:

- Child exit status and timeout separately for echo and argument dump.
- Expected and actual complete payload lengths and escaped bytes (`\r`, `\n`,
  `\t`, `\xNN`).
- Each payload argument's index, UTF-8 byte count, UTF-16 code-unit count, and
  escaped contents (`\uXXXX`, including surrogate pairs).
- A specific assertion for process failure, argument count, UTF-16 mismatch,
  UTF-8 mismatch, malformed dump, or byte transport mismatch.

Coverage includes the original arguments, empty and repeated empty arguments,
spaces, literal quotes, zero through eight backslashes before quotes, trailing
backslashes, drive/UNC paths, tab/CR/LF, shell metacharacters, and supplementary
Unicode. The separate portable `process-argument-diagnostics` test verifies the
diagnostic encoding/parser and rejects truncation, malformed lengths/hex, and
CRLF-translated framing. No arguments or assertions were skipped.

`quoteWindowsArgument()` follows CRT parity rules: double backslashes before
closing quotes, and use `2*n + 1` backslashes before literal quotes. The native
UCRT test executable invokes the probe directly through `CreateProcessW`; its
internally constructed payload never goes through an MSYS2 shell. MSYS2's
shell-to-native path conversion is therefore not a fix for this output-mode
bug. GCC defaults to UTF-8 source/execution encoding; `-municode` selects wide
startup, and the tests use `wmain` to observe UTF-16 arguments. Expected and
actual raw UTF-16 are compared independently of display encoding.

Windows is unavailable here, as are Wine and a MinGW cross-compiler. The Windows
fix must still be rerun on the reported Windows 10/UCRT64/GCC 16.2 environment.
In the **MSYS2 UCRT64** terminal at the source root, rebuild both test and probe:

```sh
cmake -S . -B build-windows -G Ninja
cmake --build build-windows --target flaccd-windows-process-tests flaccd-process-probe flaccd-process-diagnostic-tests
ctest --test-dir build-windows -R "^windows-process$" -V
ctest --test-dir build-windows -R "^process-argument-diagnostics$" --output-on-failure
```

Then run the full Windows suite. If the process test still fails, retain the
complete verbose output: it now distinguishes a remaining argv/encoding defect
from a newline transport mismatch. Do not rebuild only the parent test; a stale
probe will not understand the new argument-dump mode.

References: [UCRT text/binary mode](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/setmode?view=msvc-170),
[CRT argument parsing](https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments?view=msvc-170),
[GCC character encodings](https://gcc.gnu.org/onlinedocs/gcc/Preprocessor-Options.html),
[MSYS2 shell path conversion](https://www.msys2.org/docs/filesystem-paths/).

## Root cause of the earlier `mount-...\disc.flac` error

The earlier implementation created only cache-local `disc.cue`, state and phase
files. Its FILE line named the original absolute FLAC path; it created no
`disc.flac`, link or audio copy. Resources survived process exit and failed
confirmation, so premature cleanup was not the cause in that path.

Subsequent user tests found that ImgDrive looked for `disc.flac` next to
`disc.cue`, or `Zelda.flac` next to `Zelda.cue`. Placing a matching FLAC beside
the CUE worked; a separate cached CUE with an absolute FILE path did not. The
new strategy therefore always supplies same-directory matching basenames,
without assuming this behavior applies to other CUE consumers. Logs identify
the selected strategy, absolute CUE path, relative FILE reference and actual
audio resource before invoking the unchanged Windows process adapter.

Do not retrofit an old mounted `disc.cue` by editing it. Inspect its recorded
state and perform image-specific unmount first; old manifests remain supported
for that cleanup. Then rebuild and use a new mount with the revised strategy.
A passing mock suite does not prove the installed proprietary application will
accept every encoding, path, filesystem or CUE feature: run the checklist above.
