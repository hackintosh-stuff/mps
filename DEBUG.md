# DEBUG — MPS implementation and diagnostics

Date: 2026-11-14. Driver version: 0.1.0.

This describes source behavior, not hardware qualification. All MPS native
link/load, controller DMA and media operations require separate evidence.
The wire conversion checklist is [PROTOCOL.md](PROTOCOL.md), and executable
hardware acceptance procedures are in [TESTING.md](TESTING.md).

## Protocol and ownership

`MPSController` owns the PCI function, MMIO map, doorbell, IOC state, request
frames, sense, reply frames and free/post queues. Facts accept MPI 2.0, consume
a 0x40-byte reply and treat offset 0x26 as reserved. IOC Init sends header
0x1200 and leaves its offset 0x16 reserved. Missing EVENT_REPLAY on initial
attach triggers diagnostic reset and renewed Facts before discovery. Waits and
retries are bounded; a timeout is not permission to recycle firmware-owned DMA.

SAS2 uses 12-byte Simple64/Chain64 elements. Simple flags occupy byte 3 and the
24-bit length occupies bytes 0–2; the address begins at byte 4. Direction is
HOST_TO_IOC for writes. LAST marks the last data element before a chain; the
final data element additionally has END_OF_BUFFER and END_OF_LIST. Chain length
is 16 bits, NextChainOffset and request ChainOffset count DWORDs, and the chain
frame size is the IOC request-frame size. `MPSDmaSgl.h` validates geometry,
exact byte totals, address ranges, segment lengths, chain depth/storage and
one-byte offsets before touching the DMA output. Unrepresentable direct
mappings fall back before submission to a private resident staging window.

SGLs are built for device-visible addresses. `MPSResidentDma.h` maps free/post
queues, reply/request/sense, temporary and resident config pages, synchronous
staging, growing control buffers and retained diagnostic buffers uniformly.
`mpsMapResidentDma` uses the retained controller `fDmaMapper`, 32-bit address
limits and one full-length page-aligned IOVA. Low32 contiguous/page-aligned host
allocation is checked only as an allocation invariant: only the mapped IOVA is
submitted to firmware. No mapping failure falls back to a physical address.

Shared rings cannot use a deferred copy/bounce buffer while firmware and CPU
access them concurrently. Mapping therefore requires the DMA descriptor to be
the original memory descriptor; the allocation/range/alignment constraints also
exclude per-page address/alignment bounce. Unsupported mappings fail before
post. Client DMA and resident DMA use the same device mapper. These are source
checks, not evidence that a particular active IOMMU mapping was exercised.

Each `MPSResidentDma` retains its IODMACommand, descriptor preparation, byte count
and IOVA for the original descriptor's lifetime. `mpsReleaseResidentDma` is used
only for an unposted candidate, a completed/released dynamic buffer or after
proved IOC DMA stop. Timeout or unknown-stop paths keep descriptor, command and
IOVA; final teardown also keeps the mapper if resident ownership is still held.
Control growth maps the replacement before use, and diagnostic unregister
detaches ownership under the reply lock before unmapping outside it.

SMIDs have explicit reserved, posted, completed, quarantined and cancelled
ownership. Replies must name the outstanding function/handle; sense is per
command. A timed-out command is retained until its reply or a proved IOC stop.
TMF completion alone does not consume the original I/O reply. Recovery escalates
ABORT TASK, LUN RESET, TARGET RESET and IOC reset subject to credit/identity
checks. Uncertain writes are not replayed. An unproved DMA stop retains direct
client mappings and callback responsibility rather than exposing freed pages
to DMA; an unresolved operation in that state is a failure requiring recovery.

## PCI attach and target admission

The personality is a single `IOPCIPrimaryMatch` entry over the FreeBSD ID table
with a nonzero `IOProbeScore`. A personality with no probe score is not offered
the device at all while it is the only candidate: `probe()` never runs and the
controller stays unclaimed even though the kext is loaded and its classes are
registered. Keep the score in the plist.

