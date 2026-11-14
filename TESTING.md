# MPS hardware verification project

Date: 2026-11-14. Driver version: 0.1.0.

This is a plan, **not an execution report or authorization to deploy**. The
current task permits source changes, host validation and reversible builds. It
does not permit loading/injection, ESP/OpenCore edits, reboot/power operations,
firmware changes, formatting or test-disk writes. Every procedure below that
needs those actions requires a later task authorizing that exact scope.

## Fixture facts and identity prerequisites

The Linux survey is `_scratch/lsi2308/lsi2308-survey.md`: SAS2308 `1000:0087`,
subsystem `1000:3020`, revision `05`, firmware `20.00.07.00`, MPI `200.23`,
BAR1 64 KiB, eight PHYs with one 6 Gb/s SATA link, PCIe 3.0 x4. One disk reports
`ATA PCCOOLER SSD T20`, revision `5B`, 468,862,128 logical blocks of 512 bytes:

| Boundary | Value |
|---|---|
| Capacity | 240,057,409,536 bytes |
| Last valid LBA | 468,862,127 |
| Last aligned 4 KiB start | LBA 468,862,120 / byte offset 240,057,405,440 |
| First invalid LBA | 468,862,128 |

These values must be rechecked against the device on the day. IT/IR runtime mode
is not proved by the survey; obtain IOC Facts and relevant IR configuration
pages. `0x0009` is a transient firmware handle, `0x4433221100000000` is a
synthetic SATA address, `/dev/sdb` is a Linux node, and `testdisk`/the exFAT UUID
are not exclusive stable disk identity. The survey has no disk serial, GPT disk
GUID, partition GUIDs or reference hashes.

Before **first MPS load**, independently identify the disk under the existing
known-good environment and save a small manifest: HBA identity/firmware, disk
model/firmware/ATA serial or VPD identifier, capacity/logical+physical sector
size, GPT disk/partition GUIDs, partition layout, current node, controller path
and mount state. If identity cannot be made unambiguous, stop before writes or
destructive control commands. Read-only evidence collection may continue.

Capture pre-MPS streaming SHA-256 digests and exact returned byte counts for
deterministic windows (including LBA0/GPT, a partition data window, a high-LBA
window and the final 4 KiB). Record each offset and length so macOS reads the
same bytes. Capture two independent reads and require equal bytes/counts/hashes.
Do not claim a Linux survey supplied this baseline; it did not.

Before the first MPS load, establish and independently verify trusted no-auto-
mount or read-only isolation for the identified test disk and all its volumes.
MPS publishes writable media: running only read commands does not prevent
macOS Disk Arbitration, filesystem mounting or another process from writing.
An already configured, verified policy must exclude automatic mounts/writers;
confirm the resulting mount/open state before each case. If that isolation
cannot be demonstrated, first-load/read-only acceptance is **pending preparation**,
not ready to execute. This task does not authorize configuring that policy.

Use streaming checksums instead of disk images. Any backup expected to exceed
1 GB needs its range, total size and destination stated and explicit separate
permission before it begins. Do not split a backup to evade that limit. Preserve
existing PKG/EFI/history. Do not treat permission for a partition or an admin
command as backup permission.

## Risk and gating

| Level | Operations | Required gate |
|---|---|---|
| R0 host-only | Parse/build/protocol/CLI checks | Current task allows these; no device opened |
| R1 read-only kernel | New kext load/attach, identity, raw reads | Separate deployment/reboot authorization; known-good rescue and read baseline first; load can panic or affect DMA |
| R2 availability/recovery | TMF/reset, replug, sleep/wake/shutdown | Explicit interruption/recovery authorization; no mounted writer or other active controller workload |
| R3 media mutation | Bounded write/flush/FUA/UNMAP | Explicit disk/range authorization, freshly verified identity and a reversible bounded test footprint |
| R4 maintenance | Firmware/RAID changes or diagnostic experiments affecting controller ownership | Separate reviewed maintenance project; never a prerequisite for ordinary I/O acceptance |

