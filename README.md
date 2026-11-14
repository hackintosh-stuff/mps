# MPS — Fusion-MPT SAS2 for macOS

Date: 2026-11-14. Driver version: 0.1.0.

MPS is an x86_64 IOKit kext for LSI/Avago MPI 2.0 SAS2 controllers. It adapts
the FreeBSD `mps` controller protocol to macOS, using the adjacent MPR project's
macOS scheduling and block-storage glue as a read-only reference. It has its
own `com.maz1.MPS` bundle, `MPSController`, `MPSBlockDev`, `MPSUserClient`,
boot arguments and management utility. MPR and MPS target different PCI IDs.

This port has **no MPS hardware qualification**. Host protocol checks and
cross-compilation do not prove native link, signature, load, DMA safety on the
test machine, or data integrity. MPR's historical SAS3008 results do not apply
to this source. Native build/load and all hardware procedures remain separate
verification steps. Current build evidence is delivered in `_scratch`.

## Hardware and protocol scope

The exact FreeBSD `mps_pci.c` table contains 17 unique vendor `0x1000` IDs:

| Family | Device IDs |
|---|---|
| SAS2004 / SAS2008 | `0070`, `0072` |
| SAS2108 | `0074`, `0076`, `0077` |
| SAS2116 | `0064`, `0065` |
| SAS2208 | `0080`, `0081`, `0082`, `0083`, `0084`, `0085` |
| SAS2308 | `0086`, `0087`, `006e` |
| SSS6200 WarpDrive | `007e` |

These are implementation matches, not a qualification list. Facts must report
MPI 2.0. There are no SAS3 FastPath, IEEE SGE, NVMe, atomic-post, GEN35 or SEA
paths. WarpDrive applies its Manufacturing Page 10 hide/expose policy and uses
ordinary IR-volume I/O; the optional DirectDrive optimization is not implemented.

The available survey describes one `1000:0087` / `1000:3020` SAS2308, revision 05,
firmware 20.00.07.00, eight 6 Gb/s PHYs, PCIe 3.0 x4 and one directly attached
SATA SSD. It provides no SAS SSP, expander, enclosure, IR array or multi-target
qualification. IT/IR runtime mode must be determined from Facts/configuration
pages. The survey is a Linux inventory, not an MPS test result.

All resident hardware DMA pools use the controller's retained per-device
IOMapper through IODMACommand. They require a single aligned device-visible
IOVA below 4 GiB and reject incompatible or bounced mappings without falling
back to host physical addresses. This source path includes translated mapper
support; it has no MPS IOMMU hardware qualification.

## Implemented source paths

- Bounded diagnostic reset, doorbell handshake, MPI2 Facts/Init, credit-bound
  queues, one reply ring, interrupt delivery and a bounded polling fallback.
- 12-byte MPI Simple64/Chain64 SGLs, DWORD chain offsets, per-command sense,
  direct client mappings and independently owned resident fallback buffers.
- SAS/SATA and IR-volume discovery, member filtering, multi-LUN probing,
  bounded controller-local path selection, events/ACK and raw event history.
- Per-LUN block nubs through `IOBlockStorageDevice` and the system's
  `IOBlockStorageDriver`: READ/WRITE(10)/(16), capacity, sense, cache flush,
  FUA policy and advertised-limit UNMAP.
- Timeout/TMF escalation and IOC recovery without replaying writes whose
  completion is uncertain; DMA ownership survives timeout until completion or
  a proved IOC stop.
- Immutable nub bindings checked at admission; media invalidation, removal,
  retirement, native power management and wake rebuild/revalidation.
- Root/boot-gated native IOUserClient management and `tools/mpsctl.py`, with
  rebuilt SAS2 DMA descriptors rather than caller-provided physical pointers.

These are source capabilities. Every runtime path remains unverified on MPS.
Block publication fails closed when a trustworthy disk identity cannot be
established; a model string, transient handle or synthetic SATA address cannot
authorize a writable medium. The survey does not yet supply the required serial.
Detailed implementation and honest gaps are in [DEBUG.md](DEBUG.md) and
[PLAN.md](PLAN.md). [PROTOCOL.md](PROTOCOL.md) lists the SAS2 conversion
boundaries. [TESTING.md](TESTING.md) is the hardware test project, with risk,
prerequisites, byte-level acceptance criteria and rollback.

## Build and host checks

On a currently running, configured x86_64 Mac with Command Line Tools:

```sh
python3 tools/validate-build-inputs.py
sh tools/build-noxcode.sh
```

The native script builds `build/MPS.kext`, checks kmod entry points and
driver-symbol closure, validates the plist and applies an ad-hoc signature.
It does not install, load, inject, modify an ESP or rebuild a kernel collection.
The Xcode project is `MPS.xcodeproj`; `MacKernelSDK` is a separate dependency.

Host-only SGL checks can run on a machine with a C++17 compiler:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tools/test_sas2.cpp -o /tmp/mps-sas2-protocol
/tmp/mps-sas2-protocol
python3 tools/test_management.py
```

The independent decoder checks actual MPI bytes; it does not emulate a
controller. Cross-compiled Mach-O objects remain distinct from a native linked,
signed and loadable kext. Do not reboot a rescue system just to obtain a compiler.

## Runtime options

Diagnostics are silent by default. `mps_debug=1` enables kernel messages and
diagnostic IORegistry properties. `mps_io_depth=N` requests 1–32 asynchronous
command windows, constrained further by Facts and available memory.
`mps_direct_dma=0` forces the resident fallback path for comparison.

`mps_mgmt=1` enables the administrator control interface. `mps_maintenance=1`
also enables management and suppresses block-media publication. Media, firmware
and array mutations require maintenance mode and the CLI's explicit mutation
acknowledgement. Fault-injection switches are for a separately authorized
recovery test only; they are never enabled by default.

```sh
python3 tools/mpsctl.py list
sudo python3 tools/mpsctl.py info --registry-id <registry-id>
sudo python3 tools/mpsctl.py targets --registry-id <registry-id>
sudo python3 tools/mpsctl.py events --registry-id <registry-id>
```

## Installation boundary

Installation/loading, OpenCore edits, reboot/power operations and test-disk
writes require explicit authorization for those operations. Installation uses
only the verified **internal SATA ESP**,
`EFI/OC/Kexts/MPS.kext` and a validated `Kernel > Add` entry; never
`/Library/Extensions`, another system volume or a USB EFI. Follow [TESTING.md](TESTING.md)
before deployment. Preserve the notices in [NOTICE](NOTICE) with binary
redistribution.
