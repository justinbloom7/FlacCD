# FlacCD

Initial platform-independent C++20 foundation for a future FLAC audio CD
virtualization utility, with a Windows-only ImgDrive mounting adapter.
`flaccd-info` reads native FLAC metadata using libFLAC;
it does not decode audio or mount a drive. `flaccd-export` uses the core CDDA
reader for full-disc, track, range, split-track, and BIN/CUE exports. No custom
Windows driver is built or installed.

Build dependencies: a C++20 compiler, CMake 3.20 or newer, Ninja, pkg-config,
and libFLAC development headers/library (1.3.3 or newer).

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/flaccd-info /path/to/disc.flac
```

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

ImgDrive is the preferred mounting backend. The Windows-only `flaccd.exe`
mounts a small external CUE beside a FLAC with the **same basename**. Manual
Windows testing found that the installed ImgDrive looks for `Zelda.flac` beside
`Zelda.cue`, even when FILE names an absolute path elsewhere. This compatibility
rule is isolated in the ImgDrive backend; portable export tools remain independent.
No raw BIN is generated, no original FLAC is modified, and no driver is installed.

The backend selects these strategies in order:

1. Reuse adjacent `Zelda.cue` if its normalized layout matches the embedded
   layout and its WAVE FILE reference identifies the original `Zelda.flac`.
   Whitespace, comments, UTF-8 BOM, and formatting may differ. This user-owned
   CUE is never modified or deleted. Conflicts are reported and preserved.
2. If absent, exclusively create `Zelda.cue` beside the original FLAC, using
   `FILE "Zelda.flac" WAVE`. An actual creation failure triggers staging;
   permission bits are not used to predict writability. A racing file creator
   cannot be overwritten: the newly appeared CUE is checked for reuse instead.
3. If an adjacent CUE conflicts, is already in use, or cannot be created, stage
   matching `Zelda.cue`/`Zelda.flac` in a unique managed cache directory. First
   try a hard link (no extra audio storage, no symlink privileges). Expected
   filesystem/volume restrictions trigger a compressed-FLAC copy. Copies are
   logged with both paths and byte count, space-checked where supported, streamed,
   and byte-verified before mounting. **`--no-copy` forbids this last resort.**

Prerequisites: Windows 10/11, a working installed ImgDrive, and an existing
empty visible ImgDrive virtual drive. ImgDrive's vendor documents these Windows
versions and CUE+FLAC mounting; FlacCD's new wrapper still needs Windows
qualification. Install/use ImgDrive under its own proprietary licensing terms;
FlacCD does not redistribute its executable/driver or grant integration or
redistribution rights. See the [vendor product page](https://www.yubsoft.com/imgdrive/)
and [licensing FAQ](https://yubsoft.com/faq.html). Alcohol is not the preferred
backend.

```powershell
.\build\flaccd.exe drives
.\build\flaccd.exe mount "Y:\temp\music\Zelda.flac" --drive F:
.\build\flaccd.exe mount "Y:\temp\music\Zelda.flac" --drive F: --layout text
.\build\flaccd.exe mount "Y:\temp\music\Zelda.flac" --drive F: --no-copy
.\build\flaccd.exe status
.\build\flaccd.exe unmount F:
```

Use the actual ImgDrive drive letter, not an arbitrary free drive letter.
Default layout is `auto`, using the existing validation/conflict policy.
Automatic drive selection is disabled until reliable enumeration is verified:
ImgDrive documents `-l`, but the user's installed build shows its drive ID and
letter in a popup, without a machine-readable console result. `drives` invokes
that vendor listing; close the popup if needed. `mount FILE.flac` without
`--drive` returns a clear explanation rather than assuming drive 1 is empty.

Executable discovery checks `--imgdrive`, `FLACCD_IMGDRIVE`, registered Windows
App Paths (per-user/machine and 32/64-bit views), then absolute PATH directories.
No installation directory is guessed and the current directory is not searched
automatically. An explicitly configured bad path is an error, not a fallback.

```powershell
.\build\flaccd.exe mount "Y:\temp\music\Zelda.flac" --drive F: --imgdrive "C:\Actual\ImgDrive.exe"
$env:FLACCD_IMGDRIVE = 'C:\Actual\ImgDrive.exe'
.\build\flaccd.exe unmount F: --timeout 60
```

All commands accept `--imgdrive PATH`, `--cache-dir DIR`, and
`--timeout SECONDS` (1..600, default 30 per phase). `--layout` and `--drive` apply
only to mount, as does `--no-copy`. Paths above are examples: always supply your
actual FLAC and executable paths. `--help` prints syntax. Exit codes are 0 for success, 1 for
runtime/process/state failures, and 2 for invalid arguments, configuration, or layout selection.
The executable must be available for commands, including status. Errors include
Windows/process status and captured diagnostics where available; stderr prints
executable/cache paths. For troubleshooting, redirect stderr, for example
`flaccd mount ... 2> mount.log`. No background logging service is installed.

The cache defaults to `%LOCALAPPDATA%\FlacCD\ImgDrive\v1`. A unique mount
entry records the source, chosen CUE, drive, strategy, file identities, expected
CUE contents, and phase. An immutable intent is persisted before files are
created; completed identities are recorded before any ImgDrive invocation.
Generated CUEs use relative UTF-8 FILE filenames, while ImgDrive receives the
absolute CUE path. References and normalized layouts are reparsed and validated
before invocation. Spaces and Unicode are retained; unsafe filenames are rejected.
Supported titles, performers, ISRC, PRE/DCP, and stored indexes use the shared
CUE writer, exact 75-frame timestamps, and no artificial 150-sector shift.
Nonzero source-less gaps and unrepresentable layouts are rejected. Keep source
and mounted resources unchanged and readable until unmount, including hard-linked
audio: a hard link is another name for the original data, not a snapshot.

The CUE and state survive CLI exit. The real adapter uses CreateProcessW with
literal quoted UTF-16 arguments, without a shell; captures bounded diagnostics;
and waits for optical readiness plus matching TOC track starts/flags/lead-out.
A per-cache Windows named mutex serializes FlacCD commands and recovers after
process death. There is no ImgDrive provider SDK or custom sector-serving
service in this implementation.

Unmount uses the **recorded CUE pathname**, not an unconditional drive eject
or unmount-all command. Only after process success and empty/missing-drive
confirmation does it remove owned resources. Reused CUEs and originals are
never removed. Adjacent CUE cleanup requires the saved file identity and exact
contents; modified or replaced files are preserved with a diagnostic. Staged
resources receive the same identity checks. Cleanup enumerates known files only,
never recursively removes a user-supplied directory, and refuses unexpected files.

A shared cache lock serializes concurrent commands. Another mount of the same
FLAC uses a separate staged pair instead of sharing a managed CUE, so unmounting
one cannot remove another's resources. Use the same cache directory for related
commands. Do not manipulate the same image/drive concurrently through ImgDrive
or another application: its CLI provides no cross-application image-identity lock.

Failures/timeouts preserve resources for inspection/retry; process exit alone
never triggers cleanup. `status` separates recorded phase from current media
presence. A fresh CLI can recover completed records after a crash/restart and
perform image-specific unmount; old cache-only records remain readable. An
interrupted preparation or partial copy remains `preparing`, was never passed
to ImgDrive, and requires manual ownership inspection before removal. Corrupt
records, altered resources, and unknown entries fail closed; no automatic orphan
pruning is attempted. This is not a power-loss durability guarantee.

The native Windows adapter/CLI are CMake targets only on Windows. The separate
`flaccd-mount` coordinator and process interface are portable for Linux mock
tests; the core has no Windows API dependency. FLAC metadata inspection now uses
standard path-aware stream callbacks so Windows filenames are not narrowed
through an ANSI code page. CDDA decoding and fallback exports retain their
existing interfaces.

Linux tests cover FLAC-backed CUE round trips, Unicode/spaces, supported metadata,
process argument plans, missing/invalid executables, failures/timeouts, persistent
state across backend instances, stale/pending recovery, and protection against
unrelated/altered files. **They do not establish Windows runtime compatibility.**
The Windows build adds real process-adapter helper tests, requiring no ImgDrive.
See [WindowsImgDriveTesting.md](docs/historical/WindowsImgDriveTesting.md) for build prerequisites,
the Windows 10 Zelda checklist, remount/cleanup/no-BIN/hash checks, failure
recovery, strategy/copy/Unicode qualification, and remaining limitations.