Stop a run on panic, cross-target delivery, byte mismatch, duplicate callback,
uncontrolled DMA, unexpected firmware mutation, or resources that fail to drain.
Preserve evidence and use the approved recovery path; do not repeatedly inject
faults or increase load after such a result. A timeout/retained mapping after an
unproved stop is a failure to resolve, not permission to force-release memory.

## Build, artifact and rollback prerequisites

1. Run the input validator, independent SAS2 decoder/golden tests, management
   encoder tests and all translation-unit builds. Save full commands, compiler
   versions, exits and logs in `_scratch`. Check LF on every changed text input.
2. On an already running configured Mac, run `sh tools/build-noxcode.sh` in its
   isolated development directory. Record native link/plist/kmod/symbol/signature
   checks and the binary/plist SHA-256. Cross objects alone do not satisfy this.
   Do not reboot a rescue system solely to obtain a native compiler.
3. Before a separately authorized deployment, verify NanoKVM recovery access and
   the actual target/current OS without changing power. Follow workspace strict
   SSH host-key rules. Serial debug remains off (`debug=0x100`, no `serial=5`).
4. Verify the internal SATA disk and ESP model/capacity/GUID/location afresh.
   Back up only the necessary small config/kext within the approved scope. Never
   install to `/Library/Extensions`, a USB EFI or another system volume. Validate
   the proposed `Kernel > Add` difference with `plutil` and `ocvalidate`.
5. Define rollback before boot: remove/disable only the MPS injection entry and
   restore the saved small config/kext using the approved rescue method. Do not
   change BootOrder, reset NVRAM, flash firmware or repair/format any volume.
   Internal macOS APFS inspection remains read-only under workspace rules.

The artifact tested must be the artifact whose version/hash was recorded.
After an authorized boot compare loaded bundle version and the actual injected
binary/plist; a copied file is not proof it loaded. Enable `mps_debug=1` for
diagnostic acceptance and retain a quiet configuration for later measurements.

## Existing fixture acceptance sequence

Each row is a separate test case. Save start/end timestamps, command/argument
manifest, bytes requested/returned, I/O status/sense, relevant counter deltas,
full kernel log and target identity snapshots. After **every recovery or
re-enumeration**, rediscover the current BSD disk name from the verified stable
identity and IORegistry path; never reuse a remembered `diskN` blindly.

