# FlacCD: Alternative Windows Virtual Optical Backends

Research date: October 31, 2026.

**Test ImgDrive and Alcohol first.** Both have vendor evidence supporting audio-CD image emulation and CUE-based images. Neither is yet a confirmed solution for exact INDEX 00 handling or the complete MMC command set.

The most practical route appears to be **FlacCD-generated BIN/CUE files mounted by an independently installed, unmodified emulator**. No documented interface was found among these candidates that would let FlacCD directly supply sector-read callbacks.

This investigation follows `AGENTS.md` and `VirtualOpticalBackendResearch.md`. It used vendor documentation; no Windows installation, driver-signature verification, or runtime MMC testing was performed. No project or external repository source files were changed during the investigation.

## Ranked shortlist

“Documented” below means a vendor statement, rather than source-code or runtime confirmation.

| Rank | Candidate | Windows optical device and audio evidence | BIN/CUE from FlacCD | Windows 10/11 | Main uncertainty |
|---|---|---|---|---|---|
| **1** | **ImgDrive** | Explicit virtual optical drive and audio-CD mounting support | CUE referencing BIN is documented | Both documented | Exact index semantics; current driver signing |
| **2** | **Alcohol 120% / 52%** | Virtual CD/DVD-ROM devices; historical manual explicitly describes audio-CD playback | CUE/BIN documented | Current 120% explicitly lists both | Current MMC behavior and pregap fidelity |
| **3** | **DAEMON Tools Lite** | Optical-device emulation and audio-image features documented | CUE/BIN formats documented | Both documented | Index fidelity, device-mode restrictions, recent security incident |
| **4** | **gBurner Virtual Drive** | Vendor explicitly describes virtual CD/DVD-ROM music playback | BIN/CUE documented | Current suite supports both; standalone package needs confirmation | Thin technical documentation |
| Reserve | **Virtual CloneDrive** | Vendor explicitly claims physical-drive behavior and audio images | BIN supported; CUE is not listed | Both documented | No confirmed direct multitrack BIN/CUE route |

These are candidates for qualification, **not five verified implementations of all FlacCD requirements**.

## 1. ImgDrive — strongest first experiment