`start()` enables memory decode and bus master, but treats
`setMemoryEnable()`/`setBusMasterEnable()` results as advisory: during early
enumeration they report failure while the command register ends up programmed
(observed `0x0006`). Acceptance is judged from
`configRead16(kIOPCIConfigCommand)` after one 2 ms retry, and a genuinely
unprogrammed register still fails the start.

Target admission walks SAS Device Page0 with `GET_NEXT_HANDLE`. A page whose
`AccessStatus` is `SATA_NEEDS_INITIALIZATION` (0x04) is admissible, matching
`mpt3sas _scsih_get_sas_device_status`; rejecting it hides a healthy
direct-attached SATA disk. Other access statuses and the unsupported/unauthorized
device flags still skip a page. `MPSEnumPages`, `MPSEnumLastStatus`,
`MPSEnumDisks`, `MPSEnumAbortStatus`, `MPSDiskHandle`, `MPSDiskAccessStatus` and
`MPSDiskSkipReason` describe the enumeration and the first skipped disk.

## Scheduling, bindings and lifecycle

There are 64 accepted-request records and up to 32 independent 64 KiB command
windows. SMIDs 1–9 serve config/probes, 10–25 synchronous bulk, 26–28 TMF,
29 cache, 30 UNMAP, 31 event ACK, and 32–63 async commands. Facts and memory
availability may lower the effective async count. One reply ring batches
retirement and only writes its host index after consuming descriptors.

The scheduler harvests ready commands before refilling windows, tracks each
post-time deadline and reports only the contiguous prefix preceding an error.
Accepted buffers and completions remain owned through one completion in a
separate serialized context. Cache/UNMAP sequence fences order prior accepted
I/O. No client callback is issued under the transport lock.

An allocation-unique nub binding is independent of a firmware handle or
synthetic SAS address. Controller/handle/binding are snapshotted together under
the nub lock, which is released before acquiring the transport lock. Every
read/write, flush, UNMAP, geometry and media-state admission validates the
supplied binding under the transport lock. A handle reassigned to a new target
invalidates the old association before publication; it cannot cause a stale
nub to capture the replacement's current generation. A logical device can
retain its binding across a path/handle change. Accepted work additionally
captures media/target generations for later execution/completion checks.

Replacement detection compares the new identity and geometry against the last
published identity even after a slot becomes absent or probe-invalid. Reusing
the same firmware handle and synthetic SATA port address cannot erase that
history. A different ATA/VPD identity, capacity or block size rotates the nub
binding; an unchanged logical device can regain readiness after revalidation.

SAS topology ADD, NOT_RESPONDING and PHY_CHANGED reasons require conservative
reprobe regardless of which target is currently selected. Before discovery can
reopen admission, the worker invalidates all candidate published media and
re-probes them. This intentionally pauses unaffected candidates too, avoiding
a quick replacement being hidden behind an unchanged handle/config page.

Table-owned nub references transfer to deferred termination actions when a
slot is cleared. Temporary notification references remain separate. This
preserves object lifetime across lock-free notifications without leaking the
original table ownership on repeated replug.
Retired bindings are also drained at initialization/publication, power-path
completion and stop boundaries, with transport ownership released before
OFFLINE notification, controller detachment, termination and final release.

| Lock | Responsibility |
|---|---|
| Nub controller lock | Atomic controller/handle/binding snapshot; no transport call while held |
| Transport `fIOLock` | Target table, binding validation, submission/mapping, discovery, recovery, PM |
| Scheduler IOLock | Request records, completion queue and sequence fences |
| Reply simple lock | SMID/reply ownership, ACK queue, event/topology latches |
| Post simple lock | Paired low/high MMIO descriptor writes |
| Reply wait lock | Activity ticket and sleep/wakeup |

The inner simple-lock order is reply then post. No allocation or wait occurs
under either simple lock. Notifications and client callbacks occur after
dropping transport ownership. A watchdog diagnostic snapshot must not block
reply progress behind a transport waiter.

