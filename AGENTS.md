
# AGENTS.md — FLAC Audio CD Virtualization Project

This document defines the project goals, architecture, development environment, coding conventions, and instructions for Codex.

Codex should read this document before making changes to the repository.

## 1. Project Overview

Develop a Windows 10/11 utility that mounts a single FLAC file containing an embedded CUE sheet as a virtual audio CD.

The resulting virtual optical drive must behave like a real audio CD drive, allowing existing Windows CD applications to recognize the disc, enumerate tracks, and access CD-DA audio.

This is not simply an ISO mounting utility or a virtual filesystem.

### Project goals

- Support FLAC files containing embedded CUE sheets.
- Present a genuine virtual audio CD to Windows.
- Support accurate CD-DA sector reads.
- Preserve track boundaries, indexes, and pregaps.
- Prefer user-mode development whenever possible.
- Investigate existing virtual CD drivers before writing a custom driver.
- Keep the project portable and suitable for eventual open-source distribution.

The project is initially for personal use. Commercial distribution is not a goal.

Avoid introducing paid development infrastructure or production driver-signing requirements during initial development.

## 2. Development Environment

Primary development system:

- Operating system: CentOS Stream 9
- Compiler: GCC/G++ 11.5
- CMake: 3.31.8
- Git: 2.52
- libFLAC: 1.3.3
- Ninja: 1.10.2
- Editor: Vim
- AI coding assistant: Codex CLI

### Development principles

- Perform most development natively on Linux.
- Use C++20.
- Use CMake as the build system.
- Prefer Ninja for builds.
- Use Git for version control.
- Do not require VS Code or another IDE.
- Keep the workflow compatible with Vim and terminal-based tools.
- Cross-compile Windows user-mode components using MinGW-w64 when practical.
- Use a Windows 10/11 VM for integration testing.
- Use the Windows Driver Kit (WDK) inside Windows for kernel driver development if needed.

Do not upgrade existing development tools unless a specific technical requirement justifies doing so.

## 3. Architecture

Separate the project into three logical layers.

### 3.1 Core Library

The core library must be platform-independent.

Responsibilities include:

- FLAC metadata inspection.
- FLAC audio decoding.
- Native FLAC CUESHEET metadata parsing.
- Embedded textual CUE sheet parsing.
- Track layout representation.
- Track/index/pregap handling.
- CD-DA sector generation.
- Arbitrary sector access.

Do not introduce Windows-specific APIs into the core library.

### 3.2 User-Mode Service

Responsibilities may include:

- Loading and unloading disc images.
- Managing mounted-image state.
- Serving sector-read requests.
- Caching decoded audio.
- Handling requests from the virtual CD driver.
- Managing optional audio playback.

Keep the service separate from the core library.

### 3.3 Virtual CD Backend

Investigate existing drivers before implementing a custom driver.

Candidates:

- WinCDEmu
  https://github.com/sysprogs/WinCDEmu

- Arsenal Image Mounter
  https://github.com/ArsenalRecon/Arsenal-Image-Mounter

Questions to investigate:

1. Does the driver support a user-mode image provider?
2. Can the driver expose an optical CD-ROM device?
3. Does it implement the necessary SCSI/MMC commands?
4. Can the image provider be extended without modifying the signed kernel driver?
5. Does it support CD-DA sector access and audio-CD TOC information?

Do not assume either project satisfies these requirements without verifying its implementation.

If neither existing driver is suitable, investigate implementing a custom virtual CD driver.

Keep backend-specific code isolated so that alternative implementations can share the same core library.

## 4. Driver Development and Signing

Custom kernel driver development is a fallback.

For personal development:

- Use a dedicated Windows VM.
- Use a locally generated test-signing certificate.
- Enable Windows TESTSIGNING mode when required.
- Keep VM snapshots for recovery.
- Account for Secure Boot and Memory Integrity restrictions.

Windows Developer Mode is not equivalent to driver test-signing mode.

Do not assume test-signed drivers can be installed normally on other users' computers.

Production distribution may require Microsoft Hardware Developer Program enrollment, EV certificate-based registration, and Microsoft driver signing.

Do not incur those costs during initial development.

Do not modify Secure Boot, system security settings, or driver-signing configuration without explicit approval.

## 5. Audio CD Requirements

The target format is CD-DA.

CD-DA parameters:

- Sample rate: 44,100 Hz
- Channels: 2
- Sample format: signed 16-bit PCM
- Sectors per second: 75
- Stereo sample frames per sector: 588
- Bytes per sector: 2,352

Each CD-DA sector contains 588 stereo sample frames.

The core library must support retrieving arbitrary logical sectors.