| Case / risk | Steps | Pass criteria |
|---|---|---|
| Attach / R1 | Boot approved artifact; inspect loaded bundle, Facts, Init, IRQ selection and target config | Exact MPS version/hash, MPI 0x0200 and plausible 0x40 Facts; Init succeeds with 0x1200 header/reserved fields zero; one eligible whole-disk medium; reported model, identity and exact geometry agree with independent baseline; no unexpected reset/fault |
| Empty topology / R1 + R2 | With separate physical-disconnection permission, boot the empty controller | Bounded empty online controller with no spurious medium; no permanent boot wait. Malformed Facts/config injection needs a separate harness, below |
| Read integrity / R1 | Unmounted raw device, exact pre-recorded windows, two reads each; include final 4 KiB and nonzero client offsets | Every read returns exact requested bytes and hashes match both pre-MPS baseline and repeat; not merely media-online or a rising read counter |
| I/O shape / R1 | 4/16/64 KiB and 1/16 MiB requests at QD 1/4/16/32; page-aligned user I/O, nonzero offsets and command-window splits | Exact bytes and reference-region content for each accepted request; no cross-request corruption, duplicate/missing callback, sticky fault or unplanned reset. Fragmented/unaligned kernel-descriptor control requires the separate harness below |
| SGL/fallback / R1 | Repeat the same windows with direct DMA enabled and `mps_direct_dma=0`; exercise multi-chain mappings and command-window splits | Both paths match the same baseline and return correct counts; chain/direct/fallback diagnostics agree with the path actually used; active maps and posted SMIDs return to zero after normal drain |
| End of medium / R1 | Read final valid block/final 4 KiB; request one block beyond capacity and a request spanning EOF | Valid tail bytes match baseline. Distinguish raw BSD semantics: EOF may return zero or a short in-range read; record actual errno/count. Internal CDB/block API rejects a starting LBA at capacity and overrun before posting. A raw short read does not prove a CDB crossed bounds |
| Argument bounds / R1 | Record user-visible zero-length and non-block-aligned reads, separating OS validation from driver validation | Documented syscall results/counts. Proving internal overflow rejection/no post/no callback requires the separate driver-entry harness below |
| IRQ/poll / R1 | Record real ISR/action and IRQ-retired reply deltas, total replies and exact client data; separately exercise bounded polling if the mode is available | I/O content/counts correct; IRQ evidence increments at actual IRQ sites when claiming IRQ delivery; polling-only success is labelled polling. No claim that all MSI/MSI-X/INTx modes are qualified |
| Normal drain / R1 | Stop issuing requests, wait for completion, snapshot owners/mappings/events | Async outstanding and held DMA zero, no posted/quarantined runtime SMID, no stranded accepted callback; resident pools and intentional event subscriptions are not leaks. ACK queue eventually drains |
| Timeout/TMF / R2 | Use the exact one-shot setup below; run ABORT, LUN, TARGET and IOC-only variants in separate authorized boots | Expected error/count, no uncertain-write replay; recovery bounded, identities revalidated and newly resolved BSD node serves exact baseline bytes; no resource leak/duplicate completion. Record target/fabric events instead of assuming TMF is fabric-local |
| Replug / R2 | Unmount, authorize physical pull/replug; repeat once with a read in flight | Departed medium rejects requests; old binding cannot touch replacement; replug yields exactly one verified medium; error/returned prefix are accurate and post-recovery reads match baseline; no accumulating nub/reference leak |
| Sleep/wake / R2 | Short and long sleeps after normal drain, then resolve disk identity again | Flush/quiesce and one planned rebuild are recorded, medium returns with verified geometry, exact read bytes/hash match baseline, no extra sticky-fault/reset cycle or held DMA after drain |
| Shutdown / R2 | Drain/unmount, shutdown once under explicit authorization | Bounded flush/IR notification/SATA SSU policy and proved DMA stop; no driver hang. Subsequent authorized boot repeats identity and integrity checks; a black display alone is insufficient |

Direct I/O must bypass filesystem/page-cache substitution where feasible.
Capture the harness's actual syscall return values, not only shell exit status.
Check normal error injection status separately from unexpected I/O errors.
Throughput has **no acceptance threshold derived from Linux queue depth/vector
count or MPR's old 150 MiB/s path**. Only after integrity passes, measure this
fixture's own repeated median/spread at documented QD, sizes, CPU, firmware and
diagnostic settings; never declare a performance gain from counters alone.

### One-shot recovery setup

Use `mps_timeout_test=1 mps_timeout_every=0 mps_tmf_fail=0` for the ABORT variant.
For separate runs use `mps_tmf_fail=1`, `2` and `3` for LUN, TARGET and IOC-only
escalation respectively, keeping timeout_test=1 and timeout_every=0. The
one-shot activates on the first runtime command after `!fStarting`; it is not
a request that waits for a human to press a later button. Under verified
no-auto-mount/read-only isolation, arrange that first runtime request to be the
intended read and capture its actual result and injection evidence. If another
runtime request consumes it, the intended case has not been performed.

Remove **all three** injection arguments after each case, verify the clean
configuration and obtain the separately authorized boot before the next normal
integrity check. Do not leave every-N injection enabled or accumulate variants
in one boot. Recording an accepted boot-argument string alone does not prove
the requested TMF actually reached the target.

### Harness prerequisites that remain pending