Discovery combines SAS IOUnit/Device config pages, IR volume/configuration
pages and REPORT LUNS. Stable ATA/VPD identity and geometry are probed before
block publication. Handle and firmware SAS address alone do not establish disk
identity. `MPSIdentity.h` requires a non-placeholder printable ATA serial with
at least four characters after trim and more than one distinct byte, or an LU-associated binary
VPD83 EUI/NAA5/NAA6 identifier of the defined length. Repeated/null/zero/0xff and
generic serials, T10 vendor/model strings, SCSI name strings, port-associated
IDs and synthetic SATA addresses do not qualify. SATA VPD80 is not a fallback;
without trustworthy identity the driver refuses block publication. This is a
conservative identity policy, not proof that any returned serial is unique on
all hardware. IR members/spares remain filtered. On 007e WarpDrive the Page10 OEM
0x80 hide policy is applied before initial publication and on topology scans;
unreadable/invalid policy defaults to HIDE_ALWAYS. Raw event history preserves
vendor warnings; optional DirectDrive stripe mapping is not used.

Removal invalidates the binding/generation before deferred OFFLINE delivery,
checks for handle reuse and drains requests before physical REMOVE_DEVICE.
Per-LUN removal does not reset another logical unit. NOT READY/UNIT ATTENTION
probe results consume a bounded retry budget; they are distinguished from
transport faults. Recovery and rediscovery remain bounded.

Sleep stops admission, drains/fences requests, flushes eligible units and proves
DMA stopped before reclaiming memory. Wake rebuilds IOC state/queues/events
before topology access and revalidates media. Stop drains workers and completes
held work only after DMA safety is proved. Shutdown flush/IR notification/SATA
SSU follow their separate policy. These are unverified MPS kernel paths.

## SCSI and management contract

Block storage uses READ/WRITE(10)/(16), capacity, TUR, INQUIRY/VPD, sense,
SYNCHRONIZE CACHE and UNMAP. Transfer counts and SCSI/IOC/sense status determine
success; IOC acceptance or media-online alone is insufficient. FUA uses the
native capability or an ordered flush fallback. UNMAP requires B0/B2 limits
and LBPU; no rounding expands the caller's discard interval. LBPRZ controls
whether discarded blocks are promised to read zero. EEDP/TLR policy is gated
by reported target/IOC capabilities and remains hardware unverified.

Native management uses `MPSUserClient`, `MPSControlABI.h`, `MPSControl.cpp` and
`tools/mpsctl.py`. Root privilege is rechecked on every call. `mps_mgmt=1`
enables management; `mps_maintenance=1` also suppresses block publication.
Mutations require maintenance and an explicit acknowledgement. Prefix lengths,
directions, epochs and identity cookies are checked, then all SGE/sense addresses
are rebuilt. Persistent user physical pointers are never accepted.

Firmware requests use a 0x14 prefix, 16-byte TransactionContext (ContextSize 0,
DetailsLength 12, ImageOffset at +8 and ImageSize at +12), then a legacy MPI SGE.
Contiguous download segments share a nonzero token; ambiguous failure poisons
the transaction until reset. No implicit activation/reset is performed.
SMP has two 12-byte legacy SGEs, SATA's SGE begins at 0x30 and toolbox CLI's at
0x70. The offline CLI encodes the same SAS2 contract. NVMe opcodes are refused.

Diagnostic POST uses firmware-owned retained DMA. A command reply is not proof
of diagnostic-buffer release. Read/unregister require a matching unique ID and
released ownership; release event/response or proved reset ends ownership.
The event history is a bounded sequence ring separate from command EVENT_ACK.
Additional user masks cannot disable required driver events.

## Diagnostics and interpretation

`mps_debug=1` gates log lines and debug IORegistry properties. Absence on a
normal quiet boot is expected. Useful properties cover negotiated MPI/Facts,
queue geometry, target identity/generation/probe state, async outstanding/held
DMA, direct/copied/chained commands, reply batches/index writes, ACK failures,
sticky protocol faults, reset initiator and power rebuild state.

