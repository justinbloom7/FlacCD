# FlacCD: Windows Virtual Optical Backend Research

Research date: October 31, 2026.

**Neither repository provides a confirmed, usable CDDA backend through its existing unmodified driver interfaces.** Arsenal Image Mounter is closer architecturally because it supports user-mode image providers, but its optical emulation is implemented in the kernel and lacks the audio-CD operations FlacCD needs.

I reviewed these checkouts:

- WinCDEmu: `07e0924d51f6e7fdfb316143de810be085123c6d`
- Arsenal Image Mounter: `c5c1415f91dbca4bdd7413a29d46e0acf74907a2`

This was a source and package inspection on Linux. I did not install or test either driver on Windows, and did not modify any repository during the investigation.

FlacCD already supports normalized native/text CUE layouts and bit-exact 2,352-byte CDDA sector decoding from 44,100 Hz, 16-bit stereo FLAC files. The backend must expose a genuine Windows optical device, preserve audio track/index/pregap information, and serve CD-aware applications without requiring FlacCD to distribute a modified kernel driver.

Source links below identify the inspected revisions. BazisLib links refer to its upstream master branch, whose exact revision was not pinned by the WinCDEmu checkout.

## Comparison

| Requirement | WinCDEmu | Arsenal Image Mounter |
|---|---|---|
| Virtual optical device | Yes | Yes, alongside virtual disks |
| Custom user-mode data provider | No interface found | Yes: Devio |
| Sector-read interface | Kernel image parser, 2,048-byte sectors | Byte-offset/length requests to provider |
| Actual CDDA sector path | Not implemented in examined path | Not implemented in examined path |
| Multiple audio tracks | Unsupported | TOC fixed to one data track |
| INDEX 00/01 preservation | No suitable model/interface | No suitable model/interface |
| Arbitrary MMC commands forwarded to provider | No interface found | SCSI protocol definitions exist, but dispatch is unimplemented |
| Suitable without driver changes | No | No |

## WinCDEmu architecture and limitations

WinCDEmu uses a kernel bus driver, `BazisVirtualCDBus.sys`, which creates virtual SCSI optical-device children. `VirtualCDDevice` derives from BazisLib's `VirtualSCSICdrom`; the portable variant also uses the same bus implementation. See [VirtualCDDevice.h](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/BazisVirtualCDBus/VirtualCDDevice.h#L38) and [BazisVirtualCDBus.h](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/BazisVirtualCDBus/BazisVirtualCDBus.h#L17).

Its user-mode interface controls mounting and device management:

- `VirtualCDClient::ConnectDisk()` submits `VirtualCDMountParams`.
- `IOCTL_PLUGIN_DEVICE` and related IOCTLs mount, eject, enumerate, and configure drives.
- The parameters contain an image pathname, drive-letter preference, persistence, and MMC profile—not a callback, request queue, or provider endpoint.

