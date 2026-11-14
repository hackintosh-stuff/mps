# PLAN — SAS2 macOS port

Date: 2026-11-14. Driver version: 0.1.0.

The project implements an independent MPI2 SAS2 transport and macOS block
storage adaptation. There is no MPS runtime evidence. Source, host protocol,
cross-object, native link/sign, load and hardware results are separate states;
only results actually recorded in `_scratch` may be reported as executed.

## Architecture and source authority

FreeBSD sources are in `reference/freebsd/sys/dev/mps`; Linux behavior reference
is `reference/linux/drivers/scsi/mpt3sas`. Both paths are inside this project.
The source survey records their acquisition identity. FreeBSD MPI headers are
the wire authority, and upstream notices remain preserved. Linux implementation
is not copied. MPR is read-only and contributes macOS glue patterns; its SAS3
protocol semantics and historical measurements are not accepted as MPS evidence.

The adopted architecture is `IOService` on `IOPCIDevice`, one block nub per
eligible logical unit, and the system `IOBlockStorageDriver` to create IOMedia.
It implements SCSI CDBs internally and through native management; it is not an
`IOSCSIParallelInterfaceController` transport and does not create native macOS
tape/optical device classes. A separate SPI adaptation would need a new ABI,
link and hardware qualification project.

## Delivery and evidence gates

| Stage | Required evidence | Current boundary |
|---|---|---|
| M0 source | Independent bundle/class/IDs, license inventory, upstream mapping | Source implementation and review |
| M1 protocol | Facts/Init/SGL/chains/management layouts and rejection tests | Host-only bytes and boundaries; no IOC execution |
| M2 build | All translation units compile; own symbols resolve; native kext link/sign | Cross objects are not a native link/load result; see actual build logs |
| M3 attach/read | Verified running version, Facts, target identity, geometry, exact bytes and digests | Not executed |
| M4 recovery/lifecycle | Timeout/TMF/reset, stale binding rejection, resource drain, replug and stop | Host binding policy can be checked; all kernel/hardware paths unverified |
| M5 write/PM | Bounded authorized write/readback/flush/UNMAP, sleep/wake/shutdown | Not executed |
| M6 expanded hardware | SSP, 4Kn, large capacity, EEDP/TLR, IR/WarpDrive, expander, multipath | Additional fixtures and explicit authorization required |

Hardware execution follows [TESTING.md](TESTING.md), beginning with an independent
read baseline **before the first MPS load**. The current survey has no reference
hashes. Completion of one stage never turns later stages into implied passes.

## Deliberate limits and remaining engineering

- Resident pools use one retained per-device mapper and checked 32-bit single-
  segment IOVAs. Queues/sense/config/staging/control/diagnostic mappings reject
  copy/bounce descriptors or unrepresentable mappings without physical fallback.
  Identity and translated-mapper execution, coherence, failure and ownership
  paths still need hardware qualification; source implementation alone proves
  neither the machine's IOMMU policy nor successful storage DMA.
- One hardware reply ring and at most 32 asynchronous windows are implemented.
  Hardware multi-queue/MSI-X scaling is not implemented. Linux's 12 vectors and
  approximately 10k queue credits are inventory values, not requirements or
  performance thresholds for this port.
- WarpDrive's optional direct-to-member stripe optimization is disabled. Normal
  IR volume I/O is used. Page10 hide policy remains mandatory correctness behavior.
- Native FreeBSD binary ioctl/CAM ABI, firmware DPM target-number persistence,
  cross-controller MPIO/ALUA, load balancing, SCSI target mode, boot-disk support,
  Apple Silicon/DriverKit and a signed release are not implemented.
- Native non-block OS device classes are not implemented; management-only
  endpoints use bounded administrative CDB/passthrough operations.
- No generic bidirectional SCSI user DMA, persistent user DMA registration,
  arbitrary operational-register write or vendor array-management GUI is supplied.
- The current test fixture cannot qualify all PCI matches or additional hardware
  features. Firmware and RAID mutations are implemented protocol mechanisms,
  not hardware-qualified maintenance tools; do not use them to validate ordinary
  I/O or as an implicit recovery step.

The source-to-platform map and locking/DMA details are in [DEBUG.md](DEBUG.md).
Future changes must preserve source-only and hardware evidence boundaries.