The vendor explicitly documents mounting CUE+WAV, CUE+APE, and CUE+FLAC as audio CDs, lists BIN as a CUE source, and provides command-line mounting. This is substantially stronger evidence than a generic “supports ISO/BIN” claim. [Product documentation](https://www.yubsoft.com/imgdrive/)

Its changelog records added `PREGAP` support and a correction to `READ TOC` responses. However, neither establishes faithful **stored INDEX 00 audio**, INDEX 01 transitions, or working `READ SUB-CHANNEL`. `READ CD`, `READ CD MSF`, and playback-command coverage remain unverified. [Changelog](https://www.yubsoft.com/imgdrive/changelog.html)

A significant qualification issue: version 2.2.4 says binaries will no longer be signed. The wording does **not** clarify whether this includes the kernel driver or only application binaries. Inspect the installed `.sys` and catalog before accepting this candidate; do not assume the existing driver satisfies our signing requirement. [Signing-related changelog entry](https://www.yubsoft.com/imgdrive/changelog.html)

Mounting automation was found, but no documented sector-provider SDK or image-format plugin ABI. Personal use is free; commercial use requires Pro. The FAQ does not establish permission to redistribute its driver separately. [Licensing FAQ](https://yubsoft.com/faq.html)

An additional useful experiment is **external CUE+FLAC mounting**. That could avoid a temporary BIN, but does not prove that ImgDrive understands embedded native/textual CUE metadata directly.

## 2. Alcohol — best independent comparison

Alcohol documents virtual CD/DVD-ROM devices and CDRWIN CUE/BIN support. Its older manual explicitly describes routing audio-CD playback from virtual drives through DirectSound. That confirms historical audio-emulation intent, but not present-day opcode correctness. [Supported formats](https://support.alcohol-soft.com/en/knowledgebase.php?postid=14999&title=Image+FormatsSupported+by+Alcohol), [manual, pages 28–29](https://support.alcohol-soft.com/documentation/alcohol-user-manual.pdf)

The current download page explicitly supports Windows 10/11 for Alcohol 120%, including its personal-use Free Edition. It states that 120% includes 52%’s emulation functionality. Test the current **120% Free Edition**, avoiding uncertainty from older 52% compatibility descriptions. [Current downloads and licensing](https://trial.alcohol-soft.com/downloadtrial.php)

Alcohol provides documented mounting automation through:

```text
AxCmd.exe V: /M:"C:\Images\Test.cue"
```

This interface mounts image files; it does not provide FlacCD with sector callbacks. [Command-line interface](https://support.alcohol-soft.com/en/knowledgebase.php?postid=15002&title=Alcohol+Command+Line+Usage)

Exact INDEX 00/01 handling, lead-out, `READ CD`, and `READ SUB-CHANNEL` remain unverified. Historical subchannel-emulation features are not proof of those commands’ current audio-disc behavior. No public provider SDK or separate driver redistribution permission was established.

## 3. DAEMON Tools Lite — technically plausible fallback

DAEMON Tools documents optical-device emulation, audio-image features, CUE/BIN formats, and Windows 10/11 compatibility. Its CLI can create and mount optical devices. The documentation also warns that SCSI/IDE functionality is limited on Windows 10; those modes should not automatically be preferred over its ordinary DT device. [Product](https://www.daemon-tools.cc/products/dtLite), [formats](https://www.daemon-help.com/additionally/formats/), [CLI and device-mode warning](https://www.daemon-help.com/additionally/command/)

No current public sector-provider or image-plugin SDK was found. Exact index preservation and the requested MMC commands still require testing.

There is also a concrete reason to rank it below the first two: the vendor acknowledged compromised Lite installers in 2026 and says it rebuilt version 12.6, identifying 12.6.0.2445 as corrected in its notice. Any experiment should use a currently verified official release, not an archived affected installer. [Vendor security notice](https://blog.daemon-tools.cc/post/security-incident)

Lite offers a free, advertising-supported edition and paid options. That does not establish a right to bundle or extract its proprietary driver.

## 4. gBurner Virtual Drive — reserve candidate

The standalone product explicitly advertises virtual CD/DVD-ROM music playback and BIN/CUE support. It offers less technical evidence than the first three: INDEX behavior, MMC coverage, driver signatures, and provider extensibility are undocumented in the material examined. [Standalone documentation](https://gburner.com/online-help/virtual-drive.htm)

Keep the standalone free Virtual Drive separate from the current gBurner suite: the suite lists Windows 10/11, but reserves virtual mounting for Pro. Those statements should not be combined into a blanket claim that every current free package provides mounting. [Suite comparison](https://www.gburner.com/download.htm)

## Virtual CloneDrive — not a direct BIN/CUE recommendation

Its vendor explicitly supports audio-containing images and Windows 10/11. However, the listed formats include BIN and CCD, **not CUE**. A BIN alone does not carry the multitrack layout needed here. A CCD/IMG/SUB export might provide another route, but that would require additional format work and qualification. It is freeware; separate driver integration rights were not established. [Vendor documentation](https://www.elby.ch/en/products/vcd.html)

## What remains unconfirmed

No publicly inspectable driver implementation was established for these candidates. Therefore, none receives a source-confirmed pass for:

- Stored INDEX 00 and exact INDEX 01 transitions.
- `READ CD` / `READ CD MSF` returning correct 2,352-byte audio sectors.
- `READ SUB-CHANNEL` reporting track, index, and relative position correctly.
- `PLAY AUDIO`, pause/resume, stop, and playback status.
- Complete TOC, lead-out, flags, and ISRC behavior.

ImgDrive alone has explicit vendor evidence naming `READ TOC` and `PREGAP`. General audio-image support is insufficient evidence for the remaining commands.

Likewise, **using an installed emulator is different from reusing its driver as a standalone backend**. None examined provides a confirmed public interface for arbitrary FlacCD sector delivery. No separate driver redistribution grant was established. Actual signature acceptance, Secure Boot, and Memory Integrity compatibility remain VM checks for every candidate.

## Recommended next milestone

Qualify **ImgDrive and Alcohol 120% Free**, separately, in the Windows VM:

1. Verify the shipped driver/catalog and operation under the VM’s existing security settings.
2. Mount a three-track BIN/CUE containing recognizable, non-silent stored pregap audio.
3. Check TOC track starts, audio flags, and exclusive lead-out.
4. Compare raw reads against FlacCD at every INDEX 00/01 boundary, including one sector before and after.
5. Probe `READ SUB-CHANNEL`, `READ CD MSF`, playback commands, and ordinary device/media commands; record returned data and sense errors.
6. Repeat with a first-track pregap and separately with source-less `PREGAP`/`POSTGAP` directives.

The proposed integration is:

```text
FLAC + normalized layout
    → cached BIN + external CUE
    → stock emulator mounted through its CLI
    → Windows CD applications
```

The exporter must preserve stored pregap audio and use file-relative CUE timestamps. Source-less gaps need an explicit policy rather than silently invented audio.

**Choose a backend only after those tests pass.** If either candidate succeeds, FlacCD can proceed without developing a kernel driver, at the cost of temporary image storage and an independently installed proprietary dependency.
