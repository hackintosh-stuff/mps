# SAS2 port decisions

Date: 2026-11-14. Driver version: 0.1.0.

This is the implementation checklist established before replacing the reused
macOS transport. No MPR runtime result qualifies MPS.

| Boundary | MPR reference | Required MPS behavior |
|---|---|---|
| IOC Facts | MPI 2.5/2.6, 0x44 bytes | MPI 2.0, 0x40 bytes; 0x26 reserved; chain frame = request frame |
| IOC Init | header 0x3400, HostPageSize | header 0x1200; 0x16 reserved, left zero |
| SCSI DMA | 16B IEEE, element offsets | 12B MPI Simple64/Chain64, DWORD offsets, 24-bit lengths, direction, LAST/EOB/EOL |
| Descriptor post | optional atomic/FastPath | paired low/high, standard SCSI/default/high-priority descriptors only |
| SAS device flags | MPI2.5 FastPath | never interpret SAS2 reserved bits as FastPath |
| Firmware | 32B prefix, IEEE | 0x14 prefix + 16B TransactionContext + MPI SGE; CLI agrees |
| SMP/SATA/toolbox | IEEE | two 12B MPI SGEs / MPI SGE at 0x30 / MPI SGE at 0x70 |
| Families | SAS3/GEN35/SEA/NVMe | exact 17 FreeBSD SAS2 IDs, no PCIe/NVMe/atomic branches |
| First attach | replay-capable baseline | missing EVENT_REPLAY forces diagnostic reset and renewed Facts |
| WarpDrive | absent | 007e Page10 OEM/hide policy before publication and each scan; ordinary IR I/O, no DirectDrive optimization |
| DMA address space | mixed resident physical/mapped client | every hardware DMA address is a device IOVA through the retained per-device mapper; resident buffers require one aligned low32 IOVA and no copy/bounce descriptor; translated hardware behavior remains unverified |
| Nub identity | handle-only admission | allocation-unique immutable binding, validated under transport lock for every media/data entry |
| Retired nub | leaked owning reference | transfer table ownership to deferred termination actions |
| IRQ evidence | stale/non-incremented properties | count actual interrupt action/drained descriptors and refresh snapshots |

FreeBSD headers in `reference/freebsd/sys/dev/mps/mpi` are the wire authority.
Linux `mpt3sas_base.c::_base_build_sg_scmd` is an independent behavior reference
for legacy SGL flag placement and DWORD offsets; Linux implementation is not copied.
Platform glue is adapted from adjacent MPR 0.24.0, which has no current native
load qualification. All MPS hardware behavior remains unverified until the
separately authorized procedures in TESTING.md have run.