Host tests exercise byte/policy functions only. There is currently no approved
kernel/firmware fault harness for malformed Facts/config, deliberately
fragmented or unaligned kernel IOMemoryDescriptors, internal integer-overflow
admission, or IOStorage FUA selection. Those runtime cases are **pending harness
preparation and review**, not directly executable with the present CLI/fixture.
The harness must call the real entry point, capture returned status/byte count
and hardware-post/callback evidence, and remain within the later authorization.
Do not infer kernel-path coverage from a userspace request that the OS rejected
first, a host encoder test, or `fsync`.

## Native management runtime rejection matrix

Offline packet/golden checks do not establish runtime access control. After
separate load/test authorization, use a reviewed read-only probe harness and
retain call return/status, epoch/identity and transport-post evidence. Firmware
or media mutation is not permitted just to test that a guard rejects it.

| Configuration / caller or packet | Expected result |
|---|---|
| Ordinary boot, management off, administrator | Connection refused; no control command posted |
| `mps_mgmt=1`, unprivileged caller | Open and method invocation refused, including inherited/transferred connections |
| `mps_mgmt=1`, administrator, valid read-only info/target request | Bounded correctly sized result for the selected MPS registry ID |
| Stale transport epoch, WWID or target generation/binding | Explicit stale/identity error before target command; never refresh/retry a mutation silently |
| Ordinary management boot plus mutation acknowledgement | Media/array/firmware mutation refused because maintenance isolation is absent |
| Maintenance boot, mutation request without CLI acknowledgement | Refused; maintenance alone is not permission for the operation |
| Maintenance plus acknowledgement | Only eligible fully validated operations can be submitted; executing one still needs its separate explicit media/maintenance authorization |
| Short/long prefix, over-limit data, conflicting directions, wrong reserved fields | Bounds/direction error, no hardware post or buffer overrun |
| Caller SGE/sense/physical-address material | Never used as a device address: malformed prefixes rejected or accepted fixed fields rebuilt from kernel-owned mappings; no user-controlled DMA address reaches the IOC |
| NVMe/MPI2.5/2.6 opcode or descriptor request | Unsupported/refused before hardware post |
| Request aimed at another registry ID, stale/reused handle or replacement disk | No cross-controller/target delivery; bind to the explicitly selected current identity |

Packet sizes/status/sense and actual data bytes must agree. A successful
IOUserClient transport call alone is not successful firmware completion.

## Additional authorization for bounded writes

The current task does not authorize these steps. Immediately before any later
write, re-read HBA/disk identity, serial/VPD, GPT GUID, geometry/current BSD node
and mount/open state. The model/label/capacity combination alone is insufficient.
Write only the explicitly approved file or LBA interval. If a restore image is
needed, approve and size it beforehand and keep it within that interval; do not
silently image the entire disk.

| Case / risk | Procedure after permission | Pass criteria |
|---|---|---|
| Write/readback / R3 | Use a bounded file (for example 32 MiB only if authorized) or approved LBA window; deterministic payload, varied aligned sizes/QD; flush and read through a cache-bypassing path | Exact write/read byte counts and byte-for-byte payload equality; unaffected reference windows unchanged; filesystem verifies clean and footprint is removed/restored as authorized |
| Cache / R3 | Interleave accepted writes and explicit synchronize barriers, record command ordering and result; reopen/re-resolve on recovery | Each barrier covers prior accepted writes to that same binding/LUN, completion order is correct, all readback bytes match; no stale binding flushes a replacement |
| Native FUA / R3 | Pending a reviewed kernel/storage harness that actually supplies `kIOStorageOptionForceUnitAccess`; then verify emitted CDB FUA or ordered flush fallback | Correct payload/count and logged/observed command policy for that request. `fsync` alone does not prove FUA selection, power-loss durability or cache behavior |
| UNMAP / R3 | Confirm B0/B2/LBPU and limits. Either keep a specifically authorized test file allocated, establish its exact extent ownership, unmount/quiesce the filesystem and verify adjacent sentinels; or obtain separate authorization for an exact raw LBA interval | Exact authorized range represented without outward rounding; limits respected; adjacent bytes intact. Deleted-file extents are not exclusive ownership and must not be reused. Require zero readback **only if LBPRZ=1**; otherwise content is unspecified |
| Write removal / R3 + R2 | Only after read-only removal and write correctness pass; one bounded in-flight write with explicit failure expectation | Accurate success/error/prefix, no replay of uncertain writes, no write to replacement/other target; verify approved region and filesystem after recovery without automatic repair |

