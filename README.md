# FlacCD

Initial platform-independent C++20 foundation for a future FLAC audio CD
virtualization utility, with a Windows-only ImgDrive mounting adapter.
`flaccd-info` reads native FLAC metadata using libFLAC;
it does not decode audio or mount a drive. `flaccd-export` uses the core CDDA
reader for full-disc, track, range, split-track, and BIN/CUE exports. No custom
Windows driver is built or installed.

## Installing on Windows

End users need Windows 10/11 **x64** and a separately installed, working
[ImgDrive](https://www.yubsoft.com/imgdrive/) virtual CD backend. FlacCD's
installer includes its executables and any required non-system runtime DLLs;
GCC/C++ runtimes and libFLAC are linked statically when supported. You do **not**
need MSYS2, MinGW, Git Bash, CMake, Inno Setup, or a separate libFLAC installation
on the destination computer.

1. Install ImgDrive using its vendor installer and create an available virtual
   drive. Its driver installation is separate from FlacCD and may require
   administrator approval.
2. Run `FlacCD-<version>-windows-x64-setup.exe` as your normal Windows user.
   FlacCD defaults to `%LOCALAPPDATA%\Programs\FlacCD` without requiring elevation.
3. Right-click a FLAC album containing an embedded CUE sheet and choose
   **Mount as Audio CD** (under **Show more options** on Windows 11).

The FlacCD installer registers the Explorer command and installs its runtime
dependencies from the audited deployment. **It does not download, install, or
redistribute ImgDrive or its driver.** Missing ImgDrive produces an advisory;
FlacCD installation can finish, but mounting requires ImgDrive. For a custom
ImgDrive location, configure the user environment variable `FLACCD_IMGDRIVE`,
or use `--imgdrive` with the CLI. The default FLAC file association is unchanged.

Run a newer installer to upgrade. It stops the agent gracefully and preserves
mount records, backing files, and user cache/state. Uninstall through Windows
Apps; preferably unmount managed discs first. Uninstall does not remove ImgDrive
or delete retained mount resources. See [WindowsInstaller.md](docs/WindowsInstaller.md)
for maintenance and recovery details.

## Building on Windows with MSYS2 UCRT64

Build prerequisites are Git (or a source checkout), a C++20 compiler, CMake
3.20+, Ninja, pkg-config/pkgconf, and libFLAC development headers/libraries
1.3.3+. Use the **MSYS2 UCRT64** environment consistently; do not mix its
libraries with MSYS or MINGW64 libraries. Inno Setup is needed only to build the
setup EXE. ImgDrive is needed for real mounting tests, not compilation or mock
unit tests.

On a fresh [MSYS2](https://www.msys2.org/) development installation, open its
UCRT64 shell and install missing build packages:

```bash
pacman -S --needed git mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-flac
```

These are build-machine dependencies, not packages users must install to run
FlacCD. See the [MSYS2 environment guide](https://www.msys2.org/docs/environments/)
and its [FLAC](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-flac)
and [pkgconf](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-pkgconf)
package pages.

Install [Inno Setup](https://jrsoftware.org/isinfo.php) separately. The command
below uses your Inno Setup **7** installation at
`C:/Program Files/Inno Setup 7/ISCC.exe`; adjust the path if yours differs.
Inno Setup 6.3+ is the script's minimum version. From the FlacCD repository root
in UCRT64:

```bash
# Before rebuilding an existing checkout, stop the running development agent:
./build-windows/flaccd.exe agent stop
```

Skip that stop command on a first build when the executable does not exist.
If the running agent belongs to an installed copy, use that copy's `flaccd.exe`
to stop it. This releases the agent EXE lock without unmounting active discs.

```bash
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release "-DFLACCD_ISCC=C:/Program Files/Inno Setup 7/ISCC.exe"
cmake --build build-windows
ctest --test-dir build-windows --output-on-failure
cmake --build build-windows --target flaccd-installer
```

Use **Release**, not `Releas`. The installer target runs the existing deployment
audit and clean-PATH CMD/PowerShell smoke checks before compiling the setup EXE.
When ISCC is configured, CTest also registers the installer build test. Build
outputs include:

- `build-windows/installer/Release/FlacCD-<version>-windows-x64-setup.exe`:
  distributable installer.
- `build-windows/deploy/Release/FlacCD/`: audited portable deployment folder.
- Optional ZIP: run `cmake --build build-windows --target flaccd-package`.

Version numbers come from `project(FlacCD VERSION ...)` in `CMakeLists.txt`.
Distribute the installer or complete staged folder/ZIP, rather than individual
EXEs from the build directory. The staged package supplies required DLLs; no
MSYS2 directories need to be added to the destination computer's PATH.

## Building on Linux

Build dependencies: a C++20 compiler, CMake 3.20 or newer, Ninja, pkg-config,
and libFLAC development headers/library (1.3.3 or newer). Windows mounting and
the Inno Setup installer require Windows; portable core/export tests run natively
on Linux.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/flaccd-info /path/to/disc.flac
```

For more detail about Windows delivery without MSYS2/MinGW on the destination,
see the deployment documentation below.
The audited folder/ZIP contains all five applications and any required non-system
DLLs. MinGW GCC/C++ runtimes are static by default; winpthreads is static when a
link probe supports it. MinGW builds prefer a verified real `libFLAC.a` (with
private dependencies and `FLAC__NO_DLL`); unavailable/incompatible archives fall
back to bundled libFLAC and its recursive DLL dependencies.
See [WindowsDeployment.md](docs/WindowsDeployment.md) for build/package commands,
import reports, cross-compilation and clean cmd.exe/PowerShell verification.

The `flaccd-core` library exposes owned metadata values through
`include/flaccd/FlacMetadataInspector.h`. The CLI displays STREAMINFO audio
parameters, duration, CD-DA format compatibility, native CUESHEET catalog,
lead-in, tracks, ISRCs, PRE flags, indexes, and lead-out. Track offsets are
stream-relative sample frames; index offsets are track-relative sample frames.
A zero total sample count means unknown, so duration is reported as unknown.
No audio conversion occurs. Metadata inspection does not verify audio integrity
or establish that a disc layout is suitable for mounting.

Vorbis comment names are matched case-insensitively against `CUESHEET`. All
matching values, including duplicates and empty values, are retained in order
and printed verbatim. The CLI also lists ARTIST, ALBUMARTIST, ALBUM, TITLE, and
TRACKNUMBER Vorbis tags, preserving duplicate values. A separate descriptive
summary for each embedded CUE shows its disc TITLE as the album, PERFORMER as
the artist, and each track's TITLE and PERFORMER. Tracks without a performer
use the disc performer for display. Sources are labeled separately, without
merging potentially conflicting tags. UTF-8 text is preserved.

The shared text parser in `CueTextMetadata.h` reads descriptive fields and layout
commands once. It retains errors with line numbers so descriptive inspection can
show partial metadata, while track-layout construction rejects any parse errors.
Commands, track/file types, and flags are case-insensitive. Quoted and unquoted
descriptive values, UTF-8 text, and LF/CRLF line endings are supported. Duplicate
directives, malformed values, and unknown commands are diagnosed. REM comments
are ignored; SONGWRITER is accepted but not used by the layout model.

The immutable `TrackLayout` in `include/flaccd/TrackLayout.h` has factories for
inspected native metadata and parsed embedded text. Both factories reuse the
existing metadata models. It exposes tracks, indexes, flags, ISRCs, catalog,
gap declarations, lead-in/lead-out, and a region map. `flaccd-info` prints each
candidate separately, reports rejected layouts, and compares valid candidates
for conflicting structure. It never chooses a preferred source or merges them.
An agreement message covers shared structural fields only; names and unknown
fields are not inferred. Native CUESHEET does not encode DCP, so native DCP
remains unknown; textual DCP is set by FLAGS, defaulting to false when omitted.
Absent ISRC/catalog values remain unspecified.

Position and gap rules:

- Source sectors start at the containing FLAC's first sample frame. One sector
  is exactly 588 stereo sample frames. CUE timestamps use integer arithmetic:
  `(minutes * 60 + seconds) * 75 + frames`. Seconds must be 0..59 and frames
  0..74; timestamp fields use `mm:ss:ff` with at least two minute digits.
- Disc-relative LBA 0 is the first track's INDEX 01. An explicit first-track
  INDEX 00 or PREGAP can therefore occupy negative LBAs. No implicit two-second
  pregap or 150-sector offset is added. Native lead-in is retained separately
  and is outside the queryable LBA map.
- INDEX 00/01 describe boundaries in source audio. A stored pregap consumes
  source sectors. PREGAP and POSTGAP describe additional disc time with **no
  source audio position**. The model represents these regions without creating
  audio samples. Later source audio shifts in disc coordinates by their lengths.
- A positive PREGAP has a derived index 0 marked as originating from the gap
  directive. POSTGAP is attached to the preceding track, with no specified index.
  Explicit zero-duration gap declarations are retained, with no added region.
- Tracks and regions use half-open intervals `[begin, end)`. `queryLba()` returns
  a track, optional index, region kind, and optional source sector. A stored
  INDEX 00 region has kind SourceAudio; a declared gap has no source sector.
  Lead-out and positions outside the layout return no mapping. INDEX 00 and 01
  may coincide; both are preserved and lookup at that point selects INDEX 01.
- Text lead-out comes from the exact STREAMINFO audio end plus declared gaps.
  Native lead-out must be the final track 170 and match STREAMINFO exactly.
  Its provenance is retained. Unknown total sample count is insufficient for a
  complete layout and is rejected.

Initial supported scope and validation:

- CD-DA audio only: 44,100 Hz, stereo, 16-bit. No conversion or rounding occurs.
  Source length and all native sample offsets must be divisible by 588.
- Exactly one embedded FILE, of type WAVE or FLAC, describes the containing
  FLAC's source audio. Its filename is descriptive and need not match the
  current filename; it is never opened as an external file. Multi-file sheets,
  BINARY sources in embedded FLAC sheets, mixed-mode/data tracks, non-CD native CUESHEET, CDTEXTFILE,
  4CH/SCMS flags, and unknown commands are rejected rather than interpreted.
- Track numbers must increase in 1..99 (text track numbers must be consecutive).
  Each track requires INDEX 01. Index numbers start at 00 or 01 and then
  increment by one. Offsets must increase, except coincident INDEX 00/01.
  Tracks may not overlap, and every index must precede the source end.
- The first stored index must start at source sector zero. Unassigned source
  prefixes, missing indexes, partial final sectors, malformed ISRC/catalog,
  overflow, conflicting native lead-out, and invalid ordering are rejected.
  ISRC is validated as 12 uppercase alphanumeric characters; a populated CD
  catalog is 13 digits. Native lead-in must be sector-aligned and at least two
  seconds.
- PREGAP together with INDEX 00 on the same track is rejected as ambiguous for
  this initial implementation. No policy for combining those two kinds of
  pregap is assumed. Layout conflicts are reported without correction.

`--help` prints usage. Exit codes: 0 for success, 1 for read/inspection errors,
and 2 for incorrect argument count. A successful metadata inspection can still
print REJECTED layout or CONFLICT diagnostics and return 0; this CLI is an
inspector, not a mount/validation gate. Library factories throw
`std::invalid_argument` for rejected layouts, and `compareTrackLayouts()` returns
explicit conflict diagnostics. Native FLAC files are supported; Ogg FLAC
containers are outside the initial scope.

Metadata tests generate small metadata-only fixtures in the build directory,
with no encoded audio or external test dependencies. They cover STREAMINFO, native
CUESHEET fields and INDEX 00/01, embedded comments, absent CUE metadata, non-CD
audio parameters, unknown sample count, and malformed/missing input. The CLI
has basic CTest smoke tests. Descriptive metadata tests cover disc and track
scope, performer overrides, missing fields, malformed lines, and Vorbis tag
preservation. User audio files are never modified.

Layout tests cover multiple tracks/subindexes, INDEX 00/01 boundaries, negative
LBAs, declared and stored pregaps, postgaps, coincident indexes, lead-out,
missing indexes, malformed CUE input, ordering, alignment, integer overflow,
source mapping coverage, native/text agreement and conflicts, and integration
with metadata read by libFLAC. Local user audio is not required by CTest.

Format references used for the layout semantics:
[FLAC CUESHEET specification](https://www.rfc-editor.org/rfc/rfc9639.html#section-8.7)
and [GNU CUE command reference](https://www.gnu.org/software/ccd2cue/manual/ccd2cue.html#CUE-sheet-format).

The CDDA reader is exposed by `include/flaccd/CddaSectorReader.h`:

```cpp
flaccd::CddaSectorReader reader(path, layout);
flaccd::CddaSector sector = reader.readSector(discLba);
```

Pass an existing validated `TrackLayout` for that FLAC. The reader checks the
file's format and source length, but cannot detect an unrelated file with an
identical format and sample count. The caller must keep the file unchanged
while the reader is open. Instances are movable and noncopyable. Use external
locking if sharing an instance; independent readers have independent state.

Each successful read returns exactly 2,352 bytes: 588 interleaved left/right
stereo sample frames, signed 16-bit **little-endian** PCM. The implementation
packs bytes explicitly, independently of host endianness. No resampling,
bit-depth conversion, clipping, dithering, or PRE de-emphasis is applied.
Stored INDEX 00 audio is decoded unchanged, including explicit negative-LBA
first-track pregaps. Source-less PREGAP/POSTGAP regions raise an error; silence
is never synthesized. Reads of source audio after those gaps use the adjusted
layout mapping, rather than treating disc LBA as a source sample offset.

The decoder and its file handle use RAII. The reader retains one FLAC frame in
a fixed cache (at most 65,535 stereo sample frames, about 256 KiB), plus the
layout and libFLAC's own state. It copies partial cached frames into a sector,
continues decoding across frame boundaries, and seeks to exact source sample
positions for nonsequential reads. The entire audio stream is never cached.

Unsupported audio formats, unknown/unaligned lengths, and layout length
mismatches raise `std::invalid_argument` during construction. Reads outside the
layout, including lead-out, raise `std::out_of_range`. Source-less gaps raise
`std::invalid_argument` without damaging the reader. Decode, seek, frame-format,
CRC, discontinuity, and premature end-of-stream failures raise
`std::runtime_error` and make the reader unusable until reopened. No partial
sector is returned. Callback functions do not throw through libFLAC.
Whole-stream MD5 verification is disabled for random access; unread audio is
not integrity-checked. Decoded-frame errors are fatal, including CRC errors
that libFLAC might otherwise replace with silent samples.

## Export commands

```text
flaccd-export FILE native|text START-LBA SECTOR-COUNT OUTPUT.raw
flaccd-export FILE --all [--format raw|bin-cue] --output PATH [OPTIONS]
flaccd-export FILE --track N [--format raw] --output PATH [OPTIONS]
flaccd-export FILE --tracks FIRST-LAST [--format raw] --output PATH [OPTIONS]
flaccd-export FILE --split-tracks [--format raw] --output-dir DIR [OPTIONS]
flaccd-export FILE --cue-only --output OUTPUT.cue --bin-file NAME.bin [OPTIONS]

OPTIONS: --layout native|text|auto  --overwrite
Track modes also accept: --index01-only
```

All modes reuse the normalized layout and `CddaSectorReader`. RAW and BIN
contain headerless signed 16-bit little-endian stereo CDDA, 2,352 bytes per
sector. WAV output is not implemented. `--help` lists options. Exit codes are
0 for success/help, 1 for layout/read/write errors, and 2 for invalid CLI syntax.
Progress is printed to stderr in sectors, with a success message only after
complete files have been published.

The positional interface remains supported, including negative LBAs when the
layout maps them:

```sh
build/flaccd-export Zelda.flac native 0 75 first-second.raw
build/flaccd-export Zelda.flac text 36612 16 pregap-boundary.raw
```

Counts must be positive and the exclusive range end may equal lead-out.
Out-of-range sectors and source-less gaps in selected audio are rejected.
Positional exports retain their default no-overwrite behavior. Use the modern
interface for `--overwrite` and automatic track boundaries.

### Complete disc

```sh
build/flaccd-export Zelda.flac --all --format bin-cue --output Zelda
build/flaccd-export Zelda.flac --all --format raw --output Zelda.raw
```

BIN/CUE `--output` is a **basename**, so `Zelda` creates `Zelda.bin` and
`Zelda.cue`. Raw `--output` is the exact filename. Output parent directories
must already exist, except `--output-dir` for split tracks, which is created
if needed. Mount **the CUE**, rather than the BIN alone, in ImgDrive. Emulator interoperability still requires Windows VM testing.

Full-disc exports cover source sector zero through the exclusive source end.
This includes stored first-track pregap audio at negative disc LBAs, when
present. There is no implicit two-second lead-in audio or added silence.

### Individual tracks, ranges, and separate files

```sh
build/flaccd-export Zelda.flac --track 2 --format raw --output track02.raw
build/flaccd-export Zelda.flac --tracks 2-5 --format raw --output tracks02-05.raw
build/flaccd-export Zelda.flac --split-tracks --format raw --output-dir tracks
build/flaccd-export Zelda.flac --track 2 --index01-only --output track02-program.raw
build/flaccd-export Zelda.flac --tracks 2-5 --index01-only --output programs02-05.raw
build/flaccd-export Zelda.flac --split-tracks --index01-only --output-dir programs
```

Track numbers are actual CD track numbers, never zero-based positions. A track
slice starts at its stored INDEX 00, or INDEX 01 when INDEX 00 is absent. It
ends before the next track's INDEX 00 (or first stored index), with the last
track ending at exclusive lead-out. A following track's pregap never belongs
to the preceding track's output. Split files are named `Track01.raw`,
`Track02.raw`, etc., using the actual numbers.

`--index01-only` starts **each selected track** at INDEX 01. For a range it
concatenates those slices, omitting every selected track's stored pregap,
including pregaps between selected tracks. Default range exports include
all selected tracks' own pregaps. Missing/reversed track ranges are errors;
nonconsecutive native numbering is usable for individual tracks and splitting,
but a requested range containing a missing track number is rejected.

### Layout selection

`--layout auto` is the modern default. Every native and textual candidate is
validated. If none is valid, selection fails with validation diagnostics. If
valid candidates disagree, selection reports a conflict and writes nothing.
When valid candidates agree on shared structural fields, auto prefers native
geometry and lead-out provenance. It retains compatible textual descriptions,
ISRC/catalog values, and known DCP absent from the native representation;
conflicting descriptive values are also rejected. Malformed candidates are
not eligible when another valid candidate is available.

Use `--layout native` or `--layout text` to choose explicitly; that source must
contain exactly one CUESHEET. Explicit selection does not merge the other
source. `flaccd-info` remains an inspector that displays candidates and their
conflicts separately.

### External CUE and CUE-only output

```sh
build/flaccd-export Zelda.flac --cue-only --output Zelda.cue --bin-file Zelda.bin
build/flaccd-export Zelda.flac --cue-only --layout text --output "Zelda album.cue" --bin-file "Zelda album.bin"
```

CUE-only writes no audio and assumes the referenced BIN contains the **complete
disc starting at disc LBA 0**, with exactly the source length in sectors. The
reference must be a sibling filename, not a directory path. A missing BIN is
allowed; the caller is responsible for creating it correctly. An existing BIN
is checked for regular-file status and exact byte length, not audio contents.

A stored first-track pregap before LBA 0 conflicts with that assumption, so
CUE-only rejects such layouts instead of discarding audio or shifting indexes.
Use full BIN/CUE export for these discs; its BIN starts at the first stored
source sector and includes that negative-LBA audio.

Generated CUEs use `FILE "filename.bin" BINARY`, `TRACK nn AUDIO`, and all
available indexes, including 00/01 and higher subindexes. Exact integer
`MM:SS:FF` timestamps use **file-relative source-sector positions**, at 75
frames per second. No artificial 150-sector offset is added. Lead-out is
inferred from BIN length; native lead-in and lead-out provenance are not
additional audio or tracks. Every generated CUE is reparsed and structurally
compared with the selected layout before output creation.

Available titles, performers, ISRC, catalog, PRE, and DCP are preserved where
CUE can represent them. Missing disc descriptions fall back to ALBUM and
ALBUMARTIST (then ARTIST) Vorbis tags. Conflicting needed fallback values are
rejected; identical duplicates are accepted. Unknown native DCP is omitted
under explicit native selection. Zero-duration gap declarations are omitted.
UTF-8, spaces, ampersands, and brackets are retained in quoted filenames and
metadata. Quotes/control characters and Windows-invalid filename characters
are rejected instead of using a nonstandard escape convention. CUE output
requires consecutive track numbers and a `.cue` output extension.

Nonzero source-less PREGAP/POSTGAP regions in selected audio are errors;
sectors are never synthesized. Full-disc and CUE exports reject them anywhere
in the disc. Track exports can select source-backed audio elsewhere, and
`--index01-only` may omit a source-less pregap explicitly; a selected postgap
still causes rejection. Existing parser limitations, including combined
PREGAP plus stored INDEX 00, remain unchanged.

The existing dedicated utility is also available:

```sh
build/flaccd-bin-cue Zelda.flac text Zelda.cue
build/flaccd-bin-cue Zelda.flac auto Zelda.cue
```

It creates the same pair through the shared exporter and refuses existing
outputs. Unlike the modern `--output` basename, its last argument is the CUE
filename.

### Output safety and resource use

Existing outputs are refused unless `--overwrite` is explicitly requested.
Input audio and its hard-link aliases are protected even with that option;
output symlinks and nonregular files are refused. Replacement swaps the output
pathname rather than truncating an existing inode. On systems that cannot
replace an existing file through standard filesystem rename, replacement fails
and leaves the old file intact.

Every destination is preflighted before decoding large exports. Split exports
preflight all filenames together. Audio streams through a 75-sector buffer,
plus the reader's bounded FLAC frame cache; no album-sized PCM buffer is used.
Staged files live in uniquely created `.flaccd-export-*.part` directories
beside their destinations. Only closed, complete files receive final names.
Handled errors clean up staging; abrupt termination may leave clearly named
`.part` directories, which can be removed after the process exits. Failed
decoding leaves previous outputs unchanged, even with `--overwrite`.

Default no-overwrite publication uses atomic hard-link creation, protecting
against a file appearing after preflight. The output filesystem must support
hard links. Each file is published separately; multiple split files and a
BIN/CUE pair are **not one atomic transaction**. The BIN is published before
the CUE. A publication failure or interruption can leave a subset of complete
outputs; replacing an existing pair also has a brief window between the two
replacements. Wait for successful completion before mounting or consuming a
pair. This is not a power-loss durability guarantee.

Reusable interfaces are in `AudioExporter.h` and `BinCueExporter.h`.
`TrackLayout::fromBinaryCue(byteCount, parsedCue)` validates external single-file
BINARY layouts using the shared parser and normalization, without opening the
referenced BIN. Embedded FLAC `fromText()` validation remains unchanged.

## Export tests

CTest creates deterministic signed PCM and encodes small FLAC fixtures using
libFLAC, without requiring personal audio paths. Tests cover first/last sectors,
track boundaries, stored INDEX 00, negative LBAs, coincident indexes, declared
gaps, random reads, sector alignment, malformed metadata, and decoder errors.

Export tests cover positional compatibility, full-disc byte lengths and exact
PCM, individual tracks, ranges, split outputs, INDEX 01-only concatenation,
actual track numbering, auto/native/text agreement and conflicts, metadata,
CUE round trips and minute rollover, exclusive lead-out, invalid CLI options,
missing tracks, source-less gaps, existing-file protection, staged output during
simulated interruption, and failed-overwrite preservation. When the `flac`
command is available, both legacy and modern full-disc outputs are compared
against its independent full decode. No Windows APIs or personal paths are
required by automated tests.

## ImgDrive mounting backend (Windows)

The Windows `flaccd.exe` CLI now talks to a persistent, unprivileged per-user
`flaccd-agent.exe`. Keep both executables together. The first command starts the
agent without a console window; no Windows service, autostart registration,
installer, driver change, or administrator rights are required. The agent owns
mount operations, records and external-eject reconciliation. Existing portable
inspection, decoding and export tools remain independent.

```powershell
.\build-windows\flaccd.exe drives
.\build-windows\flaccd.exe mount "Y:\temp\music\Zelda.flac" --drive E:
.\build-windows\flaccd.exe mount "Y:\temp\music\Zelda.flac" --drive auto --no-copy
.\build-windows\flaccd.exe status
.\build-windows\flaccd.exe unmount E:
.\build-windows\flaccd.exe agent stop
```

Supply your actual FLAC path and actual ImgDrive drive letter. `--drive` may be
omitted for automatic selection of a visible, identified, empty ImgDrive device.
An explicitly requested physical, occupied, hidden or unidentified drive is
refused. FlacCD does not allocate or change drive letters. Keep ImgDrive's
**Hide Empty Drives disabled** for reliable selection/eject reconciliation.

ImgDrive's [published CLI](https://www.yubsoft.com/imgdrive/help.html) distinguishes
letters, numeric IDs and tags. FlacCD uses documented **letter selectors**
(`-m:E`), not an inferred numeric device index. Windows device numbers and SCSI
addresses are never treated as ImgDrive IDs. Enumeration joins CDROM interfaces
to drive letters with `IOCTL_STORAGE_GET_DEVICE_NUMBER`, persists the PnP device
identity, and checks for an `ImgDrive` driver service in its PnP ancestry.
Unrecognized service names/versions fail closed with enumeration diagnostics.
This native identification policy still requires qualification on the installed
ImgDrive build; the agent does not scrape its `-l` popup.

Before/after snapshots and matching audio-CD TOCs confirm the actual device.
A ready-but-different TOC is reported separately from absent media. If the
expected TOC appears on another formerly empty ImgDrive device, mount returns
an explicit drive-selection error, retains resources, and records that observed
binding for status/recovery. It never reports that as success on the requested
letter. `unmount` addresses a managed actual device and invokes **image-specific**
`-u <CUE>`; it never issues unmount-all or unconditional drive eject.

The CUE strategies remain:

1. Reuse an equivalent matching adjacent user CUE unchanged, comparing normalized
   layouts and validating its FILE target.
2. Exclusively create a matching adjacent CUE with `FILE "Zelda.flac" WAVE`.
3. On conflict, creation denial or concurrent use, stage matching filenames in
   the managed cache: hard link first, compressed-FLAC copy as a last resort.
   `--no-copy` prohibits copying. Copies are logged, space-checked where possible,
   streamed and byte-verified. No decoded BIN is generated.

Generated CUEs preserve supported metadata/indexes and exact file-relative
75-frame timestamps without a 150-sector shift. Source-less gaps and layouts
that cannot be represented accurately are rejected. Never edit/rename mounted
resources or the original source; a hard link shares the original file data.

All commands accept `--imgdrive PATH.exe`, `--cache-dir DIR`, and
`--timeout SECONDS` (1..600 per phase). Executable discovery checks explicit
configuration, `FLACCD_IMGDRIVE`, App Paths, and absolute PATH directories.
`--layout native|text|auto` and `--no-copy` apply to mount. Auto layout reports
conflicting embedded layouts. A running agent has one executable/cache
configuration; use `flaccd agent stop` before changing it. The command never
starts an absent agent and reports success only after confirmed exit (or when
not running). `--timeout SECONDS` bounds queueing/shutdown; the default is 30.
In-progress work may finish, but accepted shutdown rejects new work, preserves
active mounts/state and suspends eject monitoring. The next CLI command starts
a new agent and reconciles saved records, including discs ejected while stopped.
The legacy `agent-stop` spelling remains an alias.

The default cache is `%LOCALAPPDATA%\FlacCD\ImgDrive\v1`. Immutable ownership
intents, resource identities and phase/device records survive CLI/agent exit.
User CUEs and original FLACs are never deleted. Generated-file cleanup requires
unchanged identities and, for CUEs, exact contents. Modified/replaced files and
unknown cache contents are preserved with diagnostics. Cleanup messages name
what was actually removed or retained.

A hidden **top-level** window receives `WM_DEVICECHANGE`; the agent also rescans
every five seconds. Notifications request inspection, not immediate deletion.
Two empty-media observations of the same identified device, at least five
seconds apart, permit ownership-checked cleanup. Normal idle eject cleanup takes
about **5–10 seconds**; long operations can defer it. Missing/unknown devices,
unconfirmed mounts, legacy unbound records and interrupted preparations are
preserved for inspection. There is no automatic unsafe orphan pruning.

The local versioned named-pipe interface restricts access to the current user,
rejects remote clients, validates peer ownership, serializes requests, and returns
structured status rows alongside output/diagnostics. Its framing and portable
coordinator are usable by a future GUI. No request is automatically replayed
after submission if a connection fails. Logs rotate in `agent.log` and
`agent.previous.log` inside the cache; capture CLI errors with `2> mount.log`.

Windows 10/11 and an independently installed, working ImgDrive are prerequisites.
ImgDrive remains proprietary and is not redistributed. The agent does not provide
a custom sector server or establish full MMC/subchannel/CD-TEXT fidelity.
**Native mapping, IPC and eject notifications have not been tested against
ImgDrive in this Linux development session.** Follow
[WindowsAgentTesting.md](docs/WindowsAgentTesting.md) for Windows builds, evidence
collection, IPC tests, restart/eject checks and remaining limitations. Previous
investigation notes remain under `docs/historical/`.

### Windows Explorer integration

A per-user Windows x64 installer can now be built with
`cmake --build build-windows --target flaccd-installer` after configuring
`FLACCD_ISCC` to an Inno Setup 6.3+ compiler. It packages the smoke-tested staged
deployment, registers Explorer through the existing CLI, and stops the agent
gracefully for upgrades/uninstall while retaining mount resources and user state.
See [WindowsInstaller.md](docs/WindowsInstaller.md) for build commands, ImgDrive
prerequisites, maintenance behavior and Windows VM acceptance tests.

From the final deployed FlacCD folder, register the per-user FLAC context menu:

```text
flaccd.exe shell install
flaccd.exe shell status
flaccd.exe shell uninstall
```

Right-click one `.flac` file and select **Mount as Audio CD**. On Windows 11,
look under **Show more options**. The command invokes the existing
`flaccd.exe mount` path, including automatic ImgDrive selection and the background
agent. ImgDrive must already be installed/configured; normal mount restrictions
still apply. Registration itself neither requires ImgDrive nor starts the agent.

No administrator privileges are needed. Only the dedicated
`HKCU\Software\Classes\SystemFileAssociations\.flac\shell\FlacCD.MountAudioCD`
verb is managed; the default FLAC player/file association is unchanged.
The registered executable path is absolute, so keep the deployment folder in
place. Before moving/removing FlacCD, run `shell uninstall`; after moving it,
run `shell install` from the new location. Repeated install/removal is harmless.
Modified or foreign registrations are preserved and reported as conflicts.
`shell status` inspects registration, not ImgDrive readiness or mount state.

This is a console command, so Explorer can briefly open a console window. For
mount failures, reproduce with `flaccd.exe mount "path\album.flac"` in a terminal
and inspect the existing agent logs. See
[WindowsShellTesting.md](docs/WindowsShellTesting.md) for native registry,
filename quoting and Explorer integration checks. Windows integration has not
been executed in this Linux development session.