References: [VirtualCDClient.cpp](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/VirtualCDCtl/VirtualCDClient.cpp#L91), [DeviceControl.h](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/BazisVirtualCDBus/DeviceControl.h#L1).

The read path is:

```text
Windows SCSI request
  → VirtualCDDevice / BazisLib command handling
  → VirtualCDDevice::Read()
  → AIParsedCDImage::ReadSectorsBoundsChecked()
  → kernel-side file access
```

`DoMountImage()` opens and parses the image inside the driver. `VirtualCDDevice::Read()` divides offsets and lengths by `CD_SECTOR_SIZE`, which is **2,048**. The `AIParsedCDImage` interface offers track counts/end sectors and data reads, but no audio-sector or index interface. See [DoMountImage()](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/BazisVirtualCDBus/BazisVirtualCDBus.cpp#L320), [Read()](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/BazisVirtualCDBus/VirtualCDDevice.cpp#L97), and [ImageFormats.h](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/ImageFormats/ImageFormats.h#L43).

The source explicitly documents data-track-only support and discards original track placement when rebuilding multitrack images. `MultiTrackCDImageBase` concatenates tracks and extracts 2,048 bytes per image sector. Its ability to read a file containing 2,352-byte records therefore does **not** establish CDDA support. References: [ImageFormats.h](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/ImageFormats/ImageFormats.h#L12), [MultiTrackImage.h](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/ImageFormats/MultiTrackImage.h#L42).

Two particularly concrete findings:

- The CUE parser expects a mode containing `/`, such as `MODE1/2352`; `TRACK 01 AUDIO` does not satisfy that path. [CUEParser.cpp](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/ImageFormats/CUEParser.cpp#L65)
- The NRG parser explicitly skips audio tracks. [NRGParser.cpp](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/ImageFormats/NRG/NRGParser.cpp#L189)

Consequently, exporting our FLAC audio to BIN/CUE would not solve the backend problem.

BazisLib is an external dependency absent from these checkouts. I also inspected its upstream optical and block-device implementations: TOC records are marked as data tracks, while the basic command dispatcher rejects unrecognized commands. These are supporting source findings, **not verification of the exact library version compiled into a released WinCDEmu binary**. [Upstream optical implementation](https://raw.githubusercontent.com/sysprogs/BazisLib/master/bzshlp/WinKernel/SCSI/VirtualSCSICdrom.cpp), [block-device dispatcher](https://raw.githubusercontent.com/sysprogs/BazisLib/master/bzshlp/WinKernel/SCSI/BasicBlockDevice.cpp).

## Arsenal architecture and provider interface

Arsenal uses the `phdskmnt.sys` Storport miniport, with legacy ScsiPort configurations also present. Its LUNs can represent disks or optical devices: `IMSCSI_DEVICE_TYPE_CD` selects an optical device, makes it removable/read-only, and defaults to 2,048-byte blocks. It is therefore **not disk-only**. References: [device flags](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Unmanaged%20Source/phdskmnt/inc/common.h#L61), [device initialization](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Unmanaged%20Source/phdskmnt/iodisp.cpp#L2152).

Its control interfaces include:

- Native `aimapi.dll`, particularly `ImScsiCreateDeviceEx()`.
- `IOCTL_SCSI_MINIPORT` with `SMP_IMSCSI_CREATE_DEVICE` and related requests.
- Managed `ScsiAdapter.CreateDevice()`.
- Devio provider services using shared memory, TCP/service-mediated connections, or the optional DevIoDrv transport.

References: [aimapi.cpp](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Unmanaged%20Source/aimapi/aimapi.cpp#L1308), [control codes](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Unmanaged%20Source/phdskmnt/inc/common.h#L563).

The proxy-backed read path is concrete:

```text
ScsiExecute()
  → ScsiOpReadWrite()
  → worker-thread ImScsiDispatchReadWrite()
  → ImScsiReadDevice()
  → ImScsiReadProxy()
  → IMDPROXY_REQ_READ { byte offset, byte length }
  → user-mode provider
```

References: [workerthread.cpp](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Unmanaged%20Source/phdskmnt/workerthread.cpp#L210), [ImScsiReadDevice()](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Unmanaged%20Source/phdskmnt/iodisp.cpp#L792), [ImScsiReadProxy()](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Unmanaged%20Source/phdskmnt/proxy.cpp#L692).

A custom provider can implement `IDevioProvider.Read()` and supply arbitrary bytes, potentially backed by FlacCD. `DevioShmService::ReadData()` directly invokes that interface. [IDevioProvider.cs](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Managed%20Source/Arsenal.ImageMounter/Devio/Server/GenericProviders/IDevioProvider.cs#L94), [DevioShmService.cs](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Managed%20Source/Arsenal.ImageMounter/Devio/Server/Services/DevioShmService.cs#L322).

However, that byte transport does not provide audio-CD semantics:

- The miniport stores sector size as `BlockPower` and calculates byte offsets with shifts. Its normalization cannot preserve a 2,352-byte block size. [iodisp.cpp](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Unmanaged%20Source/phdskmnt/iodisp.cpp#L2214)
- Unsupported SCSI commands fail in the kernel before reaching Devio.
- TOC responses are generated in the kernel, not supplied by the provider.

The header contains `IMDPROXY_FLAG_SUPPORTS_SCSI`, `IMDPROXY_REQ_SCSI`, and a CDB-carrying request structure. **Their presence is misleading for this use case:** searches found definitions but no implementation that forwards those SCSI requests. The shared-memory server's dispatcher likewise lacks a SCSI case. [imdproxy.h](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Unmanaged%20Source/phdskmnt/inc/imdproxy.h#L49), [server dispatcher](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Managed%20Source/Arsenal.ImageMounter/Devio/Server/Services/DevioShmService.cs#L259).

## MMC commands and disc layout

The following describes examined source paths, not Windows runtime tests:

| Command | WinCDEmu | Arsenal |
|---|---|---|
| INQUIRY / TEST UNIT READY | Implemented basic device handling | Implemented |
| READ CD / READ CD MSF | No implementation found | No implementation; unsupported-command path |
| READ TOC | Data-track TOC through BazisLib | Hardcoded one data track plus `0xAA` entry |
| READ SUB-CHANNEL | No implementation found | No implementation |
| PLAY AUDIO / PLAY AUDIO MSF | No implementation found | No implementation |
| PAUSE/RESUME / STOP PLAY | No implementation found | No implementation |
| GET CONFIGURATION | Implemented feature/profile reporting | Implemented; reports DVD-ROM profile |
| START STOP / eject | Device/media handling present | Implemented |

Arsenal's decisive command switch is [ScsiExecute()](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Unmanaged%20Source/phdskmnt/scsi.cpp#L182); unknown commands return an illegal-request condition. Its [ScsiOpReadTOC()](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Unmanaged%20Source/phdskmnt/scsi.cpp#L648) sets first/last track to 1 and marks the track as data. The `0xAA` address is calculated from capacity minus one, so it must not be adopted blindly as FlacCD's exclusive lead-out.

WinCDEmu even advertises CD-read feature bits including digital audio play, despite the missing audio command path. Feature advertisement alone is not evidence of working playback. [VirtualCDDevice.cpp](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/BazisVirtualCDBus/VirtualCDDevice.cpp#L534).

Neither backend exposes our complete audio track/index map. Basic TOC track-start records alone would also be insufficient to preserve INDEX 00/01 behavior: index reporting and pregap access need corresponding subchannel and read handling.

## Signing, compatibility, builds, and licensing

WinCDEmu's vendor lists Windows support through Windows 10, not Windows 11. Its checked-in INF dates to 2015, and the build instructions require BazisLib, STLPort-kernel, WDK 7.x, WTL, and Visual Studio 2010–2015. A release-signing command is an external build prerequisite; rebuilding does not preserve the vendor's signature. [Vendor requirements](https://wincdemu.sysprogs.org/download/), [build instructions](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/readme.md#L1).

Arsenal's vendor currently recommends Windows 10 1703+ and Windows 11. The checkout includes `DriverSetup/DriverFiles.zip`; its Win10 package INF identifies version `7.12.10.629`, dated October 26, 2026. I inspected certificate structures: the x64 driver contains an Arsenal signing chain, and the catalog contains a Microsoft Windows Hardware Compatibility Publisher certificate. **Certificate presence does not verify catalog membership, Windows trust, HVCI compatibility, or correspondence between that binary and this source revision.** Those need Windows verification. [Vendor compatibility statement](https://arsenalrecon.com/faqs).

Arsenal's build documentation supports Visual Studio 2017–2022 with WDK toolchains. Its managed build instructions mentioning .NET 6/7 are stale relative to the actual target list, which includes .NET 8/9/10. A native C++ Devio implementation could avoid depending on the managed stack. [Build instructions](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/How-to-build.md#L1), [current managed targets](https://github.com/ArsenalRecon/Arsenal-Image-Mounter/blob/c5c1415f91dbca4bdd7413a29d46e0acf74907a2/Managed%20Source/Directory.Build.props#L5).

Signing acceptance must be checked on the intended patched Windows installation. Microsoft's current policy restricts legacy cross-signed drivers on systems where the newer policy is enforced; an old “digitally signed” claim is insufficient. [Microsoft driver policy](https://support.microsoft.com/en-us/windows/hardware/drivers/the-windows-driver-policy).

Licensing differs materially:

- **WinCDEmu:** LGPL v3; preserve applicable notices and source/relinking obligations when incorporating or distributing it. [License](https://github.com/sysprogs/WinCDEmu/blob/07e0924d51f6e7fdfb316143de810be085123c6d/LICENSE#L1).
- **Arsenal:** project source/APIs are offered under AGPL v3-compatible terms or alternative licensing. Its free end-user mode does not make the developer API permissively licensed. Some protocol headers have separate permissive notices, but that does not relicense the whole project. [Vendor licensing statement](https://github.com/ArsenalRecon/Arsenal-Image-Mounter#more-details-on-licensing-and-contributions).

## Recommended next milestone

Treat **reuse of either unmodified driver as a failed feasibility gate for CDDA**, rather than beginning a provider implementation against an interface that cannot express the required commands.

The next milestone should be a **Windows backend qualification harness and final backend selection**:

1. Probe candidate signed backends using SCSI pass-through, `IOCTL_CDROM_READ_TOC_EX`, and `IOCTL_CDROM_RAW_READ`.
2. Require multiple audio-track enumeration, correct lead-out, preserved pregap reads, and exact sector comparisons against FlacCD.
3. Check actual driver signatures, versions, and operation under the VM's existing security configuration.
4. Qualify a backend whose shipped interface forwards arbitrary CDBs, transfer buffers, and status/sense information to user mode. Neither examined interface currently does so.

If modifying an existing kernel driver later becomes acceptable, **Arsenal is the more practical starting point**: retain FlacCD decoding and MMC emulation in user mode, and add a properly implemented optical-command forwarding path to the miniport/Devio interface. That would still create a modified driver binary requiring its own signing path—it would not satisfy the original “reuse an existing signed binary” constraint. Personal test-signing work should remain a separately approved Windows-VM step.