Use integer sample positions and sector offsets to avoid floating-point rounding errors.

### FLAC decoding

Support or explicitly validate:

- Sample rate.
- Channel count.
- Bit depth.
- Total sample count.

Do not assume all input FLAC files already contain CD-compatible audio.

If conversion is required, define a clear policy for:

- Resampling.
- Channel conversion.
- Bit-depth conversion.
- Clipping.
- Dithering.

Do not silently convert audio without documenting the behavior.

### CUE metadata

Support both:

1. Native FLAC CUESHEET metadata blocks.
2. Textual CUE sheets embedded in Vorbis comments, commonly using a CUESHEET tag.

Preserve relevant information:

- Track numbers.
- Track types.
- INDEX 00 and INDEX 01.
- Pregaps and postgaps.
- Track offsets.
- ISRC identifiers.
- PRE and DCP flags.

Handle disc-relative and file-relative positions correctly.

Initially target audio-only discs. Mixed-mode discs are outside the initial scope.

## 6. Windows CD-ROM Compatibility

Investigate support for relevant SCSI/MMC commands:

- INQUIRY
- TEST UNIT READY
- REQUEST SENSE
- READ TOC/PMA/ATIP
- READ SUB-CHANNEL
- READ CD (0xBE)
- READ CD MSF (0xB9)
- PLAY AUDIO
- PLAY AUDIO MSF
- PAUSE/RESUME
- STOP PLAY/SCAN
- START STOP UNIT
- GET CONFIGURATION

Investigate Windows interfaces including:

- IOCTL_CDROM_READ_TOC_EX
- IOCTL_CDROM_RAW_READ

Do not consider the implementation complete merely because Windows displays a virtual drive.

Validate track enumeration, TOC information, sector reads, and compatibility with real CD-aware applications.

## 7. Coding Conventions

These conventions are mandatory unless explicitly changed by the developer.

### 7.1 Naming Conventions

Constants:

UPPER_SNAKE_CASE

Examples:

```cpp
constexpr std::uint32_t SAMPLES_PER_SECTOR = 588;
constexpr std::uint32_t BYTES_PER_SECTOR = 2352;
```

Variables and function parameters:

camelCase

Examples:

```cpp
std::uint32_t sampleRate;
std::uint64_t sectorOffset;
std::size_t sampleCount;
```

Functions and methods:

camelCase

Examples:

```cpp
readAudioSector();
parseCueSheet();
getTrackCount();
```

Classes, structs, enums, and type aliases:

PascalCase

Examples:

```cpp
class CueParser;
class FlacDecoder;
struct TrackLayout;
enum class TrackType;
```

Namespaces:

lowercase

Example:

```cpp
namespace flaccd {
}
```

Preprocessor macros:

UPPER_SNAKE_CASE

Prefer constexpr constants over macros when possible.

### 7.2 Filenames

Use PascalCase for source and header filenames.

When a file primarily implements a class, its filename must match the class name exactly.

Examples:

```text
CueParser.h
CueParser.cpp
FlacDecoder.h
FlacDecoder.cpp
TrackLayout.h
TrackLayout.cpp
CddaSectorReader.h
CddaSectorReader.cpp
```

Do not use snake_case filenames for class implementations.

For files that do not correspond to a class, prefer descriptive PascalCase names.

### 7.3 Indentation

Use exactly three spaces per indentation level.

Do not use tabs for indentation.

Example:

```cpp
class CueParser {
public:
   bool parseCueSheet(const std::string& cueText) {
      if (cueText.empty()) {
         return false;
      }

      return true;
   }
};
```

### 7.4 Brace Convention

Use attached opening braces.

The opening brace must be on the same line as the declaration or control statement.

Examples:

```cpp
void processTrack() {
   if (trackIsValid) {
      readTrack();
   } else {
      reportError();
   }
}
```

```cpp
for (std::size_t index = 0; index < trackCount; ++index) {
   processTrack(index);
}
```

```cpp
class TrackLayout {
public:
   void reset() {
      tracks.clear();
   }
};
```

Always use braces for if, else, for, while, and similar control statements, even for single-statement bodies.

### 7.5 Multiline Function Parameters

Function declarations, definitions, and calls should remain on one line when reasonably readable.

If the parameter list cannot reasonably fit on one line:

1. Place the first parameter immediately after the opening parenthesis.
2. Place each subsequent parameter on a new line.
3. Align subsequent parameters vertically with the first parameter.
4. Place the closing parenthesis on the same line as the final parameter.
5. Never place the opening or closing parenthesis on a line by itself.

Correct declaration:

```cpp
bool readAudioSector(std::uint32_t trackNumber,
                     std::uint64_t logicalBlockAddress,
                     std::span<std::byte> destination);
```

Correct function definition:

```cpp
bool readAudioSector(std::uint32_t trackNumber,
                     std::uint64_t logicalBlockAddress,
                     std::span<std::byte> destination) {
   if (trackNumber == 0) {
      return false;
   }

   return true;
}
```

Correct function call:

```cpp
bool success = readAudioSector(trackNumber,
                               logicalBlockAddress,
                               destination);
```

Incorrect:

```cpp
bool readAudioSector(
   std::uint32_t trackNumber,
   std::uint64_t logicalBlockAddress,
   std::span<std::byte> destination
);
```

Apply the same alignment convention to constructors and other function-like parameter lists where practical.

### 7.6 General C++ Rules

- Use C++20.
- Prefer RAII.
- Prefer standard-library types and containers.
- Prefer std::span for non-owning contiguous views.
- Prefer std::filesystem for paths.
- Prefer enum class over unscoped enums.
- Prefer constexpr over preprocessor macros.
- Avoid raw owning pointers.
- Avoid unnecessary global state.
- Use explicit-width integer types for file formats and sector addresses.
- Check for integer overflow in sample and sector calculations.
- Keep public interfaces small and well-defined.
- Avoid unnecessary third-party dependencies.
- Document non-obvious behavior and assumptions.

## 8. Build and Testing Rules

Use CMake as the source of truth for build configuration.

Support out-of-source builds.

Recommended native build commands:

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Add unit tests as features are implemented.

Important test cases include:

- FLAC metadata extraction.
- Native CUESHEET parsing.
- Embedded text CUE parsing.
- Track boundary handling.
- INDEX 00 and INDEX 01.
- Pregap handling.
- Sector alignment.
- Arbitrary LBA reads.
- End-of-disc behavior.
- Invalid or malformed metadata.
- Exact comparison against trusted CDDA sector data.

Do not claim tests passed unless they were actually executed.

Keep Linux-native tests independent of Windows-specific APIs.

## 9. Initial Development Milestones

### Milestone 1: Project foundation

- Initialize Git.
- Create the CMake project.
- Establish source and include directories.
- Add basic test infrastructure.
- Verify native Linux compilation.

### Milestone 2: FLAC metadata inspector

Create a command-line utility named:

flaccd-info

It should display:

- Sample rate.
- Channel count.
- Bit depth.
- Total sample count.
- Duration.
- Native FLAC CUESHEET metadata.
- Vorbis comments containing CUE text.
- Track numbers.
- Index numbers.
- Track offsets.
- Relevant flags.

The developer has two FLAC files containing embedded CUE sheets and can create more.

Use those files for local testing.

Do not require uploading audio files.

### Milestone 3: CUE parser

Implement a reusable parser and track-layout representation.

Validate track positions and indexes.

### Milestone 4: CDDA sector reader

Implement FLAC decoding and arbitrary 2,352-byte sector reads.

Test exact sector output against known-good references.

### Milestone 5: Virtual CD integration

Investigate WinCDEmu and Arsenal Image Mounter.

Determine whether an unmodified signed driver can use the custom user-mode image provider.

### Milestone 6: Custom driver if necessary

Only pursue kernel development if existing solutions cannot satisfy the requirements.

## 10. Instructions for Codex

Before making changes:

1. Read this AGENTS.md document.
2. Inspect the existing repository.
3. Identify the relevant components.
4. Explain significant architectural changes before implementing them.

During development:

- Follow the naming and formatting rules exactly.
- Keep changes focused and reviewable.
- Prefer incremental development.
- Add tests for new functionality.
- Do not introduce Windows dependencies into the core library.
- Do not require an IDE.
- Do not modify system configuration without permission.
- Do not run sudo commands without explicit approval.
- Do not overwrite user audio files.
- Do not add large dependencies without discussing them.
- Do not introduce kernel-mode code without approval.
- Do not claim functionality has been verified unless it has been tested.

When evaluating third-party drivers, distinguish confirmed behavior from assumptions.

When a requirement is unclear, ask for clarification instead of inventing behavior.

## 11. Current Project Status

The development system is CentOS Stream 9.

Verified tools:

- GCC/G++ 11.5
- CMake 3.31.8
- Git 2.52
- libFLAC 1.3.3
- Ninja 1.10.2

The developer uses Vim and intends to use Codex CLI.

Two FLAC files with embedded CUE sheets are available for testing.

The project implementation has not yet begun.

The immediate next step is to create the CMake project and implement the FLAC metadata inspection utility.