Raw firmware download, array creation/deletion and destructive diagnostic
commands are excluded from these tests. Offline golden bytes establish packet
encoding, not safety of an image, array operation or firmware update.

## Coverage that needs other hardware

| Capability | Extra prerequisite | Required evidence |
|---|---|---|
| 4Kn | Verified 4096-byte logical-sector disk | Geometry, alignment, exact read/write/window boundaries on that disk |
| Above 2 TiB | Disk/LUN with valid high LBAs | READ/WRITE(16), high-LBA/tail byte equality and capacity; host golden CDBs alone are not hardware proof |
| SAS SSP | Real SSP disk/target | SSP read/write/sense/TMF, stable VPD identity and exact data |
| EEDP | Protection-formatted type1/type3 LUN, IOC capability, approved test data | Correct protection/guard/reference handling and explicit expected protection-error cases |
| TLR/tape | Supporting sequential SSP target and administrative access | Supported-page negotiation, TLR status/error behavior; no claim of native macOS tape integration |
| IR | Test controller/known disposable array plus explicit array-write authorization | Volume geometry/state and member/spare hiding, exact logical-volume data and shutdown; ordinary disk survey does not prove IR |
| WarpDrive | Actual 1000:007e device with controlled Page10/volume fixture | OEM/policy read, hide/expose for 0/1/2 before publication and after IR changes; read failure hides; normal IR data correct; no DirectDrive optimization claim |
| Multiple targets/LUNs | At least two independently identified endpoints/LUNs | Distinct payloads, routing isolation, one removal/replacement while the other serves exact reads; old nub rejects replacement for read/write/cache/UNMAP/geometry |
| Expander/enclosure | Verified expander/backplane/SES fixture | Discovery, SMP/SEP, hotplug/recovery and independent target data; direct attach cannot qualify it |
| Controller-local multiple paths | Two valid firmware paths with common verified device identity | Path loss/rebind retains correct immutable binding, no duplicate medium or uncertain-write replay; does not prove cross-controller MPIO/ALUA |
| Other PCI IDs / older SAS2008 revision | Representative device/firmware, including B0 interrupt restriction | Attach, allowed interrupt mode, bounded fallback, integrity and recovery; matching alone is insufficient |
| Translating IOMapper | Controlled machine with active per-device mapper, safely recoverable test media and diagnostic harness | Validate device IOVA/range/alignment, mapping rejection without physical fallback, no-bounce shared-ring coherence, exact data and retained mappings through timeout/stop; source implementation is not hardware qualification |

## Evidence package and exit conditions

Store compact identity/build/test manifests, command logs, kernel logs,
IORegistry snapshots, actual byte/error reports, region hashes and explicit
unexecuted cases in `_scratch`. Streaming checksums need no disk-sized backup.
For any failure preserve the first fault and surrounding time window, request
handle/LUN/binding/epoch, sense/IOC status, IRQ/poll mode and owner states. Redact
secrets if sharing logs. Do not overwrite old evidence with a later run.

A case passes only when its stated data/count/resource criteria are met. A
host mock, static assertion, compile, rising counter or online property cannot
substitute for its hardware result. The final report lists executed cases,
exact artifact/fixture, commands/exits, failures and all pending qualification.
Rollback completes only after the known-good configuration is restored under
its authorization and the resulting boot/media state is observed.