Counter properties are snapshots, not live totals: `publishTargetProperties()`
runs on topology, probe and hotplug events only, so adjacent ioreg samples are
never a valid delta source for a single I/O burst. Gated trace lines
include `MPS: sync-cache` and `MPS: unmap` with their arguments and result,
which is how filesystem flush traffic can be evidenced after the fact. The
whole-disk IOMedia nodes reject `DKIOCSYNCHRONIZECACHE` and `DKIOCUNMAP`
(ENOTTY), so flush and unmap evidence must come from filesystem activity or a
kernel harness, and FUA requires a kernel caller.

IRQ diagnostics must reflect actual interrupt actions and descriptors consumed
there, refreshed after start. They are not aliases for all replies: polling
can complete a request when no IRQ arrived. Compare IRQ deltas, retired
descriptors and actual user byte counts together. No MPS interrupt mode has
hardware qualification. One ring does not imply every card safely supports
every MSI-X mode; older SAS2008 revisions need their interrupt restrictions.

Use `mps_timeout_test`, `mps_timeout_every` and `mps_tmf_fail` only during an
authorized read-only recovery experiment. Keep serial kernel logging off by
default; it changes timing and throughput substantially.

## FreeBSD implementation map

| FreeBSD file / mechanisms | macOS implementation / replacement |
|---|---|
| `mps_pci.c`: IDs, newbus attach/BAR/interrupts | `MPSController`: exact PCI match, IOPCIDevice/MMIO, bounded single-ring interrupt/poll path; no newbus API |
| `mps.c`: transition/reset/handshake/Facts/Init | `MPSController`: bounded doorbell and diagnostic reset, SAS2 wire structs, EVENT_REPLAY handling |
| `mps.c`, `mpsvar.h`: commands, queue sizing/completions | Facts-bound SMID/owner/reply tables, independent sense, async scheduler and cancellation/quarantine |
| `mps.c`: bus_dma mapping, SGE/chains | `MPSDmaSgl.h`, `MPSAsync`, `MPSResidentDma.h`: per-device client/resident IODMACommand mappings, checked single-segment low32 resident IOVAs, legacy SGEs and independent fallback; no physical-address recovery from mapping failure |
| `mps_config.c`: config/manufacturing/IR/WarpDrive | `MPSTarget`, `MPSController`: checked two-phase pages, membership/volume filtering and Page10 hide policy; optional DirectDrive omitted |
| `mps_sas.c`, `mps_sas_lsi.c`: discovery/events/removal | Target table, SAS GET_NEXT_HANDLE, event ACK/history, handle-reuse checks, bounded deferred publication/removal |
| `mps_sas.c`: CAM CCB I/O and sense | `MPSAsync`, `MPSBlockDev`, `MPSScsi`: native block API, CDB generation/validation and per-command completions |
| `mps_sas.c`: timeouts/TMF/reinit | Ownership-aware abort/LUN/target/IOC recovery; no uncertain-write replay |
| `mps_mapping.c`, `mps_mapping.h`: DPM/CAM target IDs | Native stable identity/LUN/path bindings; firmware DPM writes and persistent CAM numbering not implemented |
| `mps_user.c`, `mps_ioctl.h`: control/pass-through/diagnostics | Root/boot-gated pointer-free IOUserClient and SAS2 CLI; no FreeBSD ioctl binary compatibility |
| `mps_table.c`, `mps_table.h`: decoded diagnostic tables | Native opt-in properties/logs plus raw event history; not an exhaustive copy of textual decode tables |
| `mps_sas.h`, `mpsvar.h`: platform objects/callouts/taskqueues | IOKit classes, IOLock/workloop/timer/thread_call, explicit lifecycle/reference ownership |
| `mpi/*.h`: wire ABI | Checked minimal `MPS/MPI` definitions and byte encoder; upstream licenses retained in NOTICE |
| FreeBSD suspend/resume limitations/shutdown | Native IOKit PM flush/quiesce/wake rebuild, plus IR shutdown/SATA SSU; all MPS runtime unverified |

The map is functional coverage, not FreeBSD API compatibility or a statement
that every upstream tuning knob, target mode, debug string or optional
acceleration is implemented. Scope gaps are recorded in [PLAN.md](PLAN.md).
