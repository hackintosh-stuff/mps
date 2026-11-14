#include <IOKit/IOLib.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/storage/IOMedia.h>
#include <libkern/OSByteOrder.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "MPSController.h"
#include "MPSBlockDev.h"
#include "MPI/mpi2_min.h"
#include "MPI/mpi2_target.h"
#include "MPSCompletion.h"
#include "MPSWire.h"
#include "MPSBinding.h"
#include "MPSDmaSgl.h"
#include "MPI/mpi2_sas2.h"
#include "MPI/mpi2_mgmt.h"

#define super IOService
OSDefineMetaClassAndStructors(MPSController, IOService)

/* Exact 17 unique vendor/device IDs from FreeBSD mps_pci.c.
 * Subsystem-specific Intel descriptions share these same transport IDs. */
struct MPSPciId { UInt32 vendorDevice; const char *name; bool verified; UInt8 flags; };
static const MPSPciId kMPSPciIds[] = {
    { 0x00701000U, "SAS2004", false, 0 },
    { 0x00721000U, "SAS2008", false, 0 },
    { 0x00741000U, "SAS2108_1", false, 0 },
    { 0x00761000U, "SAS2108_2", false, 0 },
    { 0x00771000U, "SAS2108_3", false, 0 },
    { 0x00641000U, "SAS2116_1", false, 0 },
    { 0x00651000U, "SAS2116_2", false, 0 },
    { 0x00801000U, "SAS2208_1", false, 0 },
    { 0x00811000U, "SAS2208_2", false, 0 },
    { 0x00821000U, "SAS2208_3", false, 0 },
    { 0x00831000U, "SAS2208_4", false, 0 },
    { 0x00841000U, "SAS2208_5", false, 0 },
    { 0x00851000U, "SAS2208_6", false, 0 },
    { 0x00861000U, "SAS2308_1", false, 0 },
    { 0x00871000U, "SAS2308_2", false, 0 },
    { 0x006e1000U, "SAS2308_3", false, 0 },
    { 0x007e1000U, "SSS6200", false, 0 },
};

static const MPSPciId *
mpsLookupPciId(UInt32 vendorDevice)
{
    for (unsigned i = 0; i < sizeof(kMPSPciIds) / sizeof(kMPSPciIds[0]); i++) {
        if (kMPSPciIds[i].vendorDevice == vendorDevice) return &kMPSPciIds[i];
    }
    return NULL;
}

/* No diagnostic output until boot-args have been parsed. */
bool gMPSDebugEnabled = false;

void
mpsInitDebugOutput(void)
{
    int debug = 0;
    gMPSDebugEnabled =
        PE_parse_boot_argn("mps_debug", &debug, sizeof(debug)) && debug != 0;
}

/* Hard bounds for every hardware wait. start() runs in the boot path, so
 * bring-up must finish quickly instead of using the 300 s timeouts of
 * the FreeBSD driver. */
#define MPS_DIAG_UNLOCK_TRIES   20
#define MPS_DIAG_SETTLE_TRIES   100
#define MPS_READY_TRIES         60
#define MPS_ACK_ITERATIONS      4000
#define MPS_DBINT_ITERATIONS    4000

/* Reply free/post queue geometry. The queues must hold one unused entry
 * and be a multiple of 16 (mps.c:1379-1380); IOC Init advertises the same
 * depths and the request frame size in 32-bit words (mps.c:1264-1266). */
#define MPS_REPLY_FRAMES    128
#define MPS_REQUEST_FRAMES  128
#define MPS_IOC_STATE_TRIES     1000
#define MPS_DMA_LOW_MASK        0x00000000FFFFF000ULL

/* Config-page reads poll the reply queue instead of taking interrupts. */
#define MPS_CONFIG_TRIES           2000
#define MPS_CONFIG_PAGE_BYTES      128

/* A reply wait uses the interrupt first, then falls back to one poll pass. */
#define MPS_REPLY_TIMEOUT_MS 2000

/* PORT_ENABLE restarts firmware discovery and is answered only once that
 * discovery finishes, so it scales with the topology behind the HBA instead
 * of with a register handshake. mpt3sas allows 300 s for the same command
 * after a host reset; the 2 s management budget is only valid for config
 * pages and IOC Init. */
#define MPS_PORT_ENABLE_TIMEOUT_MS 30000

/* topology config page staging buffer (SAS IO Unit Page0 with 8+ phys). */
#define MPS_PAGE_BUF_BYTES      4096

/* Each SMID owns its own sense DMA area, including concurrently active IO. */
#define MPS_SENSE_BYTES         96U
#define MPS_SENSE_POOL_BYTES    (MPS_REQUEST_FRAMES * MPS_SENSE_BYTES)

/* bulk transfer staging buffers: one window per in-flight command so a single
 * request can keep several chunk commands outstanding instead of paying the
 * per-command round trip serially. fDataBytes stays the window size. */
#define MPS_DATA_BUF_BYTES      65536
#define MPS_IO_SLOTS            4
#define MPS_DATA_POOL_BYTES     (MPS_DATA_BUF_BYTES * MPS_IO_SLOTS)
#define MPS_SM_EVENT_ACK        31

enum {
    kMPSProtocolAckOverflow = 1, kMPSProtocolAckReply,
    kMPSProtocolManagementTimeout, kMPSProtocolEventLength,
    kMPSProtocolManagementBusy, kMPSProtocolManagementReply,
    kMPSProtocolAckTimeout,
    kMPSProtocolInterruptUnavailable, kMPSProtocolUnexpectedCompletion,
    kMPSProtocolIocNotOperational
};

/* Two registered PM states drive setPowerState(): drain/flush/quiesce before
 * power removal, then rebuild and revalidate before accepting requests. */
enum {
    kMPSPowerOff = 0,
    kMPSPowerOn
};
static const unsigned long kMPSPowerStateCount = 2;

static IOPMPowerState gMPSPowerStates[kMPSPowerStateCount] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMPowerOn | kIOPMDeviceUsable,
      kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
};

struct MPSRegisterInfo
{
    const char *name;
    UInt32      offset;
};

static const MPSRegisterInfo kMPSRegisters[] = {
    { "Doorbell",              MPI2_DOORBELL_OFFSET },
    { "WriteSequence",         MPI2_WRITE_SEQUENCE_OFFSET },
    { "HostDiagnostic",        MPI2_HOST_DIAGNOSTIC_OFFSET },
    { "DiagRWData",            MPI2_DIAG_RW_DATA_OFFSET },
    { "DiagRWAddressLow",      MPI2_DIAG_RW_ADDRESS_LOW_OFFSET },
    { "DiagRWAddressHigh",     MPI2_DIAG_RW_ADDRESS_HIGH_OFFSET },
    { "HostInterruptStatus",   MPI2_HOST_INTERRUPT_STATUS_OFFSET },
    { "HostInterruptMask",     MPI2_HOST_INTERRUPT_MASK_OFFSET },
    { "DCRData",               MPI2_DCR_DATA_OFFSET },
    { "DCRAddress",            MPI2_DCR_ADDRESS_OFFSET },
    { "ReplyFreeHostIndex",    MPI2_REPLY_FREE_HOST_INDEX_OFFSET },
    { "ReplyPostHostIndex",    MPI2_REPLY_POST_HOST_INDEX_OFFSET },
    { "HCBSize",               MPI2_HCB_SIZE_OFFSET },
    { "HCBAddressLow",         MPI2_HCB_ADDRESS_LOW_OFFSET },
    { "HCBAddressHigh",        MPI2_HCB_ADDRESS_HIGH_OFFSET },
    { "Scratchpad0",           MPI2_SCRATCHPAD0_OFFSET },
    { "Scratchpad1",           MPI2_SCRATCHPAD1_OFFSET },
    { "Scratchpad2",           MPI2_SCRATCHPAD2_OFFSET },
    { "Scratchpad3",           MPI2_SCRATCHPAD3_OFFSET },
    { "RequestDescriptorPostLow",  MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET },
    { "RequestDescriptorPostHigh", MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET },
};

UInt32
MPSController::readReg(UInt32 offset)
{
    return fRegs[offset / sizeof(UInt32)];
}

void
MPSController::writeReg(UInt32 offset, UInt32 value)
{
    fRegs[offset / sizeof(UInt32)] = value;
}

void
MPSController::postRequestDescriptor(UInt32 low, UInt32 high)
{
    if (fPostLock == NULL || fRegs == NULL) {
        return;
    }
    /* Reply processing can submit an ACK while a normal command submits.
     * Keep the two MMIO halves indivisible. Lock order: reply -> post only. */
    IOSimpleLockLock(fPostLock);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET, low);
    writeReg(MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET, high);
    IOSimpleLockUnlock(fPostLock);
}

UInt64
MPSController::senseAddress(UInt32 smid) const
{
    if (fSenseBuffer == NULL || smid == 0 || smid >= fRequestCount ||
        smid >= MPS_REQUEST_FRAMES) return 0;
    return fSensePhys + (UInt64)smid * MPS_SENSE_BYTES;
}

bool
MPSController::prepareScsiSense(UInt32 smid)
{
    if (fReplyLock == NULL || fSenseBuffer == NULL || senseAddress(smid) == 0)
        return false;
    UInt8 *sense = (UInt8 *)fSenseBuffer->getBytesNoCopy();
    if (sense == NULL) return false;
    IOSimpleLockLock(fReplyLock);
    MPSCommandOwner *owner = &fCommandOwners[smid];
    bool ok = !fStopping && !fProtocolRecoveryPending &&
        (owner->state == kMPSCommandFree || owner->state == kMPSCommandReserved);
    if (ok) {
        owner->state = kMPSCommandReserved;
        owner->function = MPI2_FUNCTION_SCSI_IO_REQUEST;
        owner->handle = (UInt16)fAttachedDevHandle;
        owner->epoch = fTransportEpoch;
        fReplySlots[smid].valid = 0;
        memset(sense + (UInt64)smid * MPS_SENSE_BYTES, 0, MPS_SENSE_BYTES);
    }
    IOSimpleLockUnlock(fReplyLock);
    return ok;
}

bool
MPSController::commandDmaSafe(UInt32 smid)
{
    if (fReplyLock == NULL || smid == 0 || smid >= MPS_REQUEST_FRAMES)
        return false;
    IOSimpleLockLock(fReplyLock);
    UInt8 state = fCommandOwners[smid].state;
    bool safe = state != kMPSCommandPosted && state != kMPSCommandQuarantined;
    IOSimpleLockUnlock(fReplyLock);
    return safe;
}

void
MPSController::cancelUnpostedCommand(UInt32 smid)
{
    if (fReplyLock == NULL || smid == 0 || smid >= MPS_REQUEST_FRAMES) return;
    IOSimpleLockLock(fReplyLock);
    if (fCommandOwners[smid].state == kMPSCommandReserved) {
        fCommandOwners[smid].state = kMPSCommandFree;
        if (smid < 10) fManagementBusy[smid] = false;
    }
    IOSimpleLockUnlock(fReplyLock);
}

bool
MPSController::postOwnedRequestLocked(UInt32 smid, UInt32 flags, UInt32 high)
{
    if (smid == 0 || smid >= fRequestCount || smid >= MPS_REQUEST_FRAMES ||
        fRequestBuffer == NULL || fStopping || fProtocolRecoveryPending ||
        fCommandOwners[smid].state != kMPSCommandReserved) return false;
    UInt8 *base = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (base == NULL) return false;
    UInt8 *frame = base + (UInt64)smid * fRequestFrameSize;
    MPSCommandOwner *owner = &fCommandOwners[smid];
    owner->function = frame[3];
    owner->lun = 0;
    if (frame[3] == MPI2_FUNCTION_SCSI_IO_REQUEST ||
        frame[3] == MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH)
        (void)mpsDecodeLun(((MPSScsiIoRequest *)frame)->LUN, &owner->lun);

    owner->handle = 0;
    if (frame[3] == MPI2_FUNCTION_SCSI_IO_REQUEST ||
        frame[3] == MPI2_FUNCTION_SCSI_TASK_MGMT ||
        frame[3] == MPI2_FUNCTION_SATA_PASSTHROUGH ||
        frame[3] == MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH)
        owner->handle = (UInt16)((UInt16)frame[0] | ((UInt16)frame[1] << 8));
    else if (frame[3] == MPI2_FUNCTION_SAS_IO_UNIT_CONTROL)
        owner->handle = (UInt16)((UInt16)frame[4] | ((UInt16)frame[5] << 8));
    MPSTargetSlot *target = findTarget(mpsTargetKey(owner->handle, owner->lun));
    owner->targetIdentity = target ? target->sasAddress : 0;
    owner->targetGeneration = target ? target->generation : 0;
    owner->targetKind = target ? (UInt8)target->kind : 0;
    owner->epoch = fTransportEpoch;
    owner->state = kMPSCommandPosted;
    fReplySlots[smid].valid = 0;
    postRequestDescriptor(flags | (smid << 16), high);
    return true;
}

bool
MPSController::storeCommandReplyLocked(UInt32 smid, UInt8 kind,
                                      const UInt8 *reply, UInt32 bytes)
{
    if (smid == 0 || smid >= fRequestCount || smid >= MPS_REQUEST_FRAMES) {
        fUnexpectedCompletionCount++;
        recordProtocolFaultLocked(kMPSProtocolUnexpectedCompletion, smid);
        return false;
    }
    MPSCommandOwner *owner = &fCommandOwners[smid];
    if ((owner->state != kMPSCommandPosted &&
         owner->state != kMPSCommandQuarantined) ||
        owner->epoch != fTransportEpoch || fReplySlots[smid].valid != 0) {
        /* Never overwrite an unconsumed completion, even when the SMID is
         * numerically valid. A software epoch alone cannot tag IOC replies. */
        fUnexpectedCompletionCount++;
        recordProtocolFaultLocked(kMPSProtocolUnexpectedCompletion, smid);
        return false;
    }
    bool valid = kind == 2 ? (owner->function == MPI2_FUNCTION_SCSI_IO_REQUEST ||
                              owner->function == MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH)
        : kind == 1 && reply != NULL && bytes >= 4 &&
          reply[3] == owner->function;
    if (valid && kind == 1 &&
        (owner->function == MPI2_FUNCTION_SCSI_IO_REQUEST ||
         owner->function == MPI2_FUNCTION_SCSI_TASK_MGMT)) {
        const UInt32 minimum = owner->function == MPI2_FUNCTION_SCSI_IO_REQUEST
            ? MPS_SCSIIO_REPLY_MIN_BYTES : sizeof(MPSScsiTaskMgmtReply);
        UInt16 handle = (UInt16)((UInt16)reply[0] | ((UInt16)reply[1] << 8));
        valid = bytes >= minimum && handle == owner->handle;
        if (valid && owner->function == MPI2_FUNCTION_SCSI_TASK_MGMT) {
            UInt8 *request = (UInt8 *)fRequestBuffer->getBytesNoCopy() +
                (UInt64)smid * fRequestFrameSize;
            valid = reply[5] == request[5];
        }
    }

    if (valid && kind == 1 && owner->function == MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH) {
        valid = bytes >= MPS_SCSIIO_REPLY_MIN_BYTES &&
            mpsControlLe16(reply) == owner->handle;
    }
    if (valid && kind == 1 && owner->function == MPI2_FUNCTION_SATA_PASSTHROUGH) {
        valid = bytes >= sizeof(MPSSataPassthroughReply) &&
            (UInt16)((UInt16)reply[0] | ((UInt16)reply[1] << 8)) == owner->handle;
    }
    if (valid && kind == 1 && owner->function == MPI2_FUNCTION_SAS_IO_UNIT_CONTROL) {
        valid = bytes >= sizeof(MPSSasIoUnitControlReply) &&
            (UInt16)((UInt16)reply[4] | ((UInt16)reply[5] << 8)) == owner->handle;
        if (valid) {
            UInt8 *request = (UInt8 *)fRequestBuffer->getBytesNoCopy() +
                (UInt64)smid * fRequestFrameSize;
            valid = reply[0] == request[0];
        }
    }
    if (!valid) {
        fUnexpectedCompletionCount++;
        recordProtocolFaultLocked(kMPSProtocolUnexpectedCompletion, smid);
        return false;
    }
    MPSReplySlot *slot = &fReplySlots[smid];
    memset(slot->reply, 0, sizeof(slot->reply));
    if (kind == 1)
        memcpy(slot->reply, reply, bytes < sizeof(slot->reply) ? bytes : sizeof(slot->reply));
    slot->smid = (UInt16)smid;
    slot->kind = kind;
    owner->state = kMPSCommandComplete;
    slot->valid = 1;
    return true;
}

void
MPSController::recordProtocolFaultLocked(UInt32 code, UInt32 smid)
{
    if (!fProtocolRecoveryPending) {
        fProtocolFaultCode = code;
        fProtocolFaultSmid = smid;
        fTopologyGeneration++;
    }
    /* Diagnostic trail: the first fault of a recovery round, kept across the
     * resetProtocolState() that the following reinit performs. */
    fStickyFaultCode = code;
    fStickyFaultSmid = smid;
    fStickyFaultCount++;
    fProtocolRecoveryPending = true;
    fHotplugPending = true;
}

void
MPSController::recordProtocolFault(UInt32 code, UInt32 smid)
{
    if (fReplyLock == NULL) {
        return;
    }
    IOSimpleLockLock(fReplyLock);
    recordProtocolFaultLocked(code, smid);
    IOSimpleLockUnlock(fReplyLock);
}

void
MPSController::noteBadReply(UInt32 site, UInt32 value, UInt32 smid,
                            const UInt8 *frame, UInt32 bytes)
{
    /* Allocation-free: several callers hold fReplyLock. */
    fBadReplySite = site;
    fBadReplyValue = value;
    fBadReplySmid = smid;
    fBadReplyCount++;
    fBadReplyFunction = (frame != NULL && bytes > 3) ? frame[3] : 0xFF;
    fBadReplyMsgLen = (frame != NULL && bytes > 2) ? frame[2] : 0xFF;
    fBadReplyWord = 0;
    if (frame != NULL && bytes >= 4) {
        memcpy(&fBadReplyWord, frame, sizeof(UInt32));
    }
}

bool
MPSController::beginManagementRequest(UInt32 smid)
{
    bool ready = false;

    if (smid == 0 || smid >= 10 || fReplyLock == NULL ||
        fRequestBuffer == NULL || fStopping) {
        return false;
    }
    IOSimpleLockLock(fReplyLock);
    if (!fProtocolRecoveryPending) {
        if (fManagementBusy[smid] ||
            fCommandOwners[smid].state != kMPSCommandFree) {
            recordProtocolFaultLocked(kMPSProtocolManagementBusy, smid);
        } else {
            fManagementBusy[smid] = true;
            fCommandOwners[smid].state = kMPSCommandReserved;
            fCommandOwners[smid].epoch = fTransportEpoch;
            fReplySlots[smid].valid = 0;
            ready = true;
        }
    }
    IOSimpleLockUnlock(fReplyLock);
    return ready;
}

bool
MPSController::finishManagementRequest(UInt32 smid, UInt32 timeoutMs,
                                        UInt8 *reply, UInt8 *kind)
{
    if (smid == 0 || smid >= 10 || fReplyLock == NULL) return false;
    /* SMIDs 1, 2 and 4 are config/event requests posted while the IOC is
     * re-enumerating the fabric. During the wake settling window their reply
     * can legitimately take longer than 2 s. PortEnable (3) already has its
     * own 30 s allowance, and SCSI probes (6) must keep the short timeout so
     * a device that never answers still classifies as a definite failure. */
    UInt32 timeout = timeoutMs;
    if ((smid == 1 || smid == 2 || smid == 4) && fWakeSettleTicks != 0 &&
        timeout < kMPSWakeConfigTimeoutMs)
        timeout = kMPSWakeConfigTimeoutMs;
    /* PortEnable is the firmware's whole discovery pass. After a long sleep
     * an expander plus a spun-down disk can keep it busy well past 30 s;
     * timing out there quarantines SMID 3 and turns one wake rebuild into a
     * failed rebuild plus a second, fault-driven reset. */
    if (smid == 3 && fWakeSettleTicks != 0 &&
        timeout < kMPSWakePortEnableTimeoutMs)
        timeout = kMPSWakePortEnableTimeoutMs;
    if (!waitForSmid(timeout, smid) ||
        !takeReplySlot(smid, reply, 64, kind)) {
        /* The IOC may still own this frame AND the shared DMA buffer.
         * Keep the busy bit until a diagnostic reset has stopped old DMA. */
        IOSimpleLockLock(fReplyLock);
        if (fCommandOwners[smid].state == kMPSCommandPosted)
            fCommandOwners[smid].state = kMPSCommandQuarantined;
        recordProtocolFaultLocked(kMPSProtocolManagementTimeout, smid);
        IOSimpleLockUnlock(fReplyLock);
        return false;
    }
    IOSimpleLockLock(fReplyLock);
    fManagementBusy[smid] = false;
    bool healthy = !fProtocolRecoveryPending;
    IOSimpleLockUnlock(fReplyLock);
    return healthy;
}

void
MPSController::resetProtocolState(void)
{
    /* Caller has stopped the old IOC's DMA and disabled reply processing.
     * Never use this to reclaim a timed-out frame while the IOC is live. */
    if (fReplyLock == NULL) {
        return;
    }
    IOSimpleLockLock(fReplyLock);
    memset(fManagementBusy, 0, sizeof(fManagementBusy));
    memset(fTmfBusy, 0, sizeof(fTmfBusy));
    ++fTransportEpoch;
    fControlDmaBusy = false;
    fFirmwareDownloadActive = fFirmwareDownloadPoisoned = false;
    fFirmwareTotal = fFirmwareOffset = 0; fFirmwareToken = 0;
    for (unsigned type = 0; type < 3; ++type) {
        fDiagnostic[type].owned = false;
        fDiagnostic[type].valid = false;
    }
    for (UInt32 smid = 0; smid < MPS_REQUEST_FRAMES; ++smid) {
        bool runtime = (smid >= 10 && smid <= 25) || mpsIsAsyncSmid(smid) ||
                       smid == 29 || smid == 30;
        bool occupied = fCommandOwners[smid].state != kMPSCommandFree;
        memset(&fReplySlots[smid], 0, sizeof(fReplySlots[smid]));
        if (runtime && occupied) {
            /* The caller has proved old DMA stopped. Preserve one synthetic
             * cancellation for each old waiter instead of waiting for replies
             * that this reset necessarily discarded. */
            fCommandOwners[smid].state = kMPSCommandCancelled;
            fReplySlots[smid].smid = (UInt16)smid;
            fReplySlots[smid].kind = 3;
            fReplySlots[smid].valid = 1;
        } else {
            fCommandOwners[smid].state = kMPSCommandFree;
        }
    }
    fEventAckHead = fEventAckQueued = fEventAckAge = 0;
    fEventAckBusy = false;
    fProtocolRecoveryPending = false;
    fProtocolFaultCode = fProtocolFaultSmid = 0;
    fPendingRemovalHandle = 0;
    IOSimpleLockUnlock(fReplyLock);
    notifyReplyWaiters();
}

void
MPSController::serviceProtocolState(void)
{
    UInt32 queued, completed, failed, overflow, fault, smid, generation;
    bool busy, recovery;

    if (fReplyLock == NULL) {
        return;
    }
    IOSimpleLockLock(fReplyLock);
    if (!fReinitInProgress && !fStopping && fEventAckBusy &&
        !fProtocolRecoveryPending && ++fEventAckAge >= 5) {
        fEventAckFailed++;
        recordProtocolFaultLocked(kMPSProtocolAckTimeout, MPS_SM_EVENT_ACK);
    }
    queued = fEventAckQueued;
    completed = fEventAckCompleted;
    failed = fEventAckFailed;
    overflow = fEventAckOverflow;
    busy = fEventAckBusy;
    recovery = fProtocolRecoveryPending;
    fault = fProtocolFaultCode;
    smid = fProtocolFaultSmid;
    generation = fTopologyGeneration;
    IOSimpleLockUnlock(fReplyLock);
    MPS_DEBUG_PROP("MPSEventAckQueued", (unsigned long long)queued, 32);
    MPS_DEBUG_PROP("MPSEventAckInFlight", busy);
    MPS_DEBUG_PROP("MPSEventAckCompleted", (unsigned long long)completed, 32);
    MPS_DEBUG_PROP("MPSEventAckFailed", (unsigned long long)failed, 32);
    MPS_DEBUG_PROP("MPSEventAckOverflow", (unsigned long long)overflow, 32);
    MPS_DEBUG_PROP("MPSProtocolRecoveryPending", recovery);
    MPS_DEBUG_PROP("MPSProtocolFaultCode", (unsigned long long)fault, 32);
    MPS_DEBUG_PROP("MPSProtocolFaultSmid", (unsigned long long)smid, 16);
    MPS_DEBUG_PROP("MPSTopologyGeneration", (unsigned long long)generation, 32);
    MPS_DEBUG_PROP("MPSIntCount", (unsigned long long)fInterruptCount,32);
    MPS_DEBUG_PROP("MPSReplyViaIrq", (unsigned long long)fReplyViaIrq,32);
    MPS_DEBUG_PROP("MPSUnexpectedCompletionCount",
                (unsigned long long)fUnexpectedCompletionCount, 32);
    /* The members below are written under the reply lock, so they are
     * published here instead of allocating inside that lock. */
    MPS_DEBUG_PROP("MPSStickyFaultCode", (unsigned long long)fStickyFaultCode, 32);
    MPS_DEBUG_PROP("MPSStickyFaultSmid", (unsigned long long)fStickyFaultSmid, 16);
    MPS_DEBUG_PROP("MPSStickyFaultCount", (unsigned long long)fStickyFaultCount, 32);
    MPS_DEBUG_PROP("MPSLastTopoHandle", (unsigned long long)fLastTopoHandle, 16);
    MPS_DEBUG_PROP("MPSLastTopoReason", (unsigned long long)fLastTopoReason, 8);
    MPS_DEBUG_PROP("MPSLastTopoRate", (unsigned long long)fLastTopoRate, 8);
    MPS_DEBUG_PROP("MPSTopoRemovalHits", (unsigned long long)fTopoRemovalHits, 32);
    MPS_DEBUG_PROP("MPSBadReplySite", (unsigned long long)fBadReplySite, 32);
    MPS_DEBUG_PROP("MPSBadReplyValue", (unsigned long long)fBadReplyValue, 32);
    MPS_DEBUG_PROP("MPSBadReplySmid", (unsigned long long)fBadReplySmid, 16);
    MPS_DEBUG_PROP("MPSBadReplyCount", (unsigned long long)fBadReplyCount, 32);
    MPS_DEBUG_PROP("MPSBadReplyFunction", (unsigned long long)fBadReplyFunction, 8);
    MPS_DEBUG_PROP("MPSBadReplyMsgLen", (unsigned long long)fBadReplyMsgLen, 8);
    MPS_DEBUG_PROP("MPSBadReplyWord", (unsigned long long)fBadReplyWord, 32);
    MPS_DEBUG_PROP("MPSWriteCount", (unsigned long long)fWriteCount, 32);
    MPS_DEBUG_PROP("MPSWriteBytes", (unsigned long long)fWriteBytes, 64);
    MPS_DEBUG_PROP("MPSLastWriteLba", (unsigned long long)fLastWriteLba, 64);
    MPS_DEBUG_PROP("MPSLastWriteSmid", (unsigned long long)fLastWriteSmid, 16);
}

void
MPSController::recordTopologyEventLocked(const UInt8 *data, UInt32 bytes)
{
    /* Payload is borrowed from the reply frame and parsed before recycling.
     * The diagnostics-only fLastEventData is too short for eight PHYs. */
    if (bytes < 12 || data[8] > (bytes - 12) / 4) {
        recordProtocolFaultLocked(kMPSProtocolEventLength, 0);
        return;
    }
    bool changed = false;
    for (UInt32 i = 0; i < data[8]; i++) {
        const UInt8 *phy = data + 12 + i * 4;
        UInt16 handle = (UInt16)((UInt16)phy[0] | ((UInt16)phy[1] << 8));
        UInt8 reason = phy[3] & MPI2_EVENT_SAS_TOPO_RC_MASK;
        UInt8 rate = phy[2] >> 4;
        if (handle == 0 ||
            ((phy[3] & MPI2_EVENT_SAS_TOPO_PHYSTATUS_VACANT) != 0 &&
             reason != MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING)) {
            continue;
        }
        /* A change on the phy that carries our target means the link was
         * renegotiated, which only happens after it dropped. Treating that as
         * removal is what makes a replug shorter than one poll interval still
         * retire the old medium: the firmware may report only PHY_CHANGED and
         * never TARGET_NOT_RESPONDING, and the reattached target can keep the
         * same handle with an identical device page. */
        bool removed = reason == MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING ||
                       reason == MPI2_EVENT_SAS_TOPO_RC_PHY_CHANGED;
        /* Diagnostics: the last reason that carried a target handle decides
         * which transition the driver reacts to. Plain members only; the
         * watchdog publishes them outside this lock. */
        fLastTopoHandle = handle;
        fLastTopoReason = reason;
        fLastTopoRate = rate;
        if (mpsTopologyRequiresReprobe(reason)) {
            /* Preserve removal even if a fast replug reuses the same handle. */
            if (removed) fTopoRemovalHits++;
            fRemovalPending = true;
            fPhysicalChangePending = true;
        }
        if (reason == MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING) {
            /* Latch the handle only. The target table is owned by the hotplug
             * worker under fIOLock; this runs with fReplyLock held. */
            fPendingRemovalHandle = handle;
        } else if (handle == fPendingRemovalHandle &&
                   (reason == MPI2_EVENT_SAS_TOPO_RC_TARG_ADDED ||
                    (reason == MPI2_EVENT_SAS_TOPO_RC_PHY_CHANGED && rate >= 8))) {
            fPendingRemovalHandle = 0;
        }
        if (reason == MPI2_EVENT_SAS_TOPO_RC_TARG_ADDED &&
            !fReinitInProgress && !fStarting) {
            /* A new insertion may retry an exhausted recovery budget;
             * discovery generated by our own IOC reset must not do so. */
            fHotplugRetryRequested = true;
            fPhysicalChangePending = true;
            fRemovalPending = true;
        }
        if (reason == MPI2_EVENT_SAS_TOPO_RC_TARG_ADDED ||
            reason == MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING ||
            reason == MPI2_EVENT_SAS_TOPO_RC_PHY_CHANGED ||
            reason == MPI2_EVENT_SAS_TOPO_RC_DELAY_NOT_RESPONDING) {
            changed = true;
        }
    }
    if (changed) {
        fTopologyGeneration++;
        fTopologyChanged = true;
        /* Cooldown delays work; it must never discard a physical event. */
        fHotplugPending = true;
    }
}

IOService *
MPSController::probe(IOService *provider, SInt32 *score)
{
    mpsInitDebugOutput();
    IOPCIDevice         *pci;
    const MPSPciId      *id;
    UInt32               vendorDevice;

    if (super::probe(provider, score) == NULL) {
        return NULL;
    }

    pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci == NULL) {
        return NULL;
    }

    vendorDevice = pci->configRead32(0);
    id = mpsLookupPciId(vendorDevice);
    if (id == NULL) {
        MPS_DEBUG_LOG("MPS: probe rejected vendor/device 0x%08x\n",
              (unsigned int)vendorDevice);
        return NULL;
    }


    /* Which chip bound is worth knowing on the unverified entries, since the
     * rest of the diagnostics look identical on every supported part. */
    MPS_DEBUG_PROP("MPSChipName", id->name);
    MPS_DEBUG_LOG("MPS: probe matched %s (0x%08x), verified=%u\n", id->name,
          (unsigned int)vendorDevice, id->verified ? 1U : 0U);
    return this;
}

void
MPSController::dumpRegisters(void)
{
    unsigned int i;

    for (i = 0; i < sizeof(kMPSRegisters) / sizeof(kMPSRegisters[0]); i++) {
        UInt32 value = readReg(kMPSRegisters[i].offset);
        MPS_DEBUG_PROP(kMPSRegisters[i].name, (unsigned long long)value, 32);
    }

    MPS_DEBUG_LOG("MPS MMIO dump: Doorbell=0x%08x HostDiag=0x%08x HIS=0x%08x HIM=0x%08x\n",
          (unsigned int)readReg(MPI2_DOORBELL_OFFSET),
          (unsigned int)readReg(MPI2_HOST_DIAGNOSTIC_OFFSET),
          (unsigned int)readReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET),
          (unsigned int)readReg(MPI2_HOST_INTERRUPT_MASK_OFFSET));
}

bool
MPSController::waitDoorbellAck(UInt32 iterations)
{
    UInt32 i;

    for (i = 0; i < iterations; i++) {
        UInt32 status = readReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET);
        if (!(status & MPI2_HIS_SYS2IOC_DB_STATUS)) {
            return true;
        }
        if (status == 0xFFFFFFFFU) {
            return false;
        }
        if ((status & MPI2_HIS_IOC2SYS_DB_STATUS) &&
            ((readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK) ==
             MPI2_IOC_STATE_FAULT)) {
            return false;
        }
        IODelay(100);
    }
    return false;
}

bool
MPSController::waitDoorbellInt(UInt32 iterations)
{
    UInt32 i;

    for (i = 0; i < iterations; i++) {
        if (readReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET) &
            MPI2_HIS_IOC2SYS_DB_STATUS) {
            return true;
        }
        IODelay(100);
    }
    return false;
}

bool
MPSController::diagReset(void)
{
    static const UInt8 magic[7] = {
        MPI2_WRSEQ_FLUSH_KEY_VALUE, MPI2_WRSEQ_1ST_KEY_VALUE,
        MPI2_WRSEQ_2ND_KEY_VALUE,   MPI2_WRSEQ_3RD_KEY_VALUE,
        MPI2_WRSEQ_4TH_KEY_VALUE,   MPI2_WRSEQ_5TH_KEY_VALUE,
        MPI2_WRSEQ_6TH_KEY_VALUE
    };
    UInt32 reg;
    unsigned int i, tries;
    bool unlocked = false;

    /* Keep interrupts masked while the IOC is being reset; the reset path polls only. */
    writeReg(MPI2_HOST_INTERRUPT_MASK_OFFSET,
             MPI2_HIM_REPLY_INT_MASK | MPI2_HIM_IOC2SYS_DB_MASK);
    writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);

    for (tries = 0; tries < MPS_DIAG_UNLOCK_TRIES && !unlocked; tries++) {
        for (i = 0; i < sizeof(magic); i++) {
            writeReg(MPI2_WRITE_SEQUENCE_OFFSET, magic[i]);
        }
        IOSleep(100);
        if (readReg(MPI2_HOST_DIAGNOSTIC_OFFSET) &
            MPI2_DIAG_DIAG_WRITE_ENABLE) {
            unlocked = true;
        }
    }
    if (!unlocked) {
        MPS_DEBUG_LOG("MPS diag write-enable never set\n");
        return false;
    }

    reg = readReg(MPI2_HOST_DIAGNOSTIC_OFFSET);
    writeReg(MPI2_HOST_DIAGNOSTIC_OFFSET, reg | MPI2_DIAG_RESET_ADAPTER);

    IOSleep(256);
    for (tries = 0; tries < MPS_DIAG_SETTLE_TRIES; tries++) {
        reg = readReg(MPI2_HOST_DIAGNOSTIC_OFFSET);
        if (reg & MPI2_DIAG_RESET_ADAPTER) {
            IOSleep(50);
            continue;
        }
        if ((readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK) !=
            MPI2_IOC_STATE_RESET) {
            writeReg(MPI2_WRITE_SEQUENCE_OFFSET,
                     MPI2_WRSEQ_FLUSH_KEY_VALUE);
            return true;
        }
        IOSleep(50);
    }

    MPS_DEBUG_LOG("MPS diag reset did not leave RESET state\n");
    return false;
}

bool
MPSController::transitionToReady(void)
{
    unsigned int tries;

    for (tries = 0; tries < MPS_READY_TRIES; tries++) {
        UInt32 doorbell = readReg(MPI2_DOORBELL_OFFSET);
        UInt32 state = doorbell & MPI2_IOC_STATE_MASK;
        UInt32 whoInit = (doorbell & MPI2_DOORBELL_WHO_INIT_MASK) >>
                         MPI2_DOORBELL_WHO_INIT_SHIFT;

        if (whoInit == MPI2_WHOINIT_PCI_PEER) {
            MPS_DEBUG_LOG("MPS another PCI peer owns the IOC, refusing\n");
            return false;
        }

        if (state == MPI2_IOC_STATE_READY) {
            return true;
        }

        if (state == MPI2_IOC_STATE_OPERATIONAL) {
            /* Take ownership back without a full diagnostic reset. */
            writeReg(MPI2_DOORBELL_OFFSET,
                     MPI2_FUNCTION_IOC_MESSAGE_UNIT_RESET <<
                     MPI2_DOORBELL_FUNCTION_SHIFT);
            if (!waitDoorbellAck(MPS_ACK_ITERATIONS)) {
                return false;
            }
            IOSleep(50);
            continue;
        }

        if ((state == MPI2_IOC_STATE_FAULT) ||
            (doorbell & MPI2_DOORBELL_USED)) {
            if (!diagReset()) {
                return false;
            }
            continue;
        }

        /* RESET or an unknown state: let the firmware settle. */
        IOSleep(50);
    }

    MPS_DEBUG_LOG("MPS IOC never reached READY\n");
    return false;
}

/* Synchronous doorbell message transfer (mps.c:994-1123). The reply is
 * clocked in as 16-bit words; every word is followed by clearing HIS. */
bool
MPSController::handshake(const UInt32 *reqWords, UInt32 requestBytes,
                         UInt16 *repWords, UInt32 replyBytes)
{
    UInt32 reqCount = (requestBytes + 3) / 4;
    UInt32 maxWords = replyBytes / 2;
    UInt32 i, count, residual, iocWords;

    writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);
    if (readReg(MPI2_DOORBELL_OFFSET) & MPI2_DOORBELL_USED) {
        return false;
    }

    writeReg(MPI2_DOORBELL_OFFSET,
             (MPI2_FUNCTION_HANDSHAKE << MPI2_DOORBELL_FUNCTION_SHIFT) |
             (reqCount << MPI2_DOORBELL_ADD_DWORDS_SHIFT));

    if (!waitDoorbellInt(MPS_DBINT_ITERATIONS) ||
        !(readReg(MPI2_DOORBELL_OFFSET) & MPI2_DOORBELL_USED)) {
        return false;
    }
    writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);
    if (!waitDoorbellAck(MPS_ACK_ITERATIONS)) {
        return false;
    }

    for (i = 0; i < reqCount; i++) {
        writeReg(MPI2_DOORBELL_OFFSET, reqWords[i]);
        if (!waitDoorbellAck(MPS_ACK_ITERATIONS)) {
            return false;
        }
    }

    if (!waitDoorbellInt(MPS_DBINT_ITERATIONS)) {
        return false;
    }
    repWords[0] = (UInt16)(readReg(MPI2_DOORBELL_OFFSET) &
                           MPI2_DOORBELL_DATA_MASK);
    writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);

    if (!waitDoorbellInt(MPS_DBINT_ITERATIONS)) {
        return false;
    }
    repWords[1] = (UInt16)(readReg(MPI2_DOORBELL_OFFSET) &
                           MPI2_DOORBELL_DATA_MASK);
    writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);

    /* MsgLength lives in byte 2 of the reply and counts 32-bit words. */
    iocWords = ((UInt8 *)repWords)[MPI2_DEFAULT_REPLY_MSG_LENGTH_OFF];
    if (iocWords == 0) {
        return false;
    }
    count = iocWords * 2;
    if (count > maxWords) {
        residual = count - maxWords;
        count = maxWords;
    } else {
        residual = 0;
    }

    for (i = 2; i < count; i++) {
        if (!waitDoorbellInt(MPS_DBINT_ITERATIONS)) {
            return false;
        }
        repWords[i] = (UInt16)(readReg(MPI2_DOORBELL_OFFSET) &
                               MPI2_DOORBELL_DATA_MASK);
        writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);
    }

    while (residual--) {
        if (!waitDoorbellInt(MPS_DBINT_ITERATIONS)) {
            return false;
        }
        (void)readReg(MPI2_DOORBELL_OFFSET);
        writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);
    }

    if (!waitDoorbellInt(MPS_DBINT_ITERATIONS)) {
        return false;
    }
    writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET, 0);
    return true;
}

bool
MPSController::getIOCFacts(void)
{
    union {
        MPSIOCFactsRequest req;
        UInt32             words[sizeof(MPSIOCFactsRequest) / 4];
    } request;
    union {
        MPSIOCFactsReply reply;
        UInt16           words[sizeof(MPSIOCFactsReply) / 2];
    } answer;
    MPSIOCFactsReply   facts;
    unsigned int       retry;
    bool               ok = false;

    bzero(&request, sizeof(request));
    request.req.Function = MPI2_FUNCTION_IOC_FACTS;

    for (retry = 0; retry < 3 && !ok; retry++) {
        bzero(&answer, sizeof(answer));
        ok = handshake(request.words, sizeof(request.words),
                       answer.words, sizeof(answer.words));
        if (!ok) {
            IODelay(1000);
        }
    }
    if (!ok) {
        MPS_DEBUG_PROP("MPSControllerInitFailure", "IOC Facts handshake failed");
        return false;
    }

    /* The doorbell reply is little-endian 16-bit words; x86_64 needs no swap,
     * but copy through the union so the packed structure stays aligned. */
    memcpy(&facts, answer.words, sizeof(facts));
    bzero(fIocFactsSnapshot, sizeof(fIocFactsSnapshot));
    memcpy(fIocFactsSnapshot, &facts, sizeof(facts) < sizeof(fIocFactsSnapshot)
        ? sizeof(facts) : sizeof(fIocFactsSnapshot));

    MPS_DEBUG_PROP("MPSFactsMsgVersion",        (unsigned long long)facts.MsgVersion, 16);
    MPS_DEBUG_PROP("MPSFactsMsgLength",         (unsigned long long)facts.MsgLength, 8);
    MPS_DEBUG_PROP("MPSFactsHeaderVersion",     (unsigned long long)facts.HeaderVersion, 16);
    MPS_DEBUG_PROP("MPSFactsIOCStatus",         (unsigned long long)facts.IOCStatus, 16);
    MPS_DEBUG_PROP("MPSFactsIOCLogInfo",        (unsigned long long)facts.IOCLogInfo, 32);
    MPS_DEBUG_PROP("MPSFactsWhoInit",           (unsigned long long)facts.WhoInit, 8);
    MPS_DEBUG_PROP("MPSFactsMaxChainDepth",     (unsigned long long)facts.MaxChainDepth, 8);
    MPS_DEBUG_PROP("MPSFactsMaxMSIxVectors",    (unsigned long long)facts.MaxMSIxVectors, 8);
    MPS_DEBUG_PROP("MPSFactsNumberOfPorts",     (unsigned long long)facts.NumberOfPorts, 8);
    MPS_DEBUG_PROP("MPSFactsRequestCredit",     (unsigned long long)facts.RequestCredit, 16);
    MPS_DEBUG_PROP("MPSFactsProductID",         (unsigned long long)facts.ProductID, 16);
    MPS_DEBUG_PROP("MPSFactsIOCCapabilities",   (unsigned long long)facts.IOCCapabilities, 32);
    MPS_DEBUG_PROP("MPSFactsFWVersion",         (unsigned long long)facts.FWVersion, 32);
    MPS_DEBUG_PROP("MPSFactsRequestFrameSize",  (unsigned long long)facts.IOCRequestFrameSize, 16);
    MPS_DEBUG_PROP("MPSFactsMaxReplyPostDepth", (unsigned long long)facts.MaxReplyDescriptorPostQueueDepth, 16);
    MPS_DEBUG_PROP("MPSFactsReplyFrameSize",    (unsigned long long)facts.ReplyFrameSize, 8);
    MPS_DEBUG_PROP("MPSFactsMaxDevHandle",      (unsigned long long)facts.MaxDevHandle, 16);
    MPS_DEBUG_PROP("MPSFactsProtocolFlags",     (unsigned long long)facts.ProtocolFlags, 16);
    MPS_DEBUG_PROP("MPSFactsHighPriorityCredit",(unsigned long long)facts.HighPriorityCredit, 16);

    MPS_DEBUG_PROP("MPSFactsFunction", (unsigned long long)facts.Function, 8);
    MPS_DEBUG_PROP("MPSFactsStatus",
                (unsigned long long)(facts.IOCStatus & MPI2_IOCSTATUS_MASK), 16);

    MPS_DEBUG_LOG("MPS IOC Facts: status=0x%04x FW=0x%08x credit=%u replyDepth=%u frameSize=%u\n",
          (unsigned int)(facts.IOCStatus & MPI2_IOCSTATUS_MASK),
          (unsigned int)facts.FWVersion,
          (unsigned int)facts.RequestCredit,
          (unsigned int)facts.MaxReplyDescriptorPostQueueDepth,
          (unsigned int)facts.IOCRequestFrameSize);

    ok = mpsReplyHeaderValid((const UInt8 *)&facts,sizeof(facts),
                              MPI2_FUNCTION_IOC_FACTS,sizeof(facts)) &&
         ((facts.IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS);
    if (ok && (!mpsFactsVersionValid(facts.MsgVersion))) {
        MPS_DEBUG_LOG("MPS: unsupported MPI version 0x%04x (supported minimum 0x%04x)\n",
                      facts.MsgVersion, MPI2_VERSION);
        MPS_DEBUG_PROP("MPSControllerInitFailure", "unsupported-MPI-version");
        return false;
    }
    if (ok) {
        fMpiVersion = facts.MsgVersion;
        MPS_DEBUG_PROP("MPSNegotiatedMPI", (unsigned long long)fMpiVersion, 16);
        /* Keep the geometry for queue initialization; both frame sizes are in 32-bit words. */
        fRequestFrameSize = (UInt32)facts.IOCRequestFrameSize * 4;
        fReplyFrameSize   = (UInt32)facts.ReplyFrameSize * 4;
        fRequestCredit      = facts.RequestCredit;
        fMaxReplyDepth      = facts.MaxReplyDescriptorPostQueueDepth;
        fHighPriorityCredit = facts.HighPriorityCredit;
        fMaxChainDepth      = facts.MaxChainDepth;
        /* MPI 2.0 uses request-sized chain frames; 0x26 in Facts is reserved. */
        fChainFrameBytes = fRequestFrameSize;
        fIocCapabilities    = facts.IOCCapabilities;
        fMaxDeviceHandle    = facts.MaxDevHandle;
        fFactsValid         = true;
    }
    return ok;
}

bool
MPSController::initializeController(void)
{
    UInt32 initialState, finalState;

    dumpRegisters();
    initialState = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
    MPS_DEBUG_PROP("MPSInitialIOCState", (unsigned long long)initialState, 32);

    if (!transitionToReady()) {
        MPS_DEBUG_PROP("MPSControllerInitFailure", "cannot reach READY");
        MPS_DEBUG_PROP("MPSControllerInitState", "failed-ready");
        return false;
    }
    MPS_DEBUG_PROP("MPSReachedReady", true);

    if (!getIOCFacts()) {
        MPS_DEBUG_PROP("MPSControllerInitState", "failed-facts");
        return false;
    }

    if (!(fIocCapabilities & MPI2_IOCFACTS_CAPABILITY_EVENT_REPLAY)) {
        MPS_DEBUG_PROP("MPSInitialReplayReset", true);
        if (!diagReset() || !transitionToReady() || !getIOCFacts()) return false;
    }

    finalState = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
    MPS_DEBUG_PROP("MPSFinalIOCState", (unsigned long long)finalState, 32);
    MPS_DEBUG_PROP("MPSControllerInitState", "ok");
    MPS_DEBUG_LOG("MPS completed, IOC state after IOC Facts = 0x%08x\n",
          (unsigned int)finalState);
    return true;
}

/* ------------------------------------------------------------------------ */
/* synchronous IOC Init with a low-4GB, physically contiguous pool.    */
/* No interrupts, no config pages and no I/O: the only hardware effects are  */
/* the IOC Init message and the two reply-index registers (mps.c:1231-1289). */

static UInt32
mpsRoundUp16(UInt32 value)
{
    return (value + 15U) & ~15U;
}

bool
MPSController::allocateQueueMemory(void)
{
    UInt32      queueBytes, replyBytes, requestBytes;
    UInt8      *queue;
    UInt32     *freeQueue;
    UInt64     *postQueue;
    UInt32      i;

    if (!fFactsValid || fReplyFrameSize < MPS_SCSIIO_REPLY_MIN_BYTES ||
        fRequestFrameSize < sizeof(MPSScsiIoRequest) + 12) {
        MPS_DEBUG_PROP("MPSQueueInitFailure", "no usable IOC Facts geometry");
        return false;
    }

    fFqDepth        = mpsRoundUp16(MPS_REPLY_FRAMES + 1);
    if (fFqDepth > fMaxReplyDepth) fFqDepth = fMaxReplyDepth & ~15U;
    fPqDepth        = fFqDepth;
    fRequestCount   = fRequestCredit < MPS_REQUEST_FRAMES
        ? fRequestCredit : MPS_REQUEST_FRAMES;
    if (fRequestCount <= MPS_SM_EVENT_ACK || fFqDepth < 16) {
        MPS_DEBUG_PROP("MPSQueueInitFailure", "insufficient management and IO credits");
        return false;
    }
    fReplyFreeIndex = fFqDepth - 1 < MPS_REPLY_FRAMES
        ? fFqDepth - 1 : MPS_REPLY_FRAMES;

    queueBytes   = fFqDepth * 4 + fPqDepth * 8;
    replyBytes   = fReplyFrameSize * fFqDepth;
    requestBytes = fRequestFrameSize * fRequestCount;
    const UInt32 requestPoolBytes = requestBytes;

    if (fRequestCount > fRequestCredit || fFqDepth > fMaxReplyDepth) {
        MPS_DEBUG_PROP("MPSQueueInitFailure", "geometry exceeds IOC credit");
        return false;
    }

    fQueueBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
        queueBytes, MPS_DMA_LOW_MASK);
    fReplyBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
        replyBytes, MPS_DMA_LOW_MASK);
    fRequestBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
        requestPoolBytes, MPS_DMA_LOW_MASK);
    fSenseBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
        MPS_SENSE_POOL_BYTES, MPS_DMA_LOW_MASK);

    if (fQueueBuffer == NULL || fReplyBuffer == NULL || fRequestBuffer == NULL ||
        fSenseBuffer == NULL) {
        MPS_DEBUG_PROP("MPSQueueInitFailure", "DMA pool allocation failed");
        return false;
    }

    if (!mpsMapResidentDma(fQueueDma, fQueueBuffer, fDmaMapper, queueBytes, 16) ||
        !mpsMapResidentDma(fReplyDma, fReplyBuffer, fDmaMapper, replyBytes, 16) ||
        !mpsMapResidentDma(fRequestDma, fRequestBuffer, fDmaMapper, requestPoolBytes, 16) ||
        !mpsMapResidentDma(fSenseDma, fSenseBuffer, fDmaMapper, MPS_SENSE_POOL_BYTES, 16)) {
        MPS_DEBUG_PROP("MPSQueueInitFailure", "resident-DMA-mapping-failed");
        return false;
    }
    fFreePhys = fQueueDma.address;
    fPostPhys = fFreePhys + (UInt64)fFqDepth * 4;
    fReplyPhys = fReplyDma.address;
    fRequestPhys = fRequestDma.address;
    fSensePhys = fSenseDma.address;
    if (fSenseBuffer->getBytesNoCopy() == NULL) {
        MPS_DEBUG_PROP("MPSQueueInitFailure", "sense pool violates DMA address bounds");
        return false;
    }
    memset(fSenseBuffer->getBytesNoCopy(), 0, MPS_SENSE_POOL_BYTES);

    if ((fFreePhys + queueBytes) > (UINT64_C(0x100000000)) ||
        (fReplyPhys + replyBytes) > (UINT64_C(0x100000000)) ||
        (fRequestPhys + requestPoolBytes) > (UINT64_C(0x100000000))) {
        MPS_DEBUG_PROP("MPSQueueInitFailure", "DMA pool crosses the 4 GB boundary");
        return false;
    }

    queue = (UInt8 *)fQueueBuffer->getBytesNoCopy();
    if (queue == NULL) {
        MPS_DEBUG_PROP("MPSQueueInitFailure", "queue pool has no virtual address");
        return false;
    }

    memset(queue, 0, queueBytes);
    if (fReplyBuffer->getBytesNoCopy() != NULL) {
        memset(fReplyBuffer->getBytesNoCopy(), 0, replyBytes);
    }
    if (fRequestBuffer->getBytesNoCopy() != NULL) {
        memset(fRequestBuffer->getBytesNoCopy(), 0, requestPoolBytes);
    }
    freeQueue = (UInt32 *)queue;
    postQueue = (UInt64 *)(queue + fFqDepth * 4);

    /* Unused post entries are 0xff; each free entry carries the low 32 bits
     * of its reply frame address (mps.c:1691-1706). */
    memset(postQueue, 0xFF, fPqDepth * 8);
    for (i = 0; i < fFqDepth; i++) {
        freeQueue[i] = (UInt32)(fReplyPhys + (UInt64)i * fReplyFrameSize);
    }

    MPS_DEBUG_PROP("MPSQueueFreeDepth", (unsigned long long)fFqDepth, 32);
    MPS_DEBUG_PROP("MPSQueuePostDepth", (unsigned long long)fPqDepth, 32);
    MPS_DEBUG_PROP("MPSRequestCount",   (unsigned long long)fRequestCount, 32);
    MPS_DEBUG_PROP("MPSQueuePhys",      fFreePhys, 64);
    MPS_DEBUG_PROP("MPSReplyPhys",      fReplyPhys, 64);
    MPS_DEBUG_PROP("MPSRequestPhys",    fRequestPhys, 64);
    return true;
}

void
MPSController::freeQueueMemory(void)
{
    /* Callers have proved IOC DMA stopped, or no queue was ever posted. */
    mpsReleaseResidentDma(fSenseDma);
    mpsReleaseResidentDma(fQueueDma);
    mpsReleaseResidentDma(fReplyDma);
    mpsReleaseResidentDma(fRequestDma);
    if (fSenseBuffer != NULL) {
        fSenseBuffer->release();
        fSenseBuffer = NULL;
    }
    fSensePhys = 0;
    if (fQueueBuffer != NULL) {
        fQueueBuffer->release();
        fQueueBuffer = NULL;
    }
    if (fReplyBuffer != NULL) {
        fReplyBuffer->release();
        fReplyBuffer = NULL;
    }
    if (fRequestBuffer != NULL) {
        fRequestBuffer->release();
        fRequestBuffer = NULL;
    }
    fFreePhys = fPostPhys = fReplyPhys = fRequestPhys = 0;
}

bool
MPSController::sendIOCInit(void)
{
    union {
        MPSIOCInitRequest request;
        UInt32             words[sizeof(MPSIOCInitRequest) / 4];
    } out;
    union {
        MPSIOCInitReply reply;
        UInt16          words[sizeof(MPSIOCInitReply) / 2];
    } in;
    MPSIOCInitReply answer;
    UInt16          status;
    unsigned int    retry;
    bool            ok = false;

    bzero(&out, sizeof(out));
    out.request.WhoInit                         = MPI2_WHOINIT_HOST_DRIVER;
    out.request.Function                        = MPI2_FUNCTION_IOC_INIT;
    out.request.MsgVersion                      = fMpiVersion;
    out.request.HeaderVersion                   = MPI2_HEADER_VERSION;
    out.request.SystemRequestFrameSize          = (UInt16)(fRequestFrameSize / 4);
    out.request.ReplyDescriptorPostQueueDepth   = (UInt16)fPqDepth;
    out.request.ReplyFreeQueueDepth             = (UInt16)fFqDepth;
    out.request.SenseBufferAddressHigh          = (UInt32)(fSensePhys >> 32);
    out.request.SystemReplyAddressHigh          = 0;
    out.request.SystemRequestFrameBaseAddress   = fRequestPhys;
    out.request.ReplyDescriptorPostQueueAddress = fPostPhys;
    out.request.ReplyFreeQueueAddress           = fFreePhys;
    out.request.HostMSIxVectors                 = (fInterrupt != NULL) ? 1 : 0;
    out.request.TimeStamp                       = 0;

    for (retry = 0; retry < 3 && !ok; retry++) {
        bzero(&in, sizeof(in));
        ok = handshake(out.words, sizeof(out.words),
                       in.words, sizeof(in.words));
        if (!ok) {
            IODelay(1000);
        }
    }
    if (!ok) {
        MPS_DEBUG_PROP("MPSQueueInitFailure", "IOC Init handshake failed");
        return false;
    }

    memcpy(&answer, in.words, sizeof(answer));
    status = (UInt16)(answer.IOCStatus & MPI2_IOCSTATUS_MASK);
    MPS_DEBUG_PROP("MPSIOCInitStatus",  (unsigned long long)status, 16);
    MPS_DEBUG_PROP("MPSIOCInitLogInfo", (unsigned long long)answer.IOCLogInfo, 32);
    MPS_DEBUG_LOG("MPS IOC Init status=0x%04x loginfo=0x%08x\n",
          (unsigned int)status, (unsigned int)answer.IOCLogInfo);

    return mpsReplyHeaderValid((const UInt8 *)&answer,sizeof(answer),
                               MPI2_FUNCTION_IOC_INIT,sizeof(answer)) &&
           answer.WhoInit == MPI2_WHOINIT_HOST_DRIVER &&
           status == MPI2_IOCSTATUS_SUCCESS;
}

bool
MPSController::initializeQueues(void)
{
    UInt32 state = MPI2_IOC_STATE_RESET;
    UInt32 tries;

    if (!allocateQueueMemory()) {
        MPS_DEBUG_PROP("MPSQueueInitState", "failed-alloc");
        return false;
    }
    if (!configureAsyncLimits()) {
        MPS_DEBUG_PROP("MPSQueueInitState", "insufficient-async-credits");
        return false;
    }
    if (!sendIOCInit()) {
        MPS_DEBUG_PROP("MPSQueueInitState", "failed-iocinit");
        return false;
    }

    state = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
    for (tries = 0; tries < MPS_IOC_STATE_TRIES &&
                    state != MPI2_IOC_STATE_OPERATIONAL; tries++) {
        IODelay(1000);
        state = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
    }
    MPS_DEBUG_PROP("MPSIOCState", (unsigned long long)state, 32);

    if (state != MPI2_IOC_STATE_OPERATIONAL) {
        MPS_DEBUG_PROP("MPSQueueInitState", "failed-not-operational");
        return false;
    }

    /* The IOC resets these indices during the state transition, so program
     * them only after OPERATIONAL is observed (mps.c:700-712). */
    writeReg(MPI2_REPLY_FREE_HOST_INDEX_OFFSET, fReplyFreeIndex);
    writeReg(MPI2_REPLY_POST_HOST_INDEX_OFFSET, 0);
    MPS_DEBUG_PROP("MPSReplyFreeHostIndex", (unsigned long long)fReplyFreeIndex, 32);
    MPS_DEBUG_PROP("MPSReplyPostHostIndex", 0ULL, 32);

    /* Unmask only the reply-descriptor interrupt; every other cause stays
     * masked so a stuck status bit cannot become an interrupt storm. */
    if (fInterrupt != NULL) {
        writeReg(MPI2_HOST_INTERRUPT_MASK_OFFSET, ~MPI2_HIM_REPLY_INT_MASK);
    }
    MPS_DEBUG_PROP("MPSQueueInitState", "ok");
    MPS_DEBUG_LOG("MPS IOC OPERATIONAL, freeDepth=%u postDepth=%u requests=%u\n",
          (unsigned int)fFqDepth, (unsigned int)fPqDepth,
          (unsigned int)fRequestCount);
    return true;
}

/* ------------------------------------------------------------------------ */
/* one synchronous config request (Manufacturing Page0) submitted      */
/* through the request queue and completed by polling the reply queue.       */

static void
mpsCopyField(char *dst, const UInt8 *src, UInt32 length)
{
    UInt32 i = length;

    memcpy(dst, src, length);
    dst[length] = '\0';
    while (i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\0')) {
        dst[--i] = '\0';
    }
}

/* ------------------------------------------------------------------------ */
/* reply-descriptor interrupt delivered through a filtered event        */
/* source. The filter only samples and clears the cause; the reply queue is   */
/* drained on the work loop (PLAN section 5, rule 4).                        */

bool
MPSController::setupInterrupts(void)
{
    int type = -1;

    if (fWorkLoop == NULL) {
        fWorkLoop = IOWorkLoop::workLoop();
    }
    if (fWorkLoop == NULL) {
        MPS_DEBUG_PROP("MPSIntState", "no-workloop");
        return false;
    }

    if (fPCIDevice->getInterruptType(0, &type) == kIOReturnSuccess) {
        MPS_DEBUG_PROP("MPSIntType", (unsigned long long)type, 32);
    } else {
        MPS_DEBUG_PROP("MPSIntType", "unavailable");
    }

    fInterrupt = IOFilterInterruptEventSource::filterInterruptEventSource(
        this, interruptAction, interruptFilter, fPCIDevice, 0);
    if (fInterrupt == NULL) {
        MPS_DEBUG_PROP("MPSIntState", "no-source");
        return false;
    }
    if (fWorkLoop->addEventSource(fInterrupt) != kIOReturnSuccess) {
        MPS_DEBUG_PROP("MPSIntState", "add-failed");
        fInterrupt->release();
        fInterrupt = NULL;
        return false;
    }
    fInterrupt->enable();
    MPS_DEBUG_PROP("MPSIntState", "ok");
    return true;
}

void
MPSController::teardownInterrupts(void)
{
    if (fInterrupt != NULL) {
        fInterrupt->disable();
        if (fWorkLoop != NULL) {
            fWorkLoop->removeEventSource(fInterrupt);
        }
        fInterrupt->release();
        fInterrupt = NULL;
    }
    /* The watchdog shares this workloop. Keep it alive across IOC resets. */
}

bool
MPSController::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *sender)
{
    MPSController *self = (MPSController *)owner;
    UInt32 status;

    if (self == NULL || self->fRegs == NULL) {
        return false;
    }
    status = self->readReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET);
    if ((status & MPI2_HIS_REPLY_DESCRIPTOR_INTERRUPT) == 0) {
        return false;
    }
    /* HostInterruptStatus is write-one-to-clear. */
    self->writeReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET,
                   MPI2_HIS_REPLY_DESCRIPTOR_INTERRUPT);
    self->fInterruptCount++;
    return true;
}

void
MPSController::interruptAction(OSObject *owner, IOInterruptEventSource *sender,
                               int index)
{
    MPSController *self = (MPSController *)owner;

    if (self != NULL) {
        self->processReplyQueue(true);
    }
}

void
MPSController::processReplyQueue(bool fromInterrupt)
{
    if (fQueueBuffer == NULL || fReplyBuffer == NULL || fReplyLock == NULL) {
        return;
    }
    UInt8 *queue = (UInt8 *)fQueueBuffer->getBytesNoCopy();
    UInt8 *replyBase = (UInt8 *)fReplyBuffer->getBytesNoCopy();
    if (queue == NULL || replyBase == NULL || fReplyFrameSize == 0) {
        return;
    }
    UInt8 *post = queue + fFqDepth * 4;
    UInt32 replyBytes = fReplyFrameSize * fFqDepth;

    /* Like the reference reply-queue threshold, publish progress in bounded
     * batches. There is still exactly one consumer per hardware ring; this
     * does not claim MSI-X/multiple reply queues. Release the ownership lock
     * between batches so submitters and completion readers can run. */
    UInt32 remaining = fPqDepth;
    bool any = false;
    while (remaining != 0) {
        UInt32 consumed = 0;
        const UInt32 budget = remaining < kMPSReplyBatchBudget ?
                              remaining : kMPSReplyBatchBudget;
        IOSimpleLockLock(fReplyLock);
        for (UInt32 i = 0; i < budget; i++) {
            UInt64 entry;
            memcpy(&entry, post + fReplyPostIndex * 8, sizeof(entry));
            UInt8 flags = (UInt8)(entry & MPI2_RPY_DESCRIPT_FLAGS_TYPE_MASK);
            if (flags == MPI2_RPY_DESCRIPT_FLAGS_UNUSED ||
                ((entry >> 32) & 0xFFFFFFFFULL) == 0xFFFFFFFFULL) {
                break;
            }
            UInt16 smid = (UInt16)((entry >> 16) & 0xFFFF);
            UInt32 baddr = (UInt32)(entry >> 32);
            UInt64 clear = 0xFFFFFFFFFFFFFFFFULL;
            memcpy(post + fReplyPostIndex * 8, &clear, sizeof(clear));
            fReplyPostIndex = (fReplyPostIndex + 1 == fPqDepth) ? 0 : fReplyPostIndex + 1;
            ++consumed;
            if (flags == MPI2_RPY_DESCRIPT_FLAGS_ADDRESS_REPLY &&
                baddr >= (UInt32)fReplyPhys &&
                baddr - (UInt32)fReplyPhys < replyBytes &&
                ((baddr - (UInt32)fReplyPhys) % fReplyFrameSize) == 0) {
                UInt8 *reply = replyBase + (baddr - (UInt32)fReplyPhys);
                UInt32 bytes = (UInt32)reply[2] * 4;
                if (bytes < 4 || bytes > fReplyFrameSize) {
                    recordProtocolFaultLocked(kMPSProtocolManagementReply, smid);
                } else if (smid == MPS_SM_EVENT_ACK) {
                    /* ACK completion never satisfies a config/SCSI waiter. */
                    completeEventAckLocked(reply, bytes);
                } else if (reply[3] == MPI2_FUNCTION_EVENT_NOTIFICATION) {
                    if (bytes < sizeof(MPSEventNotifyReply)) {
                        recordProtocolFaultLocked(kMPSProtocolEventLength, smid);
                    } else {
                        const MPSEventNotifyReply *event =
                            (const MPSEventNotifyReply *)reply;
                        UInt32 eventBytes = (UInt32)event->EventDataLength * 4;
                        memset(fLastEventData, 0, sizeof(fLastEventData));
                        memcpy(fLastEventData, reply,
                               bytes < sizeof(fLastEventData) ? bytes : sizeof(fLastEventData));
                        fLastEvent = event->Event;
                        fLastEventDataLength = event->EventDataLength;
                        fLastEventAckRequired = event->AckRequired;
                        fLastEventContext = event->EventContext;
                        if (event->Event < 128) {
                            fEventSeen[event->Event / 32] |= 1U << (event->Event % 32);
                        }
                        fEventCount++;
                        fEventReplyCount++;
                        if (smid == 2 && fManagementBusy[2]) {
                            /* Only the subscription's own SMID completes it. */
                            storeCommandReplyLocked(2, 1, reply, bytes);
                        } else if (smid != 0) {
                            recordProtocolFaultLocked(kMPSProtocolUnexpectedCompletion, smid);
                        }
                        if (eventBytes <= bytes - sizeof(MPSEventNotifyReply))
                            recordControlEventLocked(event->Event, event->EventContext,
                                reply + sizeof(MPSEventNotifyReply), eventBytes);
                        if (eventBytes > bytes - sizeof(MPSEventNotifyReply)) {
                            recordProtocolFaultLocked(kMPSProtocolEventLength, smid);
                        } else if (event->Event == MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST &&
                                   (event->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS) {
                            recordTopologyEventLocked(reply + sizeof(MPSEventNotifyReply),
                                                      eventBytes);
                        } else if ((event->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS &&
                                   (event->Event == MPI2_EVENT_IR_CONFIGURATION_CHANGE_LIST ||
                                    event->Event == MPI2_EVENT_IR_PHYSICAL_DISK ||
                                    event->Event == MPI2_EVENT_IR_VOLUME)) {
                            // IR changes invalidate publication before the next policy scan.
                            if (fMediaPresent) fRemovalPending = true;
                            fPendingRemovalHandle = 0;
                            fTopologyGeneration++;
                            fTopologyChanged = true;
                            fHotplugPending = true;
                        }
                        /* DEVICE_STATUS_CHANGE also reports internal reset/TMF
                         * completion and SMART. It is not a device add/remove. */
                        if (event->AckRequired == MPI2_EVENT_NOTIFICATION_ACK_REQUIRED) {
                            sendEventAck(event->Event, event->EventContext);
                        }
                    }
                } else if (smid == 0 && reply[3] == MPI2_FUNCTION_DIAG_BUFFER_POST) {
                    releaseDiagnosticEventLocked(reply, bytes);
                } else {
                    storeCommandReplyLocked(smid, 1, reply, bytes);
                    memset(fLastReply, 0, sizeof(fLastReply));
                    memcpy(fLastReply, reply,
                           bytes < sizeof(fLastReply) ? bytes : sizeof(fLastReply));
                    fLastReplySmid = smid;
                    fLastReplyKind = 1;
                    fReplyCount++;
                }
                releaseReplyFrame(baddr);
            } else if (flags == MPI2_RPY_DESCRIPT_FLAGS_SCSI_IO_SUCCESS) {
                if (smid == MPS_SM_EVENT_ACK) {
                    recordProtocolFaultLocked(kMPSProtocolAckReply, smid);
                    fEventAckFailed++;
                } else {
                    storeCommandReplyLocked(smid, 2, NULL, 0);
                    fLastReplySmid = smid;
                    fLastReplyKind = 2;
                    memset(fLastReply, 0, sizeof(fLastReply));
                    fScsiSuccess++;
                    fReplyCount++;
                }
            } else {
                noteBadReply(1, (UInt32)flags, smid, NULL, 0);
                recordProtocolFaultLocked(kMPSProtocolManagementReply, smid);
            }
        }
        /* The index and its MMIO publication share the same owner, preventing
         * an older index from being published after another consumer advances. */
        if (consumed != 0) {
            writeReg(MPI2_REPLY_POST_HOST_INDEX_OFFSET, fReplyPostIndex);
            ++fReplyIndexWrites;
            ++fReplyBatches;
            fReplyDescriptors += consumed;
            if (fromInterrupt) fReplyViaIrq += consumed;
            if (consumed > fReplyMaxBatch) fReplyMaxBatch = consumed;
            any = true;
        }
        IOSimpleLockUnlock(fReplyLock);
        remaining -= consumed;
        if (consumed < budget) break;
    }
    if (any) notifyReplyWaiters();
}



UInt64
MPSController::uptimeMilliseconds(void)
{
    AbsoluteTime uptime;
    UInt64 nanoseconds = 0;
    clock_get_uptime(&uptime);
    absolutetime_to_nanoseconds(uptime, &nanoseconds);
    return nanoseconds / 1000000ULL;
}

UInt32
MPSController::replyWakeSequence(void)
{
    if (fReplyWaitLock == NULL) return 0;
    IOLockLock(fReplyWaitLock);
    UInt32 sequence = fReplyWakeSequence;
    IOLockUnlock(fReplyWaitLock);
    return sequence;
}

void
MPSController::notifyReplyWaiters(void)
{
    /* Called only after releasing every simple lock, never in the filter. */
    if (fReplyWaitLock == NULL) return;
    IOLockLock(fReplyWaitLock);
    ++fReplyWakeSequence;
    IOLockWakeup(fReplyWaitLock, &fReplyWakeSequence, false);
    IOLockUnlock(fReplyWaitLock);
}

void
MPSController::waitForReplyActivity(UInt32 sequence, UInt32 timeoutMs)
{
    if (fReplyWaitLock == NULL || timeoutMs == 0) return;
    AbsoluteTime deadline;
    clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
    IOLockLock(fReplyWaitLock);
    if (sequence == fReplyWakeSequence && !fStopping)
        IOLockSleepDeadline(fReplyWaitLock, &fReplyWakeSequence,
                            deadline, THREAD_UNINT);
    IOLockUnlock(fReplyWaitLock);
}

bool
MPSController::waitForSmid(UInt32 timeoutMs, UInt32 smid)
{
    bool ready;

    fTimedOutSmid = 0;
    if (smid == 0 || smid >= MPS_REQUEST_FRAMES || fReplyLock == NULL) {
        return false;
    }
    /* Fault injection: with mps_timeout_test=1 in boot-args, fail exactly one
     * SCSI command as if its reply were lost. It must land after bring-up:
     * a timeout while fStarting escalates to the bounded reset by design and
     * would consume the injection without exercising the TMF cascade. */
    if (!fStarting && ((smid >= 10 && smid <= 25) || mpsIsAsyncSmid(smid)) &&
        (fInjectTimeout || fInjectEvery != 0)) {
        bool inject = false;

        IOSimpleLockLock(fReplyLock);
        bool eligible = fCommandOwners[smid].epoch == fTransportEpoch &&
            (fCommandOwners[smid].state == kMPSCommandPosted ||
             fCommandOwners[smid].state == kMPSCommandComplete);
        if (eligible && fInjectTimeout) {
            fInjectTimeout = false;
            inject = true;
        } else if (eligible && fInjectEvery != 0) {
            if (++fInjectCounter >= fInjectEvery) {
                fInjectCounter = 0;
                inject = true;
            }
        }
        if (inject) fInjectCount++;
        IOSimpleLockUnlock(fReplyLock);

        if (inject) {
            MPS_DEBUG_PROP("MPSInjectTimeout", 0ULL, 8);
            MPS_DEBUG_PROP("MPSInjectCount", (unsigned long long)fInjectCount, 32);
            fTimedOutSmid = (UInt16)smid;
            return false;
        }
    }
    const UInt64 deadline = uptimeMilliseconds() + timeoutMs;
    for (;;) {
        const UInt32 sequence = replyWakeSequence();
        /* A missing interrupt must not strand management replies or ACKs.
         * Empty drains no longer write the reply-post MMIO register. */
        processReplyQueue();
        IOSimpleLockLock(fReplyLock);
        ready = (fReplySlots[smid].valid != 0);
        bool inactive = fCommandOwners[smid].state == kMPSCommandFree ||
                        fCommandOwners[smid].state == kMPSCommandReserved;
        bool protocolFault = fProtocolRecoveryPending;
        IOSimpleLockUnlock(fReplyLock);
        if (ready) return true;
        if (fStopping || inactive || protocolFault) {
            fTimedOutSmid = inactive ? 0 : (UInt16)smid;
            return false;
        }
        const UInt64 now = uptimeMilliseconds();
        if (now >= deadline) break;
        const UInt64 remaining = deadline - now;
        waitForReplyActivity(sequence, (UInt32)(remaining < 20 ? remaining : 20));
    }
    fTimedOutSmid = (UInt16)smid;
    return false;
}

bool
MPSController::takeReplySlot(UInt32 smid, UInt8 *reply, UInt32 replyLen,
                             UInt8 *kind)
{
    bool ok = false;

    if (smid >= 128 || reply == NULL || replyLen == 0 ||
        fReplyLock == NULL) {
        return false;
    }
    IOSimpleLockLock(fReplyLock);
    if (fReplySlots[smid].valid &&
        (fCommandOwners[smid].state == kMPSCommandComplete ||
         fCommandOwners[smid].state == kMPSCommandCancelled)) {
        UInt32 n = replyLen < (UInt32)sizeof(fReplySlots[smid].reply)
                       ? replyLen : (UInt32)sizeof(fReplySlots[smid].reply);

        memcpy(reply, fReplySlots[smid].reply, n);
        if (kind != NULL) {
            *kind = fReplySlots[smid].kind;
        }
        fReplySlots[smid].valid = 0;
        fCommandOwners[smid].state = kMPSCommandFree;
        ok = true;
    }
    IOSimpleLockUnlock(fReplyLock);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* install the event mask and enable the SAS port. Firmware events are  */
/* collected asynchronously by processReplyQueue().                          */

static void
mpsClearEventBit(UInt32 *masks, UInt32 event)
{
    masks[event / 32] &= ~(1U << (event % 32));
}

bool
MPSController::sendEventNotification(void)
{
    UInt8 *frame;
    MPSEventNotifyRequest *request;
    UInt8 replyBuf[64], replyKind = 0;

    if (fRequestBuffer == NULL || !beginManagementRequest(2)) {
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        recordProtocolFault(kMPSProtocolManagementReply, 2);
        return false;
    }
    frame += 2 * fRequestFrameSize;     /* SMID 2 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPSEventNotifyRequest *)frame;

    request->Function = MPI2_FUNCTION_EVENT_NOTIFICATION;
    /* A set bit DISABLES an event (mps.c:2769-2791): mask everything first,
     * then clear the topology, diagnostic and RAID-membership events. */
    memset(request->EventMasks, 0xFF, sizeof(request->EventMasks));
    mpsClearEventBit(request->EventMasks, MPI2_EVENT_STATE_CHANGE);
    mpsClearEventBit(request->EventMasks, MPI2_EVENT_SAS_DEVICE_STATUS_CHANGE);
    mpsClearEventBit(request->EventMasks, MPI2_EVENT_SAS_DISCOVERY);
    mpsClearEventBit(request->EventMasks, MPI2_EVENT_SAS_BROADCAST_PRIMITIVE);
    mpsClearEventBit(request->EventMasks, MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST);
    mpsClearEventBit(request->EventMasks, MPI2_EVENT_LOG_ENTRY_ADDED);
    mpsClearEventBit(request->EventMasks, MPI2_EVENT_IR_CONFIGURATION_CHANGE_LIST);
    mpsClearEventBit(request->EventMasks, MPI2_EVENT_IR_PHYSICAL_DISK);
    mpsClearEventBit(request->EventMasks, MPI2_EVENT_IR_VOLUME);
    for (unsigned word = 0; word < 4; ++word)
        request->EventMasks[word] &= ~fControlEventMask[word];


    MPS_DEBUG_PROP("MPSEventMask0", (unsigned long long)request->EventMasks[0], 32);
    MPS_DEBUG_PROP("MPSEventMask1", (unsigned long long)request->EventMasks[1], 32);
    MPS_DEBUG_PROP("MPSEventMask2", (unsigned long long)request->EventMasks[2], 32);
    MPS_DEBUG_PROP("MPSEventMask3", (unsigned long long)request->EventMasks[3], 32);
    MPS_DEBUG_PROP("MPSEventMaskCheck", "mandatory-plus-client-mask");

    postDefaultRequest(2);
    if (!finishManagementRequest(2, MPS_REPLY_TIMEOUT_MS, replyBuf, &replyKind)) {
        MPS_DEBUG_PROP("MPSEventNotifyState", "no-subscription-reply");
        return false;
    }
    MPSEventNotifyReply *reply = (MPSEventNotifyReply *)replyBuf;
    if (replyKind != 1 || reply->Function != MPI2_FUNCTION_EVENT_NOTIFICATION ||
        reply->MsgLength * 4U < sizeof(MPSEventNotifyReply)) {
        noteBadReply(4, (UInt32)replyKind, 2, replyBuf, sizeof(replyBuf));
        recordProtocolFault(kMPSProtocolManagementReply, 2);
        MPS_DEBUG_PROP("MPSEventNotifyState", "unexpected-subscription-reply");
        return false;
    }
    UInt16 status = reply->IOCStatus & MPI2_IOCSTATUS_MASK;
    MPS_DEBUG_PROP("MPSEventNotifyIOCStatus", (unsigned long long)status, 16);
    MPS_DEBUG_PROP("MPSEventNotifyState", status == MPI2_IOCSTATUS_SUCCESS ? "ok" : "failed-status");
    return status == MPI2_IOCSTATUS_SUCCESS;
}

bool
MPSController::sendPortEnable(void)
{
    if (fRequestBuffer == NULL || !beginManagementRequest(3)) {
        return false;
    }
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        recordProtocolFault(kMPSProtocolManagementReply, 3);
        return false;
    }
    frame += 3 * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    MPSPortEnableRequest *request = (MPSPortEnableRequest *)frame;
    request->Function = MPI2_FUNCTION_PORT_ENABLE;
    request->PortFlags = 0;
    UInt8 replyBuf[64], replyKind = 0;
    postDefaultRequest(3);
    if (!finishManagementRequest(3, MPS_PORT_ENABLE_TIMEOUT_MS, replyBuf, &replyKind)) {
        return false;
    }
    MPSPortEnableReply *reply = (MPSPortEnableReply *)replyBuf;
    if (replyKind != 1 || reply->Function != MPI2_FUNCTION_PORT_ENABLE ||
        reply->MsgLength * 4U < sizeof(MPSPortEnableReply)) {
        noteBadReply(3, (UInt32)replyKind, 3, replyBuf, sizeof(replyBuf));
        recordProtocolFault(kMPSProtocolManagementReply, 3);
        return false;
    }
    UInt16 status = reply->IOCStatus & MPI2_IOCSTATUS_MASK;
    MPS_DEBUG_PROP("MPSPortEnableStatus", (unsigned long long)status, 16);
    MPS_DEBUG_PROP("MPSPortEnableLogInfo", (unsigned long long)reply->IOCLogInfo, 32);
    return status == MPI2_IOCSTATUS_SUCCESS;
}

bool
MPSController::enableEventsAndPort(void)
{
    bool notified, enabled;

    notified = sendEventNotification();
    MPS_DEBUG_PROP("MPSEventNotify", notified ? "posted" : "failed");
    if (!notified) {
        MPS_DEBUG_PROP("MPSEventState", "failed-notify");
        return false;
    }

    enabled = sendPortEnable();
    MPS_DEBUG_PROP("MPSPortEnable", enabled ? "ok" : "failed");
    if (!enabled) {
        MPS_DEBUG_PROP("MPSEventState", "failed-portenable");
        return false;
    }

    /* Let the IOC raise discovery/topology events before sampling counters. */
    IOSleep(5000);

    MPS_DEBUG_PROP("MPSEventCount", (unsigned long long)fEventCount, 32);
    MPS_DEBUG_PROP("MPSLastEvent", (unsigned long long)fLastEvent, 16);
    MPS_DEBUG_PROP("MPSLastEventDataLength", (unsigned long long)fLastEventDataLength, 16);
    MPS_DEBUG_PROP("MPSLastEventAck", (unsigned long long)fLastEventAckRequired, 8);
    MPS_DEBUG_PROP("MPSEventSeen0", (unsigned long long)fEventSeen[0], 32);
    MPS_DEBUG_PROP("MPSEventSeen1", (unsigned long long)fEventSeen[1], 32);
    MPS_DEBUG_PROP("MPSEventSeen2", (unsigned long long)fEventSeen[2], 32);
    MPS_DEBUG_PROP("MPSEventSeen3", (unsigned long long)fEventSeen[3], 32);
    MPS_DEBUG_PROP("MPSEventState", "ok");
    MPS_DEBUG_LOG("MPS events=%u last=0x%04x len=%u ack=%u\n",
          (unsigned int)fEventCount, (unsigned int)fLastEvent,
          (unsigned int)fLastEventDataLength, (unsigned int)fLastEventAckRequired);
    return true;
}

/* ------------------------------------------------------------------------ */
/* extended config page read, used to build the SAS target table.      */

/* Target protocol implementation is in MPSTarget.cpp. */

/* Target protocol implementation is in MPSTarget.cpp. */

/* Target protocol implementation is in MPSTarget.cpp. */

bool
MPSController::discoverTopology(void)
{
    bool ownPage = (fPageBuffer == NULL);
    bool ok;

    if (fProtocolRecoveryPending || !allocatePageBuffer()) {
        MPS_DEBUG_PROP("MPSTopologyState", "failed-buffer-unavailable");
        return false;
    }
    ok = readSasIOUnitPage0();
    MPS_DEBUG_PROP("MPSTopologyState", ok ? "ok" : "failed");

    /* A timed-out command may still DMA into the page. Recovery owns it
     * until the IOC has stopped; never free it merely because a probe ended. */
    if (ownPage && !fProtocolRecoveryPending) {
        mpsReleaseResidentDma(fPageDma);
        fPageBuffer->release();
        fPageBuffer = NULL;
        fPagePhys = 0;
    }
    return ok;
}

/* ------------------------------------------------------------------------ */
/* SCSI INQUIRY through the request/reply queues, validating the SCSI  */
/* IO path before the block-storage nub is added.                            */


bool
MPSController::applyScsiTargetPolicy(UInt8 *frame, MPSTargetSlot *slot)
{
    MPSScsiIoRequest *request = (MPSScsiIoRequest *)frame;
    if (slot) {
        request->Control = (request->Control & ~MPI2_SCSIIO_CONTROL_TLR_MASK) |
                           (slot->tlrEnabled ? MPI2_SCSIIO_CONTROL_TLR_ON : 0);
        const UInt8 opcode = request->CDB[0];
        const bool read = opcode == 0x28 || opcode == 0x88 || opcode == 0xA8;
        const bool write = opcode == 0x2A || opcode == 0x8A || opcode == 0xAA;
        if ((read || write) && slot->protectionType) {
            const UInt32 protectionBlockSize = fProbeInProgress && slot->key() == currentTargetKey()
                ? fBlockSize : slot->blockSize;
            if (slot->protectionUnsupported || !protectionBlockSize ||
                !(fIocCapabilities & MPI2_IOCFACTS_CAPABILITY_EEDP)) {
                return false;
            }
            UInt16 flags = read ? MPI2_SCSIIO_EEDPFLAGS_CHECK_REMOVE_OP : MPI2_SCSIIO_EEDPFLAGS_INSERT_OP;
            flags |= MPI2_SCSIIO_EEDPFLAGS_CHECK_GUARD;
            /* Type 3 has no address reference check. Type 1 increments/checks
             * the low 32 bits of the logical block address on every block. */
            if (slot->protectionType == 1)
                flags |= MPI2_SCSIIO_EEDPFLAGS_INC_PRI_REFTAG | MPI2_SCSIIO_EEDPFLAGS_CHECK_REFTAG;
            request->EEDPFlags = flags;
            request->EEDPBlockSize = protectionBlockSize;
            UInt32 reference = opcode == 0x88 || opcode == 0x8A
                ? (UInt32)mpsReadBe64(request->CDB + 2) : mpsReadBe32(request->CDB + 2);
            memcpy(request->CDB + 20, &reference, sizeof(reference));
            request->CDB[26] = request->CDB[27] = 0xff;
            request->CDB[1] = (request->CDB[1] & 0x1f) | 0x20;
        }
    }
    return true;
}

bool
MPSController::postScsiIo(UInt32 smid)
{
    if (fReplyLock == NULL || smid == 0 || smid >= 128) return false;
    /* Standard MPI2 descriptors carry this command's own DevHandle. */
    UInt8 *base = fRequestBuffer != NULL ?
        (UInt8 *)fRequestBuffer->getBytesNoCopy() : NULL;
    if (base == NULL) return false;
    UInt8 *frame = base + (UInt64)smid * fRequestFrameSize;
    UInt16 handle = (UInt16)((UInt16)frame[0] | ((UInt16)frame[1] << 8));
    if (handle == 0) handle = (UInt16)fAttachedDevHandle;
    MPSScsiIoRequest *request = (MPSScsiIoRequest *)frame;
    UInt32 lun = 0;
    if ((!mpsIsAsyncSmid(smid) && !mpsEncodeLun(request->LUN, fAttachedLun)) ||
        !mpsDecodeLun(request->LUN, &lun)) {
        cancelUnpostedCommand(smid); return false;
    }
    MPSTargetSlot *slot = findTarget(mpsTargetKey(handle, lun));
    if (!applyScsiTargetPolicy(frame, slot)) {
        cancelUnpostedCommand(smid); return false;
    }
    const UInt32 flags = MPI2_REQ_DESCRIPT_FLAGS_SCSI_IO;
    IOSimpleLockLock(fReplyLock);
    bool runtime = (smid >= 10 && smid <= 30) || mpsIsAsyncSmid(smid);
    if (fStopping || fProtocolRecoveryPending ||
        (runtime && (fRemovalPending || !fMediaPresent || fDeviceOffline ||
                     fReinitInProgress)) || fFrozenSmid == smid) {
        IOSimpleLockUnlock(fReplyLock);
        cancelUnpostedCommand(smid);
        return false;
    }
    bool posted = postOwnedRequestLocked(smid, flags,
                                        (UInt32)handle << 16);
    IOSimpleLockUnlock(fReplyLock);
    if (!posted) cancelUnpostedCommand(smid);
    return posted;
}

bool
MPSController::scsiTur(void)
{
    MPSScsiIoRequest *request;
    MPSScsiIoReply *reply;
    UInt8 *frame;
    UInt8 replyBuf[64], replyKind = 0;

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        MPS_DEBUG_PROP("MPSTurFailure", "no attached device or buffer");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    if (!beginManagementRequest(6)) {
        return false;
    }
    if (!prepareScsiSense(6)) {
        cancelUnpostedCommand(6);
        return false;
    }
    frame += 6 * fRequestFrameSize;     /* SMID 6 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPSScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(6);
    request->SenseBufferLength = MPS_SENSE_BYTES;
    request->SGLOffset0 = MPS_SCSIIO_SGE_DWORDS;
    request->DataLength = 0;
    request->IoFlags = (UInt16)6;
    request->Control = MPI2_SCSIIO_CONTROL_NODATATRANSFER;
    request->CDB[0] = 0x00;                 /* TEST UNIT READY */

    /* A zero-length MPI SGE is still required for a data-less command. */
    mpsMpiSimple(frame + MPS_SCSIIO_SGE_DWORDS * 4, fPagePhys, 0, (request->Control & MPI2_SCSIIO_CONTROL_WRITE) != 0);

    memset(fPageBuffer->getBytesNoCopy(), 0, MPS_PAGE_BUF_BYTES);
    if (!postScsiIo(6)) return false;
    MPS_DEBUG_PROP("MPSTurAttempts", 1ULL, 32);
    if (!finishManagementRequest(6, MPS_REPLY_TIMEOUT_MS, replyBuf, &replyKind)) {
        MPS_DEBUG_PROP("MPSTurFailure", "TEST UNIT READY timeout");
        return false;
    }
    IOReturn decodedStatus = kIOReturnIOError;
    if (!validateScsiCompletion(6, replyKind, replyBuf, 0, false,
                                &decodedStatus)) {
        return false;
    }
    reply = (MPSScsiIoReply *)replyBuf;
    MPS_DEBUG_PROP("MPSTurKind", (unsigned long long)replyKind, 32);
    MPS_DEBUG_PROP("MPSTurSCSIState", (unsigned long long)reply->SCSIState, 8);
    if (replyKind == 1) {
        MPS_DEBUG_PROP("MPSTurIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        MPS_DEBUG_PROP("MPSTurScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        return (reply->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS &&
               reply->SCSIStatus == 0;
    }
    MPS_DEBUG_PROP("MPSTurIOCStatus", 0ULL, 16);
    MPS_DEBUG_PROP("MPSTurScsiStatus", 0ULL, 8);
    return true;
}

bool
MPSController::scsiInquiry(void)
{
    MPSScsiIoRequest *request;
    MPSScsiIoReply   *reply;
    UInt8 *frame;
    UInt8 *data;
    bool   posted;
    UInt8 replyBuf[64], replyKind = 0;
    char vendor[9], product[17], revision[5];

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        MPS_DEBUG_PROP("MPSScsiProbeFailure", "no attached device or buffer");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    if (!beginManagementRequest(5)) {
        return false;
    }
    if (!prepareScsiSense(5)) {
        cancelUnpostedCommand(5);
        return false;
    }
    frame += 5 * fRequestFrameSize;     /* SMID 5 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPSScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(5);
    request->SenseBufferLength = MPS_SENSE_BYTES;
    request->SGLOffset0 = MPS_SCSIIO_SGE_DWORDS;
    request->DataLength = 36;
    request->IoFlags = (UInt16)6;   /* six-byte CDB */
    request->Control = MPI2_SCSIIO_CONTROL_READ;
    request->CDB[0] = 0x12;                 /* INQUIRY */
    request->CDB[4] = 36;                   /* allocation length */

    /* MPI SGE placed right after the fixed CDB area. */
    mpsMpiSimple(frame + MPS_SCSIIO_SGE_DWORDS * 4, fPagePhys, 36, (request->Control & MPI2_SCSIIO_CONTROL_WRITE) != 0);

    {
        UInt64 w[4];

        memcpy(w, frame, sizeof(w));
        MPS_DEBUG_PROP("MPSInquiryReqW0", w[0], 64);
        MPS_DEBUG_PROP("MPSInquiryReqW1", w[1], 64);
        MPS_DEBUG_PROP("MPSInquiryReqW2", w[2], 64);
        MPS_DEBUG_PROP("MPSInquiryReqW3", w[3], 64);
        MPS_DEBUG_PROP("MPSInquiryDevHandleUsed",
                    (unsigned long long)fAttachedDevHandle, 16);
        MPS_DEBUG_PROP("MPSInquiryControl", (unsigned long long)request->Control, 32);
        MPS_DEBUG_PROP("MPSInquiryIoFlags", (unsigned long long)request->IoFlags, 32);
    }

    memset(fPageBuffer->getBytesNoCopy(), 0, MPS_PAGE_BUF_BYTES);
    if (!postScsiIo(5)) return false;
    posted = finishManagementRequest(5, 15000, replyBuf, &replyKind);
    MPS_DEBUG_PROP("MPSInquiryAttempts", 1ULL, 32);
    if (!posted) {
        MPS_DEBUG_PROP("MPSInquiryKind", (unsigned long long)replyKind, 32);
        MPS_DEBUG_PROP("MPSInquirySmid", (unsigned long long)fLastReplySmid, 16);
        MPS_DEBUG_PROP("MPSInquiryPostIdx", (unsigned long long)fReplyPostIndex, 32);
        MPS_DEBUG_PROP("MPSInquiryRegPostIdx",
                    (unsigned long long)readReg(MPI2_REPLY_POST_HOST_INDEX_OFFSET), 32);
        MPS_DEBUG_PROP("MPSInquiryRegFreeIdx",
                    (unsigned long long)readReg(MPI2_REPLY_FREE_HOST_INDEX_OFFSET), 32);
        MPS_DEBUG_PROP("MPSScsiProbeFailure", "INQUIRY timeout");
        return false;
    }
    IOReturn decodedStatus = kIOReturnIOError;
    if (!validateScsiCompletion(5, replyKind, replyBuf, 36, false,
                                &decodedStatus)) {
        return false;
    }


    {
        UInt64 w[4];

        memcpy(w, replyBuf, sizeof(w));
        MPS_DEBUG_PROP("MPSInquiryReplyW0", w[0], 64);
        MPS_DEBUG_PROP("MPSInquiryReplyW1", w[1], 64);
        MPS_DEBUG_PROP("MPSInquiryReplyW2", w[2], 64);
        MPS_DEBUG_PROP("MPSInquiryReplyW3", w[3], 64);
        reply = (MPSScsiIoReply *)replyBuf;
        MPS_DEBUG_PROP("MPSInquirySCSIState", (unsigned long long)reply->SCSIState, 8);
        MPS_DEBUG_PROP("MPSInquiryReplyFunction", (unsigned long long)reply->Function, 8);
        MPS_DEBUG_PROP("MPSInquiryReplyMsgLength", (unsigned long long)reply->MsgLength, 8);
    }

    if (replyKind == 1) {
        reply = (MPSScsiIoReply *)replyBuf;
        MPS_DEBUG_PROP("MPSInquiryIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        MPS_DEBUG_PROP("MPSInquiryScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        MPS_DEBUG_PROP("MPSInquiryTransferCount", (unsigned long long)reply->TransferCount, 32);
        MPS_DEBUG_PROP("MPSInquiryResponseInfo", (unsigned long long)reply->ResponseInfo, 32);
        if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
            reply->SCSIStatus != 0 || reply->TransferCount < 36U) {
            MPS_DEBUG_PROP("MPSScsiProbeFailure", "INQUIRY reply status");
            return false;
        }
    } else {
        /* SCSI_IO_SUCCESS: command succeeded, no reply frame was posted. */
        MPS_DEBUG_PROP("MPSInquiryIOCStatus", 0ULL, 16);
        MPS_DEBUG_PROP("MPSInquiryScsiStatus", 0ULL, 8);
        MPS_DEBUG_PROP("MPSInquiryTransferCount", 36ULL, 32);
        MPS_DEBUG_PROP("MPSInquiryResponseInfo", 0ULL, 32);
    }
    MPS_DEBUG_PROP("MPSInquiryReplyKind", (unsigned long long)replyKind, 32);

    data = (UInt8 *)fPageBuffer->getBytesNoCopy();
    if (data == NULL) {
        MPS_DEBUG_PROP("MPSScsiProbeFailure", "INQUIRY data buffer unavailable");
        return false;
    }
    if ((data[0] & 0xE0) != 0 || data[4] < 31) {
        MPS_DEBUG_PROP("MPSScsiProbeFailure", "logical-unit-not-connected");
        return false;
    }
    MPSTargetSlot *inquiryTarget = findTarget(currentTargetKey());
    if (inquiryTarget) {
        inquiryTarget->peripheralType = data[0] & 0x1F;
        inquiryTarget->managementOnly = inquiryTarget->peripheralType != 0;
    }
    mpsCopyField(vendor, data + 8, 8);
    mpsCopyField(product, data + 16, 16);
    mpsCopyField(revision, data + 32, 4);

    /* Cache the identity for the block device nub's get*String() methods. */
    memcpy(fVendor, vendor, sizeof(vendor));
    memcpy(fProduct, product, sizeof(product));
    memcpy(fRevision, revision, sizeof(revision));

    MPS_DEBUG_PROP("MPSInquiryDeviceType", (unsigned long long)(data[0] & 0x1F), 8);
    MPS_DEBUG_PROP("MPSInquiryVendor", vendor);
    MPS_DEBUG_PROP("MPSInquiryProduct", product);
    MPS_DEBUG_PROP("MPSInquiryRevision", revision);
    MPS_DEBUG_LOG("MPS INQUIRY type=%u vendor=%s product=%s rev=%s\n",
          (unsigned int)(data[0] & 0x1F), vendor, product, revision);
    return true;
}

bool
MPSController::scsiReadCapacity16(void)
{
    /* READ CAPACITY(16) is SERVICE ACTION IN(16). The allocation length is
     * a 32-bit field at bytes 10..13 and byte 15 is CONTROL (SBC-3); the
     * previous encoding put 0x20 in the CONTROL byte. */
    static const UInt8 cdb[16] = {
        0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0
    };
    MPSScsiIoRequest *request;
    MPSScsiIoReply *reply;
    UInt8 *frame, *data;
    UInt64 lastLba = 0;
    UInt32 blockSize, i;
    bool posted;
    UInt8 replyBuf[64], replyKind = 0;

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        MPS_DEBUG_PROP("MPSCapacityProbeFailure", "no attached device or buffer");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    if (!beginManagementRequest(7)) {
        return false;
    }
    if (!prepareScsiSense(7)) {
        cancelUnpostedCommand(7);
        return false;
    }
    frame += 7 * fRequestFrameSize;     /* SMID 7 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPSScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(7);
    request->SenseBufferLength = MPS_SENSE_BYTES;
    request->SGLOffset0 = MPS_SCSIIO_SGE_DWORDS;
    request->DataLength = 32;
    request->IoFlags = (UInt16)16;
    request->Control = MPI2_SCSIIO_CONTROL_READ;
    memcpy(request->CDB, cdb, sizeof(cdb));
    mpsMpiSimple(frame + MPS_SCSIIO_SGE_DWORDS * 4, fPagePhys, 32, (request->Control & MPI2_SCSIIO_CONTROL_WRITE) != 0);

    memset(fPageBuffer->getBytesNoCopy(), 0, MPS_PAGE_BUF_BYTES);
    if (!postScsiIo(7)) return false;
    posted = finishManagementRequest(7, 15000, replyBuf, &replyKind);
    if (!posted) {
        MPS_DEBUG_PROP("MPSCapacityProbeFailure", "READ CAPACITY(16) timeout");
        return false;
    }
    IOReturn decodedStatus = kIOReturnIOError;
    if (!validateScsiCompletion(7, replyKind, replyBuf, 32, false,
                                &decodedStatus)) {
        return false;
    }

    if (replyKind == 1) {
        reply = (MPSScsiIoReply *)replyBuf;
        MPS_DEBUG_PROP("MPSScsiIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        MPS_DEBUG_PROP("MPSScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
            reply->SCSIStatus != 0 || reply->TransferCount < 32U) {
            MPS_DEBUG_PROP("MPSCapacityProbeFailure", "READ CAPACITY(16) status");
            return false;
        }
    }

    data = (UInt8 *)fPageBuffer->getBytesNoCopy();
    if (data == NULL) {
        return false;
    }
    for (i = 0; i < 8; i++) {
        lastLba = (lastLba << 8) | (UInt64)data[i];
    }
    blockSize = ((UInt32)data[8] << 24) | ((UInt32)data[9] << 16) |
                ((UInt32)data[10] << 8) | (UInt32)data[11];

    MPS_DEBUG_PROP("MPSCapacityLastLba", lastLba, 64);
    MPS_DEBUG_PROP("MPSCapacityBlockSize", (unsigned long long)blockSize, 32);
    MPS_DEBUG_PROP("MPSCapacityBlocks", lastLba + 1, 64);
    MPS_DEBUG_LOG("MPS READ CAPACITY(16) blocks=%llu blockSize=%u\n",
          (unsigned long long)(lastLba + 1), (unsigned int)blockSize);
    if (blockSize == 0 || lastLba == 0xFFFFFFFFFFFFFFFFULL) {
        return false;
    }
    MPSTargetSlot *capacityTarget = findTarget(currentTargetKey());
    if (capacityTarget) {
        capacityTarget->protectionType = (data[12] & 1) ? (UInt8)(((data[12] >> 1) & 7) + 1) : 0;
        capacityTarget->protectionUnsupported = capacityTarget->protectionType != 0 &&
            (!(fIocCapabilities & MPI2_IOCFACTS_CAPABILITY_EEDP) ||
             (capacityTarget->protectionType != 1 && capacityTarget->protectionType != 3));
        if (capacityTarget->protectionUnsupported) {
            MPS_DEBUG_LOG("MPS: protected LUN refused handle=0x%04x lun=%u type=%u EEDP=%u\n",
                capacityTarget->handle, (unsigned)capacityTarget->lun, capacityTarget->protectionType,
                (fIocCapabilities & MPI2_IOCFACTS_CAPABILITY_EEDP) != 0);
            return false;
        }
    }
    fBlockSize = blockSize;
    fCapacityBlocks = lastLba + 1;
    return true;
}

bool
MPSController::scsiReadCapacity10(void)
{
    static const UInt8 cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    MPSScsiIoRequest *request;
    MPSScsiIoReply *reply;
    UInt8 *frame, *data;
    UInt32 lastLba, blockSize;
    bool posted;
    UInt8 replyBuf[64], replyKind = 0;

    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fPageBuffer == NULL) {
        MPS_DEBUG_PROP("MPSCapacityProbeFailure", "no attached device or buffer");
        return false;
    }
    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    if (!beginManagementRequest(8)) {
        return false;
    }
    if (!prepareScsiSense(8)) {
        cancelUnpostedCommand(8);
        return false;
    }
    frame += 8 * fRequestFrameSize;     /* SMID 8 */
    memset(frame, 0, fRequestFrameSize);
    request = (MPSScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(8);
    request->SenseBufferLength = MPS_SENSE_BYTES;
    request->SGLOffset0 = MPS_SCSIIO_SGE_DWORDS;
    request->DataLength = 8;
    request->IoFlags = (UInt16)10;
    request->Control = MPI2_SCSIIO_CONTROL_READ;
    memcpy(request->CDB, cdb, sizeof(cdb));
    mpsMpiSimple(frame + MPS_SCSIIO_SGE_DWORDS * 4, fPagePhys, 8, (request->Control & MPI2_SCSIIO_CONTROL_WRITE) != 0);

    memset(fPageBuffer->getBytesNoCopy(), 0, MPS_PAGE_BUF_BYTES);
    if (!postScsiIo(8)) return false;
    posted = finishManagementRequest(8, 15000, replyBuf, &replyKind);
    if (!posted) {
        MPS_DEBUG_PROP("MPSCapacityProbeFailure", "READ CAPACITY(10) timeout");
        return false;
    }
    IOReturn decodedStatus = kIOReturnIOError;
    if (!validateScsiCompletion(8, replyKind, replyBuf, 8, false,
                                &decodedStatus)) {
        return false;
    }

    if (replyKind == 1) {
        reply = (MPSScsiIoReply *)replyBuf;
        MPS_DEBUG_PROP("MPSScsiIOCStatus",
                    (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
        MPS_DEBUG_PROP("MPSScsiStatus", (unsigned long long)reply->SCSIStatus, 8);
        if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
            reply->SCSIStatus != 0 || reply->TransferCount < 8U) {
            MPS_DEBUG_PROP("MPSCapacityProbeFailure", "READ CAPACITY(10) status");
            return false;
        }
    }

    data = (UInt8 *)fPageBuffer->getBytesNoCopy();
    if (data == NULL) {
        return false;
    }
    lastLba = ((UInt32)data[0] << 24) | ((UInt32)data[1] << 16) |
              ((UInt32)data[2] << 8) | (UInt32)data[3];
    blockSize = ((UInt32)data[4] << 24) | ((UInt32)data[5] << 16) |
                ((UInt32)data[6] << 8) | (UInt32)data[7];

    /* 0xffffffff means "capacity too large for RC10"; RC16 is mandatory. */
    if (lastLba == 0xFFFFFFFFU) {
        MPS_DEBUG_PROP("MPSCapacityProbeFailure", "RC10 sentinel requires RC16");
        return false;
    }

    MPS_DEBUG_PROP("MPSCapacityLastLba", (unsigned long long)lastLba, 64);
    MPS_DEBUG_PROP("MPSCapacityBlockSize", (unsigned long long)blockSize, 32);
    MPS_DEBUG_PROP("MPSCapacityBlocks", (unsigned long long)lastLba + 1, 64);
    MPS_DEBUG_LOG("MPS READ CAPACITY(10) blocks=%llu blockSize=%u\n",
          (unsigned long long)(lastLba + 1), (unsigned int)blockSize);
    if (blockSize == 0) {
        return false;
    }
    fBlockSize = blockSize;
    fCapacityBlocks = (UInt64)lastLba + 1;
    return true;
}

/* ------------------------------------------------------------------------ */
/* multi-block READ(10)/WRITE(10) through the same fast-path SCSI IO    */
/* path, staged in the low-4GB bulk buffer.                                  */

bool
MPSController::scsiReadWrite(bool write, UInt64 lba, UInt32 blocks,
                             UInt32 blockSize, UInt32 smid, bool diagnostics,
                             bool fua, IOReturn *outStatus)
{
    UInt8 cdb[16];
    MPSScsiIoRequest *request;
    UInt8 *frame;
    UInt64 wideDataLen = (UInt64)blocks * blockSize;
    UInt32 dataLen = (UInt32)wideDataLen;
    UInt32 cdbLength = 0;

    if (outStatus != NULL) {
        *outStatus = kIOReturnNotReady;
    }

    if ((fDeviceOffline && !fReinitInProgress) || fStopping ||
        fProtocolRecoveryPending ||
        (fFrozenSmid != 0 && fFrozenSmid == (UInt16)smid)) {
        return false;
    }
    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL ||
        fPageBuffer == NULL || fDataBuffer == NULL) {
        if (diagnostics) {
            MPS_DEBUG_PROP("MPSReadProbeFailure", "no device or staging buffer");
        }
        return false;
    }
    if (wideDataLen == 0 || wideDataLen > fDataBytes ||
        lba >= fCapacityBlocks || blocks > fCapacityBlocks - lba) {
        if (diagnostics) {
            MPS_DEBUG_PROP("MPSReadProbeFailure", "transfer out of range");
        }
        return false;
    }

    cdbLength = mpsBuildReadWriteCdb(cdb, write, lba, blocks,
                                      fua && isNativeFuaSupported());
    if (cdbLength == 0) return false;

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    if (!prepareScsiSense(smid)) {
        cancelUnpostedCommand(smid);
        return false;
    }
    frame += smid * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    request = (MPSScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(smid);
    request->SenseBufferLength = MPS_SENSE_BYTES;
    request->SGLOffset0 = MPS_SCSIIO_SGE_DWORDS;
    request->DataLength = dataLen;
    request->IoFlags = (UInt16)cdbLength;
    request->Control = write ? MPI2_SCSIIO_CONTROL_WRITE : MPI2_SCSIIO_CONTROL_READ;
    memcpy(request->CDB, cdb, cdbLength);
    mpsMpiSimple(frame + MPS_SCSIIO_SGE_DWORDS * 4, fDataPhys, dataLen, (request->Control & MPI2_SCSIIO_CONTROL_WRITE) != 0);

    /* Clear the previous command's sense so an old CHECK CONDITION cannot be
     * mistaken for this command's. A write caller has already filled the
     * staging buffer; only reads need it cleared. */
    if (!write) {
        memset(fDataBuffer->getBytesNoCopy(), 0, fDataBytes);
    }
    fReplyCount = 0;
    fLastReplyKind = 0;
    fLastReplySmid = 0xFFFF;
    if (!postScsiIo(smid)) {
        if (outStatus != NULL) *outStatus = kIOReturnNoMedia;
        return false;
    }
    UInt32 actual = 0;
    IOReturn result = kIOReturnIOError;
    bool ok = scsiWaitChunk(smid, dataLen, &result, &actual);
    if (ok && write && fua && !isNativeFuaSupported()) {
        result = scsiSynchronizeCache(0, 0, 29, true);
        ok = result == kIOReturnSuccess;
    }
    if (outStatus != NULL) *outStatus = result;
    if (diagnostics) {
        MPS_DEBUG_PROP("MPSReadTransferCount", (unsigned long long)actual, 32);
        MPS_DEBUG_PROP("MPSReadResult", (unsigned long long)(UInt32)result, 32);
    }
    return ok;
}

bool
MPSController::probeBlockRead(void)
{
    UInt8 *data;
    bool ok = false;
    UInt32 i;
    char name[32];

    if (fProtocolRecoveryPending || !allocateResidentBuffers()) {
        MPS_DEBUG_PROP("MPSReadProbeState", "failed-buffer-unavailable");
        return false;
    }
    /* Read the first 8 blocks and decode the MBR/GPT protective entry. */
    ok = scsiReadWrite(false, 0, 8, 512, 9, true, false);
    MPS_DEBUG_PROP("MPSReadProbeState", ok ? "ok" : "failed");

    data = (UInt8 *)fDataBuffer->getBytesNoCopy();
    if (ok && data != NULL) {
        UInt64 w[2];

        memcpy(w, data, sizeof(w));
        MPS_DEBUG_PROP("MPSReadWord0", w[0], 64);
        MPS_DEBUG_PROP("MPSReadWord1", w[1], 64);
        MPS_DEBUG_PROP("MPSReadSig",
                    (unsigned long long)((UInt32)data[510] |
                                         ((UInt32)data[511] << 8)), 16);
        MPS_DEBUG_PROP("MPSReadGptSig",
                    (unsigned long long)((*(const UInt64 *)(data + 512)) ==
                                         0x5452415020494645ULL), 8);

        for (i = 0; i < 4; i++) {
            const UInt8 *entry = data + 0x1BE + i * 16;
            UInt32 start = (UInt32)entry[8] | ((UInt32)entry[9] << 8) |
                           ((UInt32)entry[10] << 16) | ((UInt32)entry[11] << 24);
            UInt32 count = (UInt32)entry[12] | ((UInt32)entry[13] << 8) |
                           ((UInt32)entry[14] << 16) | ((UInt32)entry[15] << 24);

            snprintf(name, sizeof(name), "MPSPart%uType", (unsigned int)i);
            MPS_DEBUG_PROP(name, (unsigned long long)entry[4], 8);
            snprintf(name, sizeof(name), "MPSPart%uStart", (unsigned int)i);
            MPS_DEBUG_PROP(name, (unsigned long long)start, 32);
            snprintf(name, sizeof(name), "MPSPart%uBlocks", (unsigned int)i);
            MPS_DEBUG_PROP(name, (unsigned long long)count, 32);
        }
        MPS_DEBUG_LOG("MPS LBA0 read ok, sig=0x%04x part0type=0x%02x\n",
              (unsigned int)((UInt32)data[510] | ((UInt32)data[511] << 8)),
              (unsigned int)data[0x1BE + 4]);
    }

    /* These are the runtime buffers as well. Keep the same pair for nub
     * publication and recovery, including when a command has timed out. */
    return ok;
}

/* Split command submission from completion so one block request can keep
 * MPS_IO_SLOTS chunk commands outstanding. Each slot owns SMID 10..13, its own
 * request frame, 64 KiB staging window and independent bounded sense area. */
bool
MPSController::scsiPostIo(bool write, UInt64 lba, UInt32 blocks,
                          UInt32 blockSize, UInt32 smid, bool fua,
                          UInt64 dataPhys, IOReturn *outStatus)
{
    UInt8 cdb[16];
    MPSScsiIoRequest *request;
    UInt8 *frame;
    UInt64 wideDataLen = (UInt64)blocks * blockSize;
    UInt32 dataLen = (UInt32)wideDataLen;
    UInt32 cdbLength = 0;

    if (outStatus != NULL) *outStatus = kIOReturnNotReady;
    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL ||
        fDataBuffer == NULL || fPageBuffer == NULL) {
        return false;
    }
    if (wideDataLen == 0 || wideDataLen > MPS_DATA_BUF_BYTES || dataPhys == 0 ||
        lba >= fCapacityBlocks || blocks > fCapacityBlocks - lba) {
        return false;
    }

    cdbLength = mpsBuildReadWriteCdb(cdb, write, lba, blocks,
                                      fua && isNativeFuaSupported());
    if (cdbLength == 0) return false;

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        return false;
    }
    if (!prepareScsiSense(smid)) {
        cancelUnpostedCommand(smid);
        return false;
    }
    frame += smid * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    request = (MPSScsiIoRequest *)frame;
    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(smid);
    request->SenseBufferLength = MPS_SENSE_BYTES;
    request->SGLOffset0 = MPS_SCSIIO_SGE_DWORDS;
    request->DataLength = dataLen;
    request->IoFlags = (UInt16)cdbLength;
    request->Control = write ? MPI2_SCSIIO_CONTROL_WRITE : MPI2_SCSIIO_CONTROL_READ;
    memcpy(request->CDB, cdb, cdbLength);
    mpsMpiSimple(frame + MPS_SCSIIO_SGE_DWORDS * 4, dataPhys, dataLen, (request->Control & MPI2_SCSIIO_CONTROL_WRITE) != 0);

    if (!postScsiIo(smid)) {
        if (outStatus != NULL) *outStatus = kIOReturnNoMedia;
        return false;
    }
    if (outStatus != NULL) *outStatus = kIOReturnSuccess;
    return true;
}

bool
MPSController::scsiWaitChunk(UInt32 smid, UInt32 dataLen, IOReturn *outStatus,
                             UInt32 *actualBytes, bool *unitAttention,
                             UInt32 timeoutMs)
{
    UInt8 replyBuf[64], replyKind = 0;
    if (outStatus != NULL) *outStatus = kIOReturnIOError;
    if (actualBytes != NULL) *actualBytes = 0;
    if (unitAttention != NULL) *unitAttention = false;
    if (smid == 0 || smid >= fRequestCount || smid >= MPS_REQUEST_FRAMES ||
        fReplyLock == NULL) {
        if (outStatus != NULL) *outStatus = kIOReturnBadArgument;
        return false;
    }
    if (!waitForSmid(timeoutMs, smid)) {
        if (outStatus != NULL) *outStatus = kIOReturnTimeout;
        MPS_DEBUG_PROP("MPSLastIOFailure", "reply-timeout");
        MPS_DEBUG_PROP("MPSLastIOSmid", (unsigned long long)smid, 16);
        if (fTimedOutSmid == (UInt16)smid) {
            IOSimpleLockLock(fReplyLock);
            if (fCommandOwners[smid].state == kMPSCommandPosted)
                fCommandOwners[smid].state = kMPSCommandQuarantined;
            IOSimpleLockUnlock(fReplyLock);
            if (fReinitInProgress || fStarting) {
                recordProtocolFault(kMPSProtocolManagementTimeout, smid);
            } else {
                recoverTimedOutCommand(smid);
            }
        }
        /* Consume a completion/cancellation only after recovery proved this
         * command DMA-safe. An unknown-result write is still returned as an
         * error; it is never submitted again by this function. */
        if (commandDmaSafe(smid))
            takeReplySlot(smid, replyBuf, sizeof(replyBuf), &replyKind);
        return false;
    }
    if (!takeReplySlot(smid, replyBuf, sizeof(replyBuf), &replyKind)) return false;
    return validateScsiCompletion(smid, replyKind, replyBuf, dataLen, false,
                                  outStatus, actualBytes, unitAttention);
}

/* ------------------------------------------------------------------------ */
/* IOBlockStorageDevice nub, IOMedia, and the block read path.        */

IOReturn
MPSController::transferBlocks(bool write, IOMemoryDescriptor *buffer,
                              UInt64 block, UInt64 nblks, bool fua,
                              UInt64 *actualBytes)
{
    UInt64 byteCount = 0, done = 0, maxBlocks = 0;
    const UInt32 generation = fMediaGeneration;
    IOReturn status = kIOReturnNotReady;
    bool retried = false;
    if (actualBytes != NULL) *actualBytes = 0;
    if (buffer == NULL) return kIOReturnBadArgument;
    if (fIOLock == NULL) return kIOReturnNotReady;
    status = buffer->prepare();
    if (status != kIOReturnSuccess) return status;
    IOLockLock(fIOLock);
    if (generation != fMediaGeneration || !fMediaPresent || mediaRemovalPending()) {
        status = kIOReturnNoMedia;
        goto out;
    }
    if (fSuspended || fDeviceOffline || fStopping || fReinitInProgress ||
        fProtocolRecoveryPending || fDataBuffer == NULL || fSenseBuffer == NULL ||
        fPageBuffer == NULL || fCapacityBlocks == 0 || fBlockSize == 0) {
        status = kIOReturnNotReady;
        goto out;
    }
    if (nblks == 0) {
        status = kIOReturnSuccess;
        goto out;
    }
    if (nblks > 0xFFFFFFFFFFFFFFFFULL / fBlockSize) {
        status = kIOReturnBadArgument;
        goto out;
    }
    byteCount = nblks * fBlockSize;
    if (buffer->getLength() < byteCount || block >= fCapacityBlocks ||
        nblks > fCapacityBlocks - block) {
        status = kIOReturnBadArgument;
        goto out;
    }
    maxBlocks = fDataBytes / fBlockSize;
    if (maxBlocks == 0 || fDataBuffer->getBytesNoCopy() == NULL) {
        status = kIOReturnNotReady;
        goto out;
    }
    if (maxBlocks > 0xFFFFULL) maxBlocks = 0xFFFFULL;
    for (UInt32 id = 10; id < 10 + MPS_IO_SLOTS; ++id) {
        /* This synchronous caller owns no current batch yet. A previous
         * caller may have returned while an unsafe IOC kept its resident
         * window; a later real reply/reset cancellation can now be retired. */
        if (!commandDmaSafe(id)) {
            recordProtocolFault(kMPSProtocolManagementTimeout, id);
            status = kIOReturnNotReady;
            goto out;
        }
        UInt8 oldReply[64], oldKind = 0;
        takeReplySlot(id, oldReply, sizeof(oldReply), &oldKind);
    }
    status = kIOReturnSuccess;
    while (done < byteCount) {
        UInt32 batch = 0, slotBlocks[MPS_IO_SLOTS];
        UInt64 slotOff[MPS_IO_SLOTS], batchBytes = 0, prefixBytes = 0;
        bool failed = false, prefixOpen = true, allFailuresUA = true;
        bool dmaSafe = true;
        IOReturn failStatus = kIOReturnSuccess;
        UInt8 *pool = (UInt8 *)fDataBuffer->getBytesNoCopy();
        if (generation != fMediaGeneration || !fMediaPresent || mediaRemovalPending()) {
            status = kIOReturnNoMedia;
            break;
        }
        if (fSuspended || fStopping || fDeviceOffline || fReinitInProgress ||
            fProtocolRecoveryPending || pool == NULL) {
            status = kIOReturnNotReady;
            break;
        }
        while (batch < MPS_IO_SLOTS && done + batchBytes < byteCount) {
            const UInt64 offset = done + batchBytes;
            UInt64 blocks = (byteCount - offset) / fBlockSize;
            if (blocks > maxBlocks) blocks = maxBlocks;
            const IOByteCount bytes = (IOByteCount)(blocks * fBlockSize);
            IOReturn posted = kIOReturnNotReady;
            if (write && buffer->readBytes((IOByteCount)offset,
                    pool + (UInt64)batch * MPS_DATA_BUF_BYTES, bytes) != bytes) {
                failed = true;
                allFailuresUA = false;
                failStatus = kIOReturnUnderrun;
                break;
            }
            if (!scsiPostIo(write, block + offset / fBlockSize, (UInt32)blocks,
                            fBlockSize, 10 + batch, fua,
                            fDataPhys + (UInt64)batch * MPS_DATA_BUF_BYTES, &posted)) {
                failed = true;
                allFailuresUA = false;
                failStatus = posted == kIOReturnSuccess ? kIOReturnIOError : posted;
                break;
            }
            slotBlocks[batch] = (UInt32)blocks;
            slotOff[batch] = offset;
            batchBytes += bytes;
            ++batch;
            ++fIoCount;
        }
        /* Every posted window is harvested, even after copy errors. A caller
         * cannot release this loop while later commands still own its windows. */
        for (UInt32 i = 0; i < batch; ++i) {
            IOReturn chunkStatus = kIOReturnIOError;
            UInt32 transferred = 0;
            bool ua = false;
            const IOByteCount bytes = (IOByteCount)((UInt64)slotBlocks[i] * fBlockSize);
            bool ok = scsiWaitChunk(10 + i, (UInt32)bytes, &chunkStatus,
                                    &transferred, &ua);
            if (!commandDmaSafe(10 + i)) dmaSafe = false;
            if (ok && (generation != fMediaGeneration || !fMediaPresent ||
                       mediaRemovalPending())) {
                ok = false;
                chunkStatus = kIOReturnNoMedia;
                ua = false;
            }
            IOByteCount copied = ok ? bytes : 0;
            if (ok && !write) {
                copied = buffer->writeBytes((IOByteCount)slotOff[i],
                            pool + (UInt64)i * MPS_DATA_BUF_BYTES, bytes);
                if (copied != bytes) {
                    ok = false;
                    chunkStatus = kIOReturnUnderrun;
                    ua = false;
                }
            }
            if (prefixOpen) prefixBytes += copied;
            if (!ok) {
                prefixOpen = false;
                allFailuresUA = allFailuresUA && ua;
                if (!failed) failStatus = chunkStatus;
                failed = true;
            } else if (write) {
                ++fWriteCount;
                fWriteBytes += bytes;
                fLastWriteLba = block + slotOff[i] / fBlockSize;
                fLastWriteSmid = 10 + i;
            }
        }
        if (!dmaSafe) {
            fDeviceOffline = true;
            recordProtocolFault(kMPSProtocolManagementTimeout, fTimedOutSmid);
            allFailuresUA = false;
            failed = true;
            if (failStatus == kIOReturnSuccess) failStatus = kIOReturnTimeout;
        }
        if (failed) {
            /* Re-reading a completed sibling is harmless; re-posting an
             * unknown-result WRITE is not. Retry only explicitly decoded,
             * current UNIT ATTENTION on a read, once, after all DMA has ended. */
            if (!write && allFailuresUA && !retried && dmaSafe &&
                generation == fMediaGeneration && fMediaPresent &&
                !fProtocolRecoveryPending && !mediaRemovalPending()) {
                retried = true;
                ++fIoRetryCount;
                scsiTur();
                IOSleep(50);
                continue;
            }
            done += prefixBytes;
            status = failStatus == kIOReturnSuccess ? kIOReturnIOError : failStatus;
            break;
        }
        if (batch == 0) {
            status = kIOReturnIOError;
            break;
        }
        done += batchBytes;
    }
    MPS_DEBUG_PROP("MPSBlockIODoneBytes", done, 64);
    MPS_DEBUG_PROP("MPSBlockIOCount", (unsigned long long)fIoCount, 32);
    MPS_DEBUG_PROP("MPSBlockIORetryCount", (unsigned long long)fIoRetryCount, 32);
out:
    IOLockUnlock(fIOLock);
    buffer->complete();
    if (actualBytes != NULL) *actualBytes = done;
    return status;
}

bool
MPSController::allocatePageBuffer(void)
{
    IOBufferMemoryDescriptor *page = fPageBuffer;
    bool allocated = false;

    if (page == NULL) {
        page = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
            MPS_PAGE_BUF_BYTES, MPS_DMA_LOW_MASK);
        if (page == NULL) {
            return false;
        }
        allocated = true;
    }
    if (!mpsMapResidentDma(fPageDma, page, fDmaMapper, MPS_PAGE_BUF_BYTES, 4)) {
        if (allocated) {
            page->release();
        }
        return false;
    }
    fPageBuffer = page;
    fPagePhys = fPageDma.address;
    return true;
}

bool
MPSController::allocateResidentBuffers(void)
{
    IOBufferMemoryDescriptor *data = fDataBuffer;
    bool allocated = false;
    bool ownPage = (fPageBuffer == NULL);

    if (!allocatePageBuffer()) {
        return false;
    }
    if (data == NULL) {
        data = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
            MPS_DATA_POOL_BYTES, MPS_DMA_LOW_MASK);
        allocated = (data != NULL);
    }
    if (data != NULL) {
        if (mpsMapResidentDma(fDataDma, data, fDmaMapper, MPS_DATA_POOL_BYTES, 16)) {
            fDataBuffer = data;
            fDataPhys = fDataDma.address;
            /* fDataBytes remains the per-command window size: maxBlocks and the
             * probe length checks are expressed in it. */
            fDataBytes = MPS_DATA_BUF_BYTES;
            return true;
        }
    }
    if (allocated) {
        data->release();
    }
    if (ownPage && !fProtocolRecoveryPending) {
        mpsReleaseResidentDma(fPageDma);
        fPageBuffer->release();
        fPageBuffer = NULL;
        fPagePhys = 0;
    }
    return false;
}

/* Target protocol implementation is in MPSTarget.cpp. */

bool
MPSController::probeCapacity(void)
{
    bool ownPage = (fPageBuffer == NULL);
    bool ok;

    if (fProtocolRecoveryPending || !allocatePageBuffer()) {
        MPS_DEBUG_PROP("MPSCapacityProbeState", "failed-buffer-unavailable");
        return false;
    }
    MPSTargetSlot *capacityTarget = findTarget(currentTargetKey());
    if (capacityTarget) {
        capacityTarget->protectionUnsupported = false;
        capacityTarget->protectionType = 0;
    }
    ok = scsiReadCapacity16();
    if (ok) {
        MPS_DEBUG_PROP("MPSCapacityKind", "rc16");
    } else if (!fProtocolRecoveryPending && !(capacityTarget && capacityTarget->protectionUnsupported)) {
        /* A SATA bridge rejects the 16-byte CDB with ILLEGAL REQUEST. That is
         * an expected fallback, not a failed disk: only the RC10 outcome may
         * classify this stage, so discard the RC16 status first. */
        beginProbeStage();
        ok = scsiReadCapacity10();
        if (ok) {
            MPS_DEBUG_PROP("MPSCapacityKind", "rc10");
        }
    }
    MPS_DEBUG_PROP("MPSCapacityProbeState", ok ? "ok" : "failed");

    if (ownPage && !fProtocolRecoveryPending) {
        mpsReleaseResidentDma(fPageDma);
        fPageBuffer->release();
        fPageBuffer = NULL;
        fPagePhys = 0;
    }
    return ok;
}

/* Target protocol implementation is in MPSTarget.cpp. */

bool
MPSController::readManufacturingPage0(void)
{
    UInt8      *frame;
    MPSConfigRequest *config;
    MPSConfigReply   *reply = NULL;
    MPSConfigPageMan0 *page;
    UInt32 value32;
    UInt32 baddr = 0;
    UInt8  replyCopy[64];
    bool   ok = false;
    UInt8 replyKind = 0;

    fPageBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
        MPS_CONFIG_PAGE_BYTES, MPS_DMA_LOW_MASK);
    if (fPageBuffer == NULL) {
        MPS_DEBUG_PROP("MPSConfigPageFailure", "page buffer allocation failed");
        return false;
    }
    if (!mpsMapResidentDma(fPageDma, fPageBuffer, fDmaMapper, MPS_CONFIG_PAGE_BYTES, 4)) {
        MPS_DEBUG_PROP("MPSConfigPageFailure", "page-DMA-mapping-failed");
        return false;
    }
    fPagePhys = fPageDma.address;
    memset(fPageBuffer->getBytesNoCopy(), 0, MPS_CONFIG_PAGE_BYTES);

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL) {
        MPS_DEBUG_PROP("MPSConfigPageFailure", "request frame unavailable");
        return false;
    }

    /* SMID 1 = frame 1. The request descriptor (RequestFlags/SMID) travels
     * through the 0xC0/0xC4 registers; the frame itself begins at the message
     * header because MPI2 request frames have no descriptor prefix
     * (mps.c:1583-1587, mps.c:3941). */
    if (!beginManagementRequest(1)) {
        return false;
    }
    frame += fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    config = (MPSConfigRequest *)frame;

    config->Function = MPI2_FUNCTION_CONFIG;
    config->Action = MPI2_CONFIG_ACTION_PAGE_READ_CURRENT;
    config->Header.PageType = MPI2_CONFIG_PAGETYPE_MANUFACTURING;
    config->Header.PageNumber = 0;
    config->Header.PageVersion = MPI2_MANUFACTURING0_PAGEVERSION;
    /* Manufacturing Page 0 is a fixed 0x4C-byte page; the request carries the
     * length in 32-bit words (mps_config.c:141-142). */
    config->Header.PageLength = 19;
    config->PageAddress = 0;
    mpsMpiSimple((UInt8 *)config + 0x1c, fPagePhys, 76, false);

    /* Request descriptor: DEFAULT_TYPE plus SMID 1 in bits 16-31. */
    value32 = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE | (1U << 16);
    postDefaultRequest(1);

    {
        UInt64 fw[4];

        memcpy(fw, frame, sizeof(fw));
        MPS_DEBUG_PROP("MPSConfigFrameW0", fw[0], 64);
        MPS_DEBUG_PROP("MPSConfigFrameW1", fw[1], 64);
        MPS_DEBUG_PROP("MPSConfigFrameW2", fw[2], 64);
        MPS_DEBUG_PROP("MPSConfigFrameW3", fw[3], 64);
        MPS_DEBUG_PROP("MPSConfigDescLow", (unsigned long long)value32, 32);
        MPS_DEBUG_PROP("MPSConfigRequestPhys", fRequestPhys, 64);
        MPS_DEBUG_PROP("MPSConfigPagePhys", fPagePhys, 64);
    }

    /* Consume only this config command's completion slot. */
    if (!finishManagementRequest(1, MPS_REPLY_TIMEOUT_MS, replyCopy, &replyKind)) {
        MPS_DEBUG_PROP("MPSConfigPageFailure", "no reply descriptor received");
        return false;
    }
    reply = (MPSConfigReply *)replyCopy;
    if (replyKind != 1 || reply->Function != MPI2_FUNCTION_CONFIG ||
        reply->MsgLength * 4U < sizeof(MPSConfigReply)) {
        recordProtocolFault(kMPSProtocolManagementReply, 1);
        return false;
    }
    baddr = (UInt32)fReplyPhys;
    ok = true;

    if (ok && reply != NULL) {
        UInt64 w[4];

        memcpy(w, (const UInt8 *)reply, sizeof(w));
        MPS_DEBUG_PROP("MPSConfigReplyW0", w[0], 64);
        MPS_DEBUG_PROP("MPSConfigReplyW1", w[1], 64);
        MPS_DEBUG_PROP("MPSConfigReplyW2", w[2], 64);
        MPS_DEBUG_PROP("MPSConfigReplyW3", w[3], 64);
        MPS_DEBUG_PROP("MPSConfigReplyBase", fReplyPhys, 64);
        MPS_DEBUG_PROP("MPSConfigReplyAddr", (unsigned long long)baddr, 64);
        MPS_DEBUG_PROP("MPSConfigHostIntStatus",
                    (unsigned long long)readReg(MPI2_HOST_INTERRUPT_STATUS_OFFSET), 32);
        MPS_DEBUG_PROP("MPSConfigPostHostIndex",
                    (unsigned long long)readReg(MPI2_REPLY_POST_HOST_INDEX_OFFSET), 32);
    }

    if (!ok || reply == NULL) {
        MPS_DEBUG_PROP("MPSConfigPageFailure", "no reply descriptor received");
        return false;
    }

    MPS_DEBUG_PROP("MPSConfigIOCStatus",
                (unsigned long long)(reply->IOCStatus & MPI2_IOCSTATUS_MASK), 16);
    MPS_DEBUG_PROP("MPSConfigIOCLogInfo", (unsigned long long)reply->IOCLogInfo, 32);
    MPS_DEBUG_PROP("MPSConfigPageType", (unsigned long long)reply->Header.PageType, 8);
    MPS_DEBUG_PROP("MPSConfigPageNumber", (unsigned long long)reply->Header.PageNumber, 8);
    MPS_DEBUG_PROP("MPSConfigPageLength", (unsigned long long)reply->Header.PageLength, 8);

    if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS) {
        MPS_DEBUG_PROP("MPSConfigPageFailure", "config request failed");
        return false;
    }
    if (reply->Header.PageType != MPI2_CONFIG_PAGETYPE_MANUFACTURING ||
        reply->Header.PageNumber != 0) {
        MPS_DEBUG_PROP("MPSConfigPageFailure", "unexpected config page");
        return false;
    }

    page = (MPSConfigPageMan0 *)fPageBuffer->getBytesNoCopy();
    if (page == NULL) {
        MPS_DEBUG_PROP("MPSConfigPageFailure", "page buffer has no virtual address");
        return false;
    }

    {
        char chip[17], revision[9], board[17], assembly[17], tracer[17];

        mpsCopyField(chip, page->ChipName, sizeof(page->ChipName));
        mpsCopyField(revision, page->ChipRevision, sizeof(page->ChipRevision));
        mpsCopyField(board, page->BoardName, sizeof(page->BoardName));
        mpsCopyField(assembly, page->BoardAssembly, sizeof(page->BoardAssembly));
        mpsCopyField(tracer, page->BoardTracerNumber, sizeof(page->BoardTracerNumber));

        MPS_DEBUG_PROP("MPSManPage0ChipName", chip);
        MPS_DEBUG_PROP("MPSManPage0ChipRevision", revision);
        MPS_DEBUG_PROP("MPSManPage0BoardName", board);
        MPS_DEBUG_PROP("MPSManPage0BoardAssembly", assembly);
        MPS_DEBUG_PROP("MPSManPage0BoardTracerNumber", tracer);
        MPS_DEBUG_LOG("MPS ManPage0 chip=%s rev=%s board=%s\n", chip, revision, board);
    }

    return true;
}

bool
MPSController::readManufacturingInfo(void)
{
    bool ok;

    fReplyPostIndex = 0;
    ok = readManufacturingPage0();

    if (fPageBuffer != NULL && !fProtocolRecoveryPending) {
        mpsReleaseResidentDma(fPageDma);
        fPageBuffer->release();
        fPageBuffer = NULL;
    }
    if (fPageBuffer == NULL) fPagePhys = 0;
    MPS_DEBUG_PROP("MPSConfigPageState", ok ? "ok" : "failed");
    return ok;
}

bool
MPSController::init(OSDictionary *properties)
{
    mpsInitDebugOutput();
    fAsync = NULL;
    fMaxChainDepth = fChainFrameBytes = 0;
    fMpiVersion = MPI2_VERSION;
    bzero(fIocFactsSnapshot, sizeof(fIocFactsSnapshot));
    fControlBuffer = NULL; fControlPhys = 0; fControlBufferBytes = 0;
    fDmaMapper = NULL;
    bzero(&fQueueDma, sizeof(fQueueDma));
    bzero(&fReplyDma, sizeof(fReplyDma));
    bzero(&fRequestDma, sizeof(fRequestDma));
    bzero(&fSenseDma, sizeof(fSenseDma));
    bzero(&fPageDma, sizeof(fPageDma));
    bzero(&fDataDma, sizeof(fDataDma));
    bzero(&fControlDma, sizeof(fControlDma));
    fControlBufferPrepared = fControlDmaBusy = false;
    int management = 0, maintenance = 0, ssu = 1;
    (void)PE_parse_boot_argn("mps_mgmt", &management, sizeof(management));
    (void)PE_parse_boot_argn("mps_maintenance", &maintenance, sizeof(maintenance));
    (void)PE_parse_boot_argn("mps_ssu", &ssu, sizeof(ssu));
    fMaintenanceMode = maintenance != 0;
    fManagementEnabled = management != 0 || fMaintenanceMode;
    fFirmwareDownloadActive = fFirmwareDownloadPoisoned = false;
    fFirmwareImageType = 0; fFirmwareTotal = fFirmwareOffset = 0; fFirmwareToken = 0;
    fShutdownSsuPolicy = ssu >= 0 && ssu <= 3 ? (UInt32)ssu : 1;
    bzero(fDiagnostic, sizeof(fDiagnostic));
    bzero(fControlEvents, sizeof(fControlEvents));
    fControlEventSequence = 0;
    /* Initially record mandatory subscribed events. Optional mask bits are
     * explicitly enabled by the privileged management client. */
    memset(fControlEventMask, 0, sizeof(fControlEventMask));
    fPowerQuiesced = false;
    fIOLock = NULL;
    fReplyLock = NULL;
    fReplyWaitLock = NULL;
    fReplyWakeSequence = 0;
    fReplyBatches = fReplyDescriptors = fReplyMaxBatch = fReplyIndexWrites = 0;
    fPostLock = NULL;
    fIocCapabilities = fMaxDeviceHandle = fTargetDeviceInfo = 0;
    fTargetGenerationSerial = 0;
    fBindingSerial = 0;
    fRetiredNubCount = 0;
    memset(fRetiredNubs, 0, sizeof(fRetiredNubs));
    fTargetDeviceFlags = 0;
    fTargetSasAddress = 0;
    fHasExpander = false;
    fDiscoveredDiskCount = 0;
    fNativeFuaSupported = false;
    fUnmapLimits = MPSUnmapLimits{};
    fTargetIdentityValid = false;
    fTargetIdentity[0] = '\0';
    fPendingRemovalHandle = 0;
    fPhysicalChangePending = false;
    return super::init(properties);
}

void
MPSController::free(void)
{
    destroyAsyncState();
    bool residentHeld = fQueueDma.command || fReplyDma.command || fRequestDma.command ||
        fSenseDma.command || fPageDma.command || fDataDma.command || fControlDma.command;
    for (unsigned type = 0; type < 3; ++type)
        residentHeld = residentHeld || fDiagnostic[type].dma.command;
    if (fDmaMapper && !residentHeld) {
        fDmaMapper->release();
        fDmaMapper = NULL;
    }
    /* An unproved final DMA stop intentionally retains mapper, descriptors
     * and prepared commands together. Do not revoke their IOVA here. */
    /* stop() leaves these locks alive for already queued callers to wake,
     * observe fStopping and return. The service's final release owns them. */
    if (fIOLock != NULL) {
        IOLockFree(fIOLock);
        fIOLock = NULL;
    }
    if (fReplyLock != NULL) {
        IOSimpleLockFree(fReplyLock);
        fReplyLock = NULL;
    }
    if (fReplyWaitLock != NULL) {
        IOLockFree(fReplyWaitLock);
        fReplyWaitLock = NULL;
    }
    if (fPostLock != NULL) {
        IOSimpleLockFree(fPostLock);
        fPostLock = NULL;
    }
    super::free();
}

bool
MPSController::start(IOService *provider)
{
    mpsInitDebugOutput();
    IOPCIDevice *pci;
    IOByteCount  length;

    if (!super::start(provider)) {
        return false;
    }

    /* OSObject instances are not guaranteed zero-filled; every member used
     * during bring-up is initialized here so a failure cannot read garbage. */
    fPCIDevice = NULL;
    fMMIO = NULL;
    fRegs = NULL;
    fQueueBuffer = NULL;
    fReplyBuffer = NULL;
    fRequestBuffer = NULL;
    fPageBuffer = NULL;
    fSenseBuffer = NULL;
    fSensePhys = 0;
    fDataBuffer = NULL;
    fFreePhys = fPostPhys = fReplyPhys = fRequestPhys = fPagePhys = 0;
    fDataPhys = 0;
    fDataBytes = 0;
    fFqDepth = fPqDepth = fRequestCount = fReplyFreeIndex = 0;
    fRequestFrameSize = fReplyFrameSize = fRequestCredit = fMaxReplyDepth = 0;
    fReplyPostIndex = 0;
    fFactsValid = false;
    fWorkLoop = NULL;
    fInterrupt = NULL;
    fReplyCount = 0;
    fInterruptCount = 0;
    fReplyViaIrq = 0;
    fEventCount = 0;
    fLastEvent = 0;
    fLastEventDataLength = 0;
    fLastEventAckRequired = 0;
    fLastEventContext = 0;
    fEventSeen[0] = fEventSeen[1] = fEventSeen[2] = fEventSeen[3] = 0;
    fAttachedDevHandle = 0;
    fAttachedLun = 0;
    fLastReplySmid = 0xFFFF;
    fLastReplyKind = 0;
    fScsiSuccess = 0;
    fIOLock = IOLockAlloc();
    fReplyLock = IOSimpleLockAlloc();
    fReplyWaitLock = IOLockAlloc();
    fPostLock = IOSimpleLockAlloc();
    memset(fManagementBusy, 0, sizeof(fManagementBusy));
    memset(fTmfBusy, 0, sizeof(fTmfBusy));
    memset(fEventAckQueue, 0, sizeof(fEventAckQueue));
    memset(fCommandOwners, 0, sizeof(fCommandOwners));
    fTransportEpoch = 1;
    fUnexpectedCompletionCount = 0;
    fEventAckHead = fEventAckQueued = fEventAckAge = 0;
    fEventAckBusy = false;
    fEventAckCompleted = fEventAckFailed = fEventAckOverflow = 0;
    fProtocolRecoveryPending = false;
    fProtocolFaultCode = fProtocolFaultSmid = 0;
    fStickyFaultCode = fStickyFaultSmid = fStickyFaultCount = 0;
    fLastTopoHandle = fLastTopoReason = fLastTopoRate = 0;
    fTopoRemovalHits = 0;
    fBadReplySite = fBadReplyValue = fBadReplySmid = fBadReplyCount = 0;
    fBadReplyFunction = fBadReplyMsgLen = fBadReplyWord = 0;
    fLinkUpPhys = 0;
    fWriteCount = 0;
    fWriteBytes = 0;
    fLastWriteLba = 0;
    fLastWriteSmid = 0;
    fSasDeviceMismatch = false;
    fIdentityLostCount = 0;
    fTopologyGeneration = 0;
    fRemovalPending = false;
    fHotplugRetryRequested = false;
    fPendingRemovalHandle = 0;
    fRemovalFailures = 0;
    fFaultRecoveries = 0;
    fIdleScanTicks = 0;
    fResumeGraceTicks = 0;
    fWakeSettleTicks = 0;
    fRemovedMediaCount = 0;
    fRemovedAgeTicks = 0;
    fRediscoveryResets = 0;
    fRediscoverRequest = false;
    fScanSerial = 0;
    fTargetCount = 0;
    memset(fTargets, 0, sizeof(fTargets));
    memset(fReplySlots, 0, sizeof(fReplySlots));
    fCapacityBlocks = 0;
    fBlockSize = 0;
    fNextIoSmid = 0;
    fIoCount = 0;
    fIoRetryCount = 0;
    fLastProbeStatus = kIOReturnSuccess;
    fProbeInProgress = false;
    fFrozenSmid = 0;
    fTimedOutSmid = 0;
    fUnmapEnabled = false;
    fReinitInProgress = false;
    fCommandCount = 0;
    fTmfCount = 0;
    fResetCount = 0;
    fLastSenseKey = 0xFF;
    fLastASC = 0;
    fLastASCQ = 0;
    fDeviceOffline = false;
    fMediaPresent = false;
    fMediaGeneration = 0;
    fStarting = true;
    fMediaOfflinePending = false;
    fMediaOnlinePending = false;
    fHotplugAttempts = 0;
    fStopping = false;
    fTopologyChanged = false;
    fWatchdogTimer = NULL;
    fHotplugCall = NULL;
    fHotplugPending = false;
    fHotplugRunning = false;
    /* A startup gate, not a blind time window, prevents concurrent probes. */
    fHotplugCooldown = 0;
    fInjectTimeout = false;
    fInjectTmfFail = 0;
    fInjectEvery = 0;
    fInjectCounter = 0;
    fInjectCount = 0;
    fSuspended = false;
    fHighPriorityCredit = 0;
    fEventReplyCount = 0;
    fEventAckCount = 0;
    fReplyFrameReturned = 0;
    memset(fVendor, 0, sizeof(fVendor));
    memset(fProduct, 0, sizeof(fProduct));
    memset(fRevision, 0, sizeof(fRevision));
    memset(fLastReply, 0, sizeof(fLastReply));
    memset(fLastEventData, 0, sizeof(fLastEventData));

    if (fIOLock == NULL || fReplyLock == NULL || fPostLock == NULL ||
        fReplyWaitLock == NULL) {
        if (fIOLock != NULL) IOLockFree(fIOLock);
        if (fReplyLock != NULL) IOSimpleLockFree(fReplyLock);
        if (fPostLock != NULL) IOSimpleLockFree(fPostLock);
        if (fReplyWaitLock != NULL) IOLockFree(fReplyWaitLock);
        fReplyWaitLock = NULL;
        fIOLock = NULL;
        fReplyLock = fPostLock = NULL;
        super::stop(provider);
        return false;
    }

    pci = OSDynamicCast(IOPCIDevice, provider);
    if (pci == NULL) {
        super::stop(provider);
        return false;
    }
    const MPSPciId *startId = mpsLookupPciId(pci->configRead32(kIOPCIConfigVendorID));
    if (!startId) {
        MPS_DEBUG_LOG("MPS: start refused unsupported or non-secure PCI identity\n");
        super::stop(provider);
        return false;
    }
    fPCIDevice = pci;
    fWarpDrive = (pci->configRead32(0) == 0x007e1000U);
    fWarpDriveHidePhysical = true;

    fDmaMapper = IOMapper::copyMapperForDevice(fPCIDevice);
    MPS_DEBUG_PROP("MPSDeviceMapperPresent", fDmaMapper != NULL);
    /* The helper return values are not a readiness proof on this controller:
     * they report failure during early enumeration even when the command
     * register ends up programmed, which aborted every start() attempt. Judge
     * the result from the command register instead, with one bounded retry. */
    bool memEnabled = pci->setMemoryEnable(true);
    bool busEnabled = pci->setBusMasterEnable(true);
    UInt16 command = pci->configRead16(kIOPCIConfigCommand);
    if (!memEnabled || !busEnabled) {
        IOSleep(2);
        memEnabled = pci->setMemoryEnable(true);
        busEnabled = pci->setBusMasterEnable(true);
        command = pci->configRead16(kIOPCIConfigCommand);
    }
    MPS_DEBUG_PROP("MPSSetMemoryEnableResult", memEnabled);
    MPS_DEBUG_PROP("MPSSetBusMasterResult", busEnabled);
    MPS_DEBUG_PROP("MPSPCICommandRegister", (unsigned long long)command, 16);
    if ((command & (kIOPCICommandMemorySpace | kIOPCICommandBusMaster)) !=
        (kIOPCICommandMemorySpace | kIOPCICommandBusMaster)) {
        MPS_DEBUG_LOG("MPS: command register lacks memory decode/bus master (0x%04x)\n",
                      (unsigned)command);
        super::stop(provider);
        return false;
    }

    /* FreeBSD mps_pci_attach uses BAR1; discover its current memory resource.
     * Skip I/O BARs and the upper half of 64-bit memory BARs. */
    UInt32 mmioRegister = 0;
    for (unsigned index = 0; index < 6; ++index) {
        UInt32 reg = kIOPCIConfigBaseAddress0 + index * 4;
        UInt32 bar = pci->configRead32(reg);
        if (bar == 0xFFFFFFFFU || (bar & 1) || (bar & 6) == 6) continue;
        const bool wide = (bar & 6) == 4;
        UInt64 address = bar & ~0xFULL;
        if (wide) {
            if (index == 5) break;
            address |= (UInt64)pci->configRead32(reg + 4) << 32;
        }
        if (address) {
            IOMemoryMap *candidate = pci->mapDeviceMemoryWithRegister(reg);
            if (candidate && candidate->getLength() >= 0x100) {
                fMMIO = candidate;
                mmioRegister = reg;
                break;
            }
            if (candidate) candidate->release();
        }
        if (wide) ++index;
    }
    if (!fMMIO) {
        MPS_DEBUG_LOG("MPS: no usable controller MMIO BAR for %s\n", startId->name);
        pci->setBusMasterEnable(false);
        super::stop(provider);
        return false;
    }
    length = fMMIO->getLength();
    fRegs = (volatile UInt32 *)fMMIO->getVirtualAddress();
    char barName[12];
    snprintf(barName, sizeof(barName), "0x%02x", (unsigned)mmioRegister);
    MPS_DEBUG_PROP("MPSMMIOBAR", barName);
    MPS_DEBUG_LOG("MPS: %s MMIO BAR=%s length=%llu\n", startId->name, barName,
                  (unsigned long long)length);
    MPS_DEBUG_PROP("MPSMMIOLength", (unsigned long long)length, 64);
    MPS_DEBUG_PROP("MPSPCIID", (unsigned long long)pci->configRead32(0), 32);
    MPS_DEBUG_PROP("MPSStage", "read-write");
    MPS_DEBUG_PROP("MPSBuild", "0.1.0");
    MPS_DEBUG_PROP("MPSFeatures",
                "async-direct-dma,credit-depth,ready-refill,reply-batch-wakeup,"
                "mpi-sgl-chain,read-write16,cache16,unmap-limits,"
                "stable-identity,raid-filter,auto-sense,tmf,remove-device,pm-quiesce,"
                "mpi20,legacy-sgl,raid-volumes");
    {
        /* Opt-in fault injection for the timeout/TMF path. The kernel ignores
         * unknown boot-args, so this is inert unless explicitly requested. */
        int inject = 0;
        int tmfFail = 0;

        if (PE_parse_boot_argn("mps_timeout_test", &inject, sizeof(inject))) {
            fInjectTimeout = (inject != 0);
        }
        if (PE_parse_boot_argn("mps_tmf_fail", &tmfFail, sizeof(tmfFail)) &&
            tmfFail > 0) {
            fInjectTmfFail = (UInt32)tmfFail;
        }
        /* Repeatable variant for a soak: every Nth runtime command loses its
         * reply, which drives the timeout -> TMF -> reset chain over and over. */
        if (PE_parse_boot_argn("mps_timeout_every", &tmfFail, sizeof(tmfFail)) &&
            tmfFail > 1) {
            fInjectEvery = (UInt32)tmfFail;
        }
        MPS_DEBUG_PROP("MPSInjectTimeout",
                    (unsigned long long)(fInjectTimeout ? 1 : 0), 8);
        MPS_DEBUG_PROP("MPSInjectTmfFail",
                    (unsigned long long)fInjectTmfFail, 8);
        MPS_DEBUG_PROP("MPSInjectEvery", (unsigned long long)fInjectEvery, 32);
    }

    /* Failures during bring-up are recorded in IORegistry and never panic
     * the machine or prevent the kext from loading. Config-page reads
     * poll the reply queue and take no interrupts. */
    bool interruptsReady = setupInterrupts();
    if (fWorkLoop != NULL) {
        fWatchdogTimer = IOTimerEventSource::timerEventSource(this,
            OSMemberFunctionCast(IOTimerEventSource::Action, this,
                                 &MPSController::watchdogTimeout));
        if (fWatchdogTimer != NULL &&
            fWorkLoop->addEventSource(fWatchdogTimer) == kIOReturnSuccess) {
            fWatchdogTimer->setTimeoutMS(1000);
            MPS_DEBUG_PROP("MPSWatchdogState", "armed");
        } else {
            MPS_DEBUG_PROP("MPSWatchdogState", "arm-failed");
        }
    } else {
        MPS_DEBUG_PROP("MPSWatchdogState", "no-workloop");
    }
    fHotplugCall = thread_call_allocate(hotplugCall, this);
    if (!initializeAsync()) {
        MPS_DEBUG_PROP("MPSAsyncState", "allocation-failed");
        stop(provider);
        return false;
    }
    if (initializeController() && initializeQueues()) {
        readManufacturingInfo();
        bool eventsReady = interruptsReady && enableEventsAndPort();
        if (!eventsReady) {
            recordProtocolFault(interruptsReady ? kMPSProtocolManagementReply :
                                kMPSProtocolInterruptUnavailable, 0);
        }
        /* A disk that is readable only through a timeout's fallback poll
         * cannot support idle hotplug. IRQ, subscription and PortEnable
         * success are mandatory before publishing a media object. */
        if (eventsReady && discoverTopology() && fTargetCount != 0) {
            /* Publish every eligible target; a target that cannot be probed
             * stays unpublished and is retried by the hotplug worker. */
            ensureTargetsPublished();
        }
    } else {
        MPS_DEBUG_PROP("MPSConfigPageState", "skipped-earlier-stage-failed");
        recordProtocolFault(kMPSProtocolManagementTimeout, 0);
    }

    fStarting = false;
    if (!fMediaPresent && fReplyLock != NULL) {
        IOSimpleLockLock(fReplyLock);
        fHotplugPending = true;
        IOSimpleLockUnlock(fReplyLock);
    }

    MPS_DEBUG_PROP("MPSIntCount", (unsigned long long)fInterruptCount, 32);
    MPS_DEBUG_PROP("MPSReplyViaIrq", (unsigned long long)fReplyViaIrq, 32);
    MPS_DEBUG_PROP("MPSEventCount", (unsigned long long)fEventCount, 32);
    MPS_DEBUG_PROP("MPSLastEvent", (unsigned long long)fLastEvent, 16);
    /* Join the power tree only once the controller is usable, so a sleep
     * arriving during bring-up cannot race the discovery path. */
    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, gMPSPowerStates, kMPSPowerStateCount);
    /* Keep the controller's normal operating vote at On. Provider/system
     * sleep can still remove power; an idle/default vote must not reset a
     * just-published disk immediately after registration. */
    changePowerStateTo(kMPSPowerOn);
    MPS_DEBUG_PROP("MPSPowerState", "on");
    /* The administrator IOUserClient is opened by matching this controller's
     * class, so the controller itself must be registered in the matching plane.
     * Target nubs already register themselves when they are published. */
    registerService();
    MPS_DEBUG_LOG("MPS start complete, irq=%u replyViaIrq=%u events=%u last=0x%04x\n",
          (unsigned int)fInterruptCount, (unsigned int)fReplyViaIrq,
          (unsigned int)fEventCount, (unsigned int)fLastEvent);
    return true;
}

void
MPSController::stop(IOService *provider)
{
    MPSBlockDev *nubs[kMPSMaxTargets + 1];
    UInt32 nubCount = 0;
    bool dmaStopped = false;

    fStopping = true;
    notifyReplyWaiters();
    stopAsync();
    if (fWatchdogTimer != NULL) {
        fWatchdogTimer->cancelTimeout();
        if (fWorkLoop != NULL) {
            fWorkLoop->removeEventSource(fWatchdogTimer);
        }
        fWatchdogTimer->release();
        fWatchdogTimer = NULL;
    }
    if (fHotplugCall != NULL) {
        /* The callback may be waiting for fIOLock. Never hold that lock
         * while waiting for the callback to finish. */
        thread_call_cancel_wait(fHotplugCall);
        thread_call_free(fHotplugCall);
        fHotplugCall = NULL;
    }
    if (fIOLock != NULL) drainRetiredNubs();
    if (fIOLock != NULL) {
        IOLockLock(fIOLock);
    }
    fMediaPresent = false;
    fDeviceOffline = true;
    fMediaGeneration++;
    /* Unbind every published target from the transport here, then hand the
     * nubs back to IOKit outside the lock: client termination can
     * synchronously re-enter the device. */
    for (UInt32 i = 0; i < kMPSMaxTargets; i++) {
        MPSTargetSlot *slot = &fTargets[i];
        if (slot->nub != NULL) {
            slot->nub->setController(NULL);
            if (nubCount < kMPSMaxTargets) nubs[nubCount++] = slot->nub;
            slot->nub = NULL;
        }
        slot->present = false;
        slot->probeValid = false;
        slot->online = false;
        slot->offlinePending = false;
        slot->onlinePending = false;
        slot->handle = 0;
    }

    /* No command submitter remains. Stop the reply consumer and the IOC
     * before releasing any host address previously supplied for DMA. */
    teardownInterrupts();
    if (fRegs != NULL) {
        dmaStopped = diagReset();
    }
    if (fPCIDevice != NULL) {
        fPCIDevice->setBusMasterEnable(false);
        UInt16 command = fPCIDevice->configRead16(kIOPCIConfigCommand);
        if (command != 0xFFFF && (command & kIOPCICommandBusMaster) == 0) {
            dmaStopped = true;
        }
    }
    if (dmaStopped) {
        resetProtocolState();
        if (fDataBuffer != NULL) {
            mpsReleaseResidentDma(fDataDma);
            fDataBuffer->release();
            fDataBuffer = NULL;
        }
        if (fPageBuffer != NULL) {
            mpsReleaseResidentDma(fPageDma);
            fPageBuffer->release();
            fPageBuffer = NULL;
        }
        fPagePhys = fDataPhys = 0;
        fDataBytes = 0;
        freeQueueMemory();
        freeControlBuffers(true);
        MPS_DEBUG_PROP("MPSStopDMAState", "stopped");
    } else if (fQueueBuffer != NULL || fReplyBuffer != NULL ||
               fRequestBuffer != NULL || fPageBuffer != NULL ||
                fDataBuffer != NULL || fSenseBuffer != NULL) {
        /* A bounded leak is safer than DMA into reallocated kernel memory.
         * Do not release these descriptors later without hardware proof. */
        MPS_DEBUG_PROP("MPSStopDMAState", "buffers-retained");
        MPS_DEBUG_LOG("MPS stop: DMA quiesce unconfirmed; buffers retained\n");
    }
    if (fMMIO != NULL) {
        fMMIO->release();
        fMMIO = NULL;
    }
    fRegs = NULL;
    fPCIDevice = NULL;
    fCapacityBlocks = 0;
    fBlockSize = 0;
    if (fIOLock != NULL) {
        IOLockUnlock(fIOLock);
    }
    releaseAsync(dmaStopped);

    /* Client termination can synchronously re-enter the device. The offline
     * gate is already closed and no controller lock is held here. IOKit
     * owns the eventual detach; do not detach a still-terminating child. */
    for (UInt32 i = 0; i < nubCount; i++) {
        nubs[i]->terminate();
        nubs[i]->release();
    }
    if (fWorkLoop != NULL) {
        fWorkLoop->release();
        fWorkLoop = NULL;
    }
    /* Leave the power tree only after the hardware is quiesced: PMstop()
     * waits for an in-flight power transition to finish. */
    PMstop();
    super::stop(provider);
}

/* Sleep/shutdown must quiesce all accepted work before the PCI provider
 * removes power. setPowerState runs on the PM thread, not the reply workloop. */
bool
MPSController::quiescePower(bool shutdown)
{
    if (fSuspended) return fPowerQuiesced;
    setAsyncAcceptance(false);
    IOReturn fence = beginAsyncBarrier();
    IOLockLock(fIOLock);
    /* Flush every target that this driver can write to. Each flush names its
     * own handle: a flush must leave one disk's cache ordered with that disk,
     * never with whichever target was published last. */
    IOReturn flush = fence;
    UInt32 flushed = 0;
    if (fence == kIOReturnSuccess && !fDeviceOffline &&
        !fProtocolRecoveryPending && !mediaRemovalPending()) {
        for (UInt32 i = 0; i < kMPSMaxTargets; i++) {
            MPSTargetSlot *slot = &fTargets[i];
            if (slot->handle == 0 || !slot->present || !slot->probeValid ||
                slot->managementOnly || slot->offlinePending) continue;
            if (!selectTarget(slot->key())) continue;
            IOReturn one = scsiSynchronizeCache(0, 0, 29, true);
            char name[48];
            snprintf(name, sizeof(name), "MPSTarget%uFlushStatus", (unsigned)i);
            MPS_DEBUG_PROP(name, (unsigned long long)(UInt32)one, 32);
            if (one != kIOReturnSuccess) {
                flush = one;
                break;
            }
            flushed++;
        }
    }
    MPS_DEBUG_PROP("MPSPowerFlushStatus", (unsigned long long)(UInt32)flush, 32);
    MPS_DEBUG_PROP("MPSPowerFlushCount", (unsigned long long)flushed, 32);
    if (shutdown && fence == kIOReturnSuccess && flush == kIOReturnSuccess &&
        !fProtocolRecoveryPending && !fStopping && !fMaintenanceMode) {
        bool clean = shutdownDevices();
        MPS_DEBUG_PROP("MPSShutdownDevices", clean ? "complete" : "incomplete");
    }
    fSuspended = true;
    teardownInterrupts();
    bool stopped = fRegs != NULL && diagReset();
    if (fPCIDevice != NULL) {
        fPCIDevice->setBusMasterEnable(false);
        UInt16 command = fPCIDevice->configRead16(kIOPCIConfigCommand);
        if (command != 0xFFFF && (command & kIOPCICommandBusMaster) == 0)
            stopped = true;
    }
    if (stopped) resetProtocolState();
    fPowerQuiesced = stopped;
    MPS_DEBUG_PROP("MPSPowerState", stopped ? "quiesced" : "quiesce-failed");
    IOLockUnlock(fIOLock);
    if (fence == kIOReturnSuccess) endAsyncBarrier();
    kickAsync(); // Reap cancelled held mappings only after the DMA-stop proof.
    return stopped;
}

bool
MPSController::resumePower(void)
{
    if (!fSuspended) return true;
    IOLockLock(fIOLock);
    /* Restore the PCI command register before the IOC can answer anything.
     * These helpers are called for their side effect: their bool result is not
     * a readiness proof on this controller. Sleep could not prove that DMA
     * stopped here, so fPowerQuiesced was false and the old gate skipped this
     * rebuild entirely. The first config-page read after wake then timed out
     * on SMID 4, quarantined the frame and forced the hotplug worker into a
     * fault-driven IOC reset. A wake must always put the IOC back into a
     * known state before any topology read; a quiesce that failed makes that
     * more necessary, not less. */
    bool pciReady = !fStopping && fPCIDevice != NULL;
    bool memoryEnabled = false, busMasterEnabled = false;
    if (pciReady) {
        memoryEnabled = fPCIDevice->setMemoryEnable(true);
        busMasterEnabled = fPCIDevice->setBusMasterEnable(true);
    }
    MPS_DEBUG_PROP("MPSWakeQuiesced", fPowerQuiesced);
    MPS_DEBUG_PROP("MPSWakeMemoryEnabled", memoryEnabled);
    MPS_DEBUG_PROP("MPSWakeBusMasterEnabled", busMasterEnabled);
    /* Config/event replies get a longer, bounded wait until the watchdog
     * closes this window. It is opened before the rebuild so the discovery
     * poll inside resetAndReinitialize() is covered too. */
    fWakeSettleTicks = kMPSWakeSettleTicks;
    bool ready = false;
    const char *rebuildState = "skipped-no-pci";
    if (pciReady) {
        ready = resetAndReinitialize();
        rebuildState = ready ? "ok" : "failed";
    }
    MPS_DEBUG_PROP("MPSWakeRebuildState", rebuildState);
    /* The fabric needs a moment after the IOC reset on wake: the expander and
     * the disks renegotiate their links, and probing them immediately made the
     * first probe fail and cost an extra reset. */
    if (ready) {
        IOSleep(1500);
        ready = ensureTargetsPublished();
    }
    /* A slow disk can still be spinning up after that first probe. A not-ready
     * target is not a failed wake: invalidating every medium here would force
     * an IOC reset that cannot make the disk spin any faster and would take
     * the healthy disk offline with it. Leave those probes to the hotplug
     * worker, which retries them at its own cooldown and reports ONLINE when
     * the media come back. */
    UInt32 notReady = 0;
    for (UInt32 i = 0; i < kMPSMaxTargets; i++) {
        MPSTargetSlot *slot = &fTargets[i];
        if (slot->handle != 0 && slot->present && !slot->probeValid &&
            slot->probeState == kMPSProbeNotReady) notReady++;
    }
    MPS_DEBUG_PROP("MPSWakeNotReadyCount", notReady, 32);
    /* Every medium that was published before sleep takes OFFLINE -> ONLINE
     * around this rebuild, whatever its identity: a disk inserted while asleep
     * must not be attached to the old medium. */
    for (UInt32 i = 0; i < kMPSMaxTargets; i++) {
        MPSTargetSlot *slot = &fTargets[i];
        if (slot->nub == NULL) continue;
        if (slot->present && slot->probeValid) slot->onlinePending = true;
        else slot->offlinePending = true;
    }
    fSuspended = false;
    fPowerQuiesced = false;
    /* The IOC can report a non-operational doorbell for a moment after wake;
     * that is a settling state, not a fault that needs its own reset. */
    fResumeGraceTicks = 4;
    IOSimpleLockLock(fReplyLock);
    fHotplugPending = true;
    IOSimpleLockUnlock(fReplyLock);
    MPS_DEBUG_PROP("MPSPowerState", ready ? "on" : "wake-recovery-pending");
    IOLockUnlock(fIOLock);
    drainRetiredNubs();
    setAsyncAcceptance(true);
    return ready;
}
IOReturn
MPSController::setPowerState(unsigned long stateNumber, IOService *whatDevice)
{
    (void)whatDevice;
    if (!fIOLock || fStarting || fStopping) return kIOPMAckImplied;
    if (stateNumber == kMPSPowerOff) quiescePower();
    else if (stateNumber == kMPSPowerOn) resumePower();
    return kIOPMAckImplied;
}

void
MPSController::systemWillShutdown(IOOptionBits specifier)
{
    if (!fStopping && fIOLock != NULL) quiescePower(true);
    super::systemWillShutdown(specifier);
}

/* ------------------------------------------------------------------------ */
/* write, SYNCHRONIZE CACHE, UNMAP, auto sense, TMF and diag reset.      */
/* SMID map: 1..4 config/events, 5..9 startup probes, 10..25 runtime IO,    */
/* 26..28 task management, 29 sync cache, 30 UNMAP, 31 event ACK;            */
/* 32..63 asynchronous I/O (independent mappings and fallback buffers).     */

#define MPS_SM_TMF_BASE         26
#define MPS_SM_SYNC_CACHE       29
#define MPS_SM_UNMAP            30
#define MPS_MAX_UNMAP_EXTENTS   32

IOReturn
MPSController::readBlocks(IOMemoryDescriptor *buffer, UInt64 block,
                          UInt64 nblks, UInt64 *actualBytes)
{
    return transferBlocks(false, buffer, block, nblks, false, actualBytes);
}

void
MPSController::postDefaultRequest(UInt32 smid)
{
    /* Config and EventAck messages use the default descriptor type;
     * only SCSI IO uses the fast-path descriptor (see postScsiIo). */
    /* ACK submission is already inside the reply lock. */
    if (smid == MPS_SM_EVENT_ACK) {
        postOwnedRequestLocked(smid, MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE, 0);
    } else {
        IOSimpleLockLock(fReplyLock);
        bool posted = postOwnedRequestLocked(smid, MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE, 0);
        if (!posted) recordProtocolFaultLocked(kMPSProtocolManagementBusy, smid);
        IOSimpleLockUnlock(fReplyLock);
    }
}

void
MPSController::postHighPriorityRequest(UInt32 smid)
{
    /* FreeBSD allocates TM requests from the high-priority pool
     * (mps_sas.c:224-236); the descriptor type is 0x06, not 0x08. */
    IOSimpleLockLock(fReplyLock);
    bool posted = postOwnedRequestLocked(smid, MPI2_REQ_DESCRIPT_FLAGS_HIGH_PRIORITY, 0);
    if (!posted) recordProtocolFaultLocked(kMPSProtocolManagementBusy, smid);
    IOSimpleLockUnlock(fReplyLock);
}

void
MPSController::releaseReplyFrame(UInt32 baddr)
{
    UInt32 *freeQueue;
    UInt32  offset;

    if (fQueueBuffer == NULL || fFqDepth == 0 || fReplyFrameSize == 0 ||
        fReplyPhys == 0) {
        return;
    }
    if (baddr < (UInt32)fReplyPhys ||
        baddr >= (UInt32)fReplyPhys + fReplyFrameSize * fFqDepth) {
        return;
    }
    offset = baddr - (UInt32)fReplyPhys;
    if ((offset % fReplyFrameSize) != 0) {
        return;
    }
    freeQueue = (UInt32 *)fQueueBuffer->getBytesNoCopy();
    if (freeQueue == NULL) {
        return;
    }
    /* Mirrors mps_free_reply(): pre-increment the index, write the frame
     * address back, then publish the new host index. */
    fReplyFreeIndex = (fReplyFreeIndex + 1) % fFqDepth;
    freeQueue[fReplyFreeIndex] = baddr;
    writeReg(MPI2_REPLY_FREE_HOST_INDEX_OFFSET, fReplyFreeIndex);
    fReplyFrameReturned++;
}

void
MPSController::sendEventAck(UInt16 event, UInt32 context)
{
    /* Called only while fReplyLock is held. Keep each context until its
     * command completes; never overwrite an IOC-owned request frame. */
    if (fEventAckQueued == kMPSEventAckQueueDepth) {
        fEventAckOverflow++;
        recordProtocolFaultLocked(kMPSProtocolAckOverflow, MPS_SM_EVENT_ACK);
        return;
    }
    UInt32 tail = (fEventAckHead + fEventAckQueued) % kMPSEventAckQueueDepth;
    fEventAckQueue[tail].event = event;
    fEventAckQueue[tail].context = context;
    fEventAckQueued++;
    postNextEventAckLocked();
}

void
MPSController::postNextEventAckLocked(void)
{
    if (fEventAckBusy || fEventAckQueued == 0 || fRequestBuffer == NULL ||
        fProtocolRecoveryPending || fStopping) {
        return;
    }
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (frame == NULL || MPS_SM_EVENT_ACK >= fRequestCount) {
        recordProtocolFaultLocked(kMPSProtocolAckReply, MPS_SM_EVENT_ACK);
        return;
    }
    if (fCommandOwners[MPS_SM_EVENT_ACK].state != kMPSCommandFree) {
        recordProtocolFaultLocked(kMPSProtocolManagementBusy, MPS_SM_EVENT_ACK);
        return;
    }
    fCommandOwners[MPS_SM_EVENT_ACK].state = kMPSCommandReserved;
    frame += MPS_SM_EVENT_ACK * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    MPSEventAckRequest *request = (MPSEventAckRequest *)frame;
    request->Function = MPI2_FUNCTION_EVENT_ACK;
    request->Event = fEventAckQueue[fEventAckHead].event;
    request->EventContext = fEventAckQueue[fEventAckHead].context;
    fEventAckBusy = true;
    fEventAckAge = 0;
    fEventAckCount++;
    postDefaultRequest(MPS_SM_EVENT_ACK);
}

void
MPSController::completeEventAckLocked(const UInt8 *reply, UInt32 replyBytes)
{
    UInt16 status = replyBytes >= 16
        ? (UInt16)((UInt16)reply[14] | ((UInt16)reply[15] << 8)) : 0xFFFF;
    if (!fEventAckBusy || fEventAckQueued == 0 || replyBytes < 20 ||
        fCommandOwners[MPS_SM_EVENT_ACK].state != kMPSCommandPosted ||
        reply[3] != MPI2_FUNCTION_EVENT_ACK ||
        (status & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS) {
        fEventAckFailed++;
        recordProtocolFaultLocked(kMPSProtocolAckReply, MPS_SM_EVENT_ACK);
        return;
    }
    fEventAckBusy = false;
    fCommandOwners[MPS_SM_EVENT_ACK].state = kMPSCommandFree;
    fEventAckAge = 0;
    fEventAckHead = (fEventAckHead + 1) % kMPSEventAckQueueDepth;
    fEventAckQueued--;
    fEventAckCompleted++;
    postNextEventAckLocked();
}

/* The probe chain is a sequence of synchronous SCSI commands, all issued under
 * fIOLock. validateScsiCompletion() publishes each decoded result here so
 * probeTarget() can tell a device that is still spinning up from one that
 * failed, without allocating or adding an out-parameter to every helper. */
void
MPSController::beginProbeStage(void)
{
    if (fProbeInProgress) fLastProbeStatus = kIOReturnSuccess;
}

void
MPSController::noteProbeStatus(IOReturn status)
{
    if (!fProbeInProgress || status == kIOReturnSuccess) return;
    if (status == kIOReturnNotReady || status == kIOReturnBusy) {
        /* A transient status is remembered only while nothing stronger has
         * been decoded for this stage. */
        if (fLastProbeStatus == kIOReturnSuccess ||
            fLastProbeStatus == kIOReturnNotReady ||
            fLastProbeStatus == kIOReturnBusy)
            fLastProbeStatus = status;
    } else {
        /* Illegal request, media or hardware error, aborted command, timeout,
         * malformed or short reply: a definite failure outranks not-ready. */
        fLastProbeStatus = status;
    }
}

void
MPSController::recordProbeResult(MPSTargetSlot *slot, MPSProbeResult result)
{
    if (slot == NULL) return;
    if (result == kMPSProbeOk) {
        slot->probeState = kMPSProbeOk;
        slot->probeRetries = 0;
        return;
    }
    slot->probeState = result;
    if (slot->probeRetries < kMPSMaxProbeRetries) slot->probeRetries++;
}

IOReturn
MPSController::decodeSense(UInt32 smid, UInt32 senseCount, IOReturn fallback,
                            bool *unitAttention)
{
    if (unitAttention != NULL) *unitAttention = false;
    if (smid == 0 || smid >= MPS_REQUEST_FRAMES || fSenseBuffer == NULL ||
        senseCount > MPS_SENSE_BYTES) return fallback;
    const UInt8 *base = (const UInt8 *)fSenseBuffer->getBytesNoCopy();
    if (base == NULL) return fallback;
    const UInt8 *sense = base + (UInt64)smid * MPS_SENSE_BYTES;
    MPSSenseFields fields;
    if (!mpsDecodeSenseFields(sense, senseCount, &fields)) return fallback;
    fLastSenseKey = fields.key;
    fLastASC = fields.asc;
    fLastASCQ = fields.ascq;
    MPS_DEBUG_PROP("MPSSenseKey", (unsigned long long)fields.key, 8);
    MPS_DEBUG_PROP("MPSSenseASC", (unsigned long long)fields.asc, 8);
    MPS_DEBUG_PROP("MPSSenseASCQ", (unsigned long long)fields.ascq, 8);
    UInt8 response = sense[0] & 0x7f;
    if (unitAttention != NULL)
        *unitAttention = fields.key == MPS_SENSE_KEY_UNIT_ATTENTION &&
                         (response == 0x70 || response == 0x72);
    switch (fields.key) {
        case MPS_SENSE_KEY_NO_SENSE:
        case MPS_SENSE_KEY_RECOVERED_ERROR: return kIOReturnSuccess;
        case MPS_SENSE_KEY_NOT_READY:
        case MPS_SENSE_KEY_UNIT_ATTENTION: return kIOReturnNotReady;
        case MPS_SENSE_KEY_MEDIUM_ERROR:
        case MPS_SENSE_KEY_BLANK_CHECK: return kIOReturnBadMedia;
        case MPS_SENSE_KEY_HARDWARE_ERROR: return kIOReturnDeviceError;
        case MPS_SENSE_KEY_ILLEGAL_REQUEST: return kIOReturnUnsupported;
        case MPS_SENSE_KEY_DATA_PROTECT: return kIOReturnNotWritable;
        case MPS_SENSE_KEY_ABORTED_COMMAND: return kIOReturnAborted;
        default: return fallback;
    }
}

bool
MPSController::validateScsiCompletion(UInt32 smid, UInt8 kind,
                                      const UInt8 *replyBuf, UInt32 dataLen,
                                      bool allowShort, IOReturn *status,
                                      UInt32 *actualBytes, bool *unitAttention)
{
    IOReturn result = kIOReturnIOError;
    UInt32 transferred = 0;
    if (actualBytes != NULL) *actualBytes = 0;
    if (unitAttention != NULL) *unitAttention = false;
    if (status != NULL) *status = result;
    if (kind == 3) {
        if (status != NULL) *status = kIOReturnAborted;
        noteProbeStatus(kIOReturnAborted);
        return false;
    }
    if (smid == 0 || smid >= MPS_REQUEST_FRAMES) {
        noteProbeStatus(result);
        return false;
    }
    if (kind == 2) {
        result = kIOReturnSuccess;
        transferred = dataLen;
    } else {
        if (kind != 1 || replyBuf == NULL ||
            replyBuf[3] != MPI2_FUNCTION_SCSI_IO_REQUEST ||
            replyBuf[2] * 4U < MPS_SCSIIO_REPLY_MIN_BYTES) {
            recordProtocolFault(kMPSProtocolManagementReply, smid);
            noteProbeStatus(result);
            return false;
        }
        const MPSScsiIoReply *reply = (const MPSScsiIoReply *)replyBuf;
        const UInt16 ioc = reply->IOCStatus & MPI2_IOCSTATUS_MASK;
        MPSTargetSlot *completionTarget = findTarget(mpsTargetKey(
            fCommandOwners[smid].handle, fCommandOwners[smid].lun));
        if (completionTarget && (reply->SCSIState & MPI2_SCSI_STATE_RESPONSE_INFO_VALID) &&
            (reply->ResponseInfo & MPI2_SCSI_RI_MASK_REASONCODE) == 2) {
            completionTarget->tlrEnabled = false;
            MPS_DEBUG_LOG("MPS: TLR disabled after invalid frame handle=0x%04x lun=%u\n",
                          completionTarget->handle, (unsigned)completionTarget->lun);
        }
        transferred = reply->TransferCount;
        MPS_DEBUG_PROP("MPSLastIOSmid", (unsigned long long)smid, 16);
        MPS_DEBUG_PROP("MPSLastIOIOCStatus", (unsigned long long)ioc, 16);
        MPS_DEBUG_PROP("MPSLastIOSCSIStatus", (unsigned long long)reply->SCSIStatus, 8);
        MPS_DEBUG_PROP("MPSLastIOTransferCount", (unsigned long long)transferred, 32);
        MPS_DEBUG_PROP("MPSLastIOSCSIState", (unsigned long long)reply->SCSIState, 8);
        MPS_DEBUG_PROP("MPSLastIOSenseCount", (unsigned long long)reply->SenseCount, 32);
        if (reply->DevHandle != fCommandOwners[smid].handle) {
            recordProtocolFault(kMPSProtocolUnexpectedCompletion, smid);
            result = kIOReturnDeviceError;
        } else if (transferred > dataLen) {
            result = kIOReturnOverrun;
        } else if (reply->SenseCount > MPS_SENSE_BYTES ||
                   (reply->SCSIState & (MPI2_SCSI_STATE_NO_SCSI_STATUS |
                                       MPI2_SCSI_STATE_AUTOSENSE_FAILED)) != 0) {
            result = kIOReturnDeviceError;
        } else if ((reply->SCSIState & MPI2_SCSI_STATE_TERMINATED) != 0) {
            result = kIOReturnAborted;
        } else if ((reply->SCSIState & MPI2_SCSI_STATE_RESPONSE_INFO_VALID) != 0 &&
                   (reply->ResponseInfo & MPI2_SCSI_RI_MASK_REASONCODE) != 0) {
            result = kIOReturnDeviceError;
        } else if (ioc != MPI2_IOCSTATUS_SUCCESS &&
                   ioc != MPI2_IOCSTATUS_SCSI_RECOVERED_ERROR &&
                   ioc != MPI2_IOCSTATUS_SCSI_DATA_UNDERRUN) {
            result = ioc == MPI2_IOCSTATUS_SCSI_DATA_OVERRUN
                ? kIOReturnOverrun : kIOReturnDeviceError;
        } else if (reply->SCSIStatus == MPI2_SCSI_STATUS_CHECK_CONDITION) {
            result = (reply->SCSIState & MPI2_SCSI_STATE_AUTOSENSE_VALID)
                ? decodeSense(smid, reply->SenseCount, kIOReturnDeviceError,
                              unitAttention) : kIOReturnDeviceError;
        } else if (reply->SCSIStatus == MPI2_SCSI_STATUS_BUSY ||
                   reply->SCSIStatus == MPI2_SCSI_STATUS_TASK_SET_FULL ||
                   reply->SCSIStatus == MPI2_SCSI_STATUS_RESERVATION_CONFLICT) {
            result = kIOReturnBusy;
        } else if (reply->SCSIStatus == MPI2_SCSI_STATUS_TASK_ABORTED) {
            result = kIOReturnAborted;
        } else if (reply->SCSIStatus == MPI2_SCSI_STATUS_GOOD) {
            result = kIOReturnSuccess;
        }
        /* An underrun can carry CHECK CONDITION (for example UNIT ATTENTION
         * or ILLEGAL REQUEST), so decode its sense before applying the exact
         * length rule. Only explicit short-read probes may succeed short. */
        if (result == kIOReturnSuccess && !allowShort &&
            (transferred != dataLen || ioc == MPI2_IOCSTATUS_SCSI_DATA_UNDERRUN))
            result = kIOReturnUnderrun;
    }
    if (actualBytes != NULL && transferred <= dataLen) *actualBytes = transferred;
    if (status != NULL) *status = result;
    MPS_DEBUG_PROP("MPSLastCommandStatus", (unsigned long long)(UInt32)result, 32);
    noteProbeStatus(result);
    return result == kIOReturnSuccess;
}

bool
MPSController::issueScsiCommand(const UInt8 *cdb, UInt32 cdbLen,
                                UInt32 control, const UInt8 *outData,
                                UInt32 dataLen, UInt32 smid,
                                IOReturn *status, UInt32 *actualBytes,
                                bool allowShort, bool *unitAttention)
{
    UInt8 *frame, *staging;
    MPSScsiIoRequest *request;
    bool posted;
    UInt8 replyBuf[64], replyKind = 0;

    if (status != NULL) *status = kIOReturnNotReady;
    if (actualBytes != NULL) *actualBytes = 0;
    if (unitAttention != NULL) *unitAttention = false;
    if ((fDeviceOffline && !fReinitInProgress) || fStopping ||
        fProtocolRecoveryPending) {
        if (status != NULL) {
            *status = kIOReturnDeviceError;
        }
        return false;
    }
    if (fRequestBuffer == NULL || fPageBuffer == NULL || fDataBuffer == NULL ||
        cdb == NULL || cdbLen == 0 || cdbLen > 32 || smid == 0 ||
        smid >= fRequestCount || smid >= 128 || smid == MPS_SM_EVENT_ACK ||
        fReplyLock == NULL || dataLen > fDataBytes ||
        (allowShort && control != MPI2_SCSIIO_CONTROL_READ) ||
        fAttachedDevHandle == 0) {
        return false;
    }
    if (fFrozenSmid != 0 && fFrozenSmid == (UInt16)smid) {
        if (status != NULL) {
            *status = kIOReturnAborted;
        }
        return false;
    }

    frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    staging = (UInt8 *)fDataBuffer->getBytesNoCopy();
    if (frame == NULL || staging == NULL) {
        return false;
    }
    if (smid < 10) {
        if (!beginManagementRequest(smid)) {
            return false;
        }
    } else if (commandDmaSafe(smid)) {
        /* Runtime management callers are serialized by fIOLock. No waiter
         * from this caller exists yet, so retire an orphaned late completion. */
        UInt8 oldReply[64], oldKind = 0;
        takeReplySlot(smid, oldReply, sizeof(oldReply), &oldKind);
    }
    if (!prepareScsiSense(smid)) {
        cancelUnpostedCommand(smid);
        return false;
    }
    frame += smid * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    request = (MPSScsiIoRequest *)frame;

    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SCSI_IO_REQUEST;
    request->SenseBufferLowAddress = (UInt32)senseAddress(smid);
    request->SenseBufferLength = MPS_SENSE_BYTES;
    request->SGLOffset0 = MPS_SCSIIO_SGE_DWORDS;
    request->DataLength = dataLen;
    request->IoFlags = (UInt16)cdbLen;
    request->Control = control;
    memcpy(request->CDB, cdb, cdbLen);

    if (dataLen != 0) {
        if (outData != NULL) {
            memcpy(staging, outData, dataLen);
        } else {
            memset(staging, 0, dataLen);
        }
        mpsMpiSimple(frame + MPS_SCSIIO_SGE_DWORDS * 4, fDataPhys, dataLen, (request->Control & MPI2_SCSIIO_CONTROL_WRITE) != 0);
    } else {
        /* A zero-length MPI SGE is still required for a data-less command. */
        mpsMpiSimple(frame + MPS_SCSIIO_SGE_DWORDS * 4, fPagePhys, 0, (request->Control & MPI2_SCSIIO_CONTROL_WRITE) != 0);
    }

    fCommandCount++;
    if (!postScsiIo(smid)) {
        if (status != NULL) *status = kIOReturnNoMedia;
        return false;
    }
    posted = smid < 10
        ? finishManagementRequest(smid, 15000, replyBuf, &replyKind)
        : (waitForSmid(15000, smid) &&
           takeReplySlot(smid, replyBuf, sizeof(replyBuf), &replyKind));
    if (!posted) {
        if (status != NULL) {
            *status = kIOReturnTimeout;
        }
        if (smid >= 10 && fTimedOutSmid == (UInt16)smid) {
            if (fReinitInProgress || fStarting) {
                recordProtocolFault(kMPSProtocolManagementTimeout, smid);
            } else {
                recoverTimedOutCommand(smid);
            }
        }
        if (smid >= 10 && commandDmaSafe(smid))
            takeReplySlot(smid, replyBuf, sizeof(replyBuf), &replyKind);
        return false;
    }
    return validateScsiCompletion(smid, replyKind, replyBuf, dataLen,
                                  allowShort, status, actualBytes, unitAttention);
}

/* Target protocol implementation is in MPSTarget.cpp. */

/* Target protocol implementation is in MPSTarget.cpp. */

/* Target protocol implementation is in MPSTarget.cpp. */

bool
MPSController::sendTaskMgmt(UInt8 taskType, UInt16 taskMid, UInt32 smid,
                            UInt16 handle, UInt16 *iocStatus, UInt32 lun)
{
    /* A task-management request names its target explicitly: resetting or
     * aborting through the controller's default handle would hit a different,
     * healthy disk. `iocStatus` stays 0xFFFF unless a well-formed reply
     * arrived, which lets a caller distinguish "the firmware answered with an
     * error" from "no completion was ever seen". */
    if (iocStatus != NULL) *iocStatus = 0xFFFF;
    if (fRequestBuffer == NULL || handle == 0 ||
        smid < MPS_SM_TMF_BASE || smid >= MPS_SM_TMF_BASE + 3 ||
        smid >= fRequestCount || fReplyLock == NULL || fProtocolRecoveryPending)
        return false;
    UInt8 *base = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (base == NULL) return false;
    IOSimpleLockLock(fReplyLock);
    UInt32 outstanding = 0;
    for (UInt32 i = 0; i < 3; ++i) if (fTmfBusy[i]) ++outstanding;
    bool available = outstanding < fHighPriorityCredit &&
        !fTmfBusy[smid - MPS_SM_TMF_BASE] &&
        fCommandOwners[smid].state == kMPSCommandFree;
    if (available) {
        fTmfBusy[smid - MPS_SM_TMF_BASE] = true;
        fCommandOwners[smid].state = kMPSCommandReserved;
        fCommandOwners[smid].epoch = fTransportEpoch;
        fReplySlots[smid].valid = 0;
    }
    IOSimpleLockUnlock(fReplyLock);
    if (!available) {
        MPS_DEBUG_PROP("MPSTmfState", "no-free-high-priority-credit");
        return false;
    }
    UInt8 *frame = base + (UInt64)smid * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    MPSScsiTaskMgmtRequest *request = (MPSScsiTaskMgmtRequest *)frame;
    request->DevHandle = handle;
    request->Function = MPI2_FUNCTION_SCSI_TASK_MGMT;
    request->TaskType = taskType;
    if (taskType != MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET)
        (void)mpsEncodeLun(request->LUN, lun);
    MPSTargetSlot *target = findTarget(mpsTargetKey(handle, lun));
    request->MsgFlags = MPI2_SCSITASKMGMT_MSGFLAGS_LINK_RESET;
    UInt32 timeoutMs = 15000;
    MPS_DEBUG_LOG("MPS: TMF handle=0x%04x kind=%u task=%u flags=0x%x timeout=%ums\n",
        handle, target ? (unsigned)target->kind : 0U, taskType, request->MsgFlags,
        (unsigned)timeoutMs);
    request->TaskMID = taskType == MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK
        ? taskMid : 0;
    fTmfCount++;
    postHighPriorityRequest(smid);
    UInt8 replyBuf[64], replyKind = 0;
    bool completed = waitForSmid(timeoutMs, smid) &&
        takeReplySlot(smid, replyBuf, sizeof(replyBuf), &replyKind);
    if (!completed) {
        IOSimpleLockLock(fReplyLock);
        if (fCommandOwners[smid].state == kMPSCommandPosted)
            fCommandOwners[smid].state = kMPSCommandQuarantined;
        IOSimpleLockUnlock(fReplyLock);
        MPS_DEBUG_PROP("MPSTmfState", "reply-timeout-credit-retained");
        return false;
    }
    IOSimpleLockLock(fReplyLock);
    fTmfBusy[smid - MPS_SM_TMF_BASE] = false;
    IOSimpleLockUnlock(fReplyLock);
    const MPSScsiTaskMgmtReply *reply = (const MPSScsiTaskMgmtReply *)replyBuf;
    if (replyKind != 1 || replyBuf[3] != MPI2_FUNCTION_SCSI_TASK_MGMT ||
        replyBuf[2] * 4U < sizeof(MPSScsiTaskMgmtReply) ||
        reply->DevHandle != handle || reply->TaskType != taskType) {
        recordProtocolFault(kMPSProtocolUnexpectedCompletion, smid);
        return false;
    }
    const UInt16 status = reply->IOCStatus & MPI2_IOCSTATUS_MASK;
    if (iocStatus != NULL) *iocStatus = status;
    bool ok = status == MPI2_IOCSTATUS_SUCCESS &&
        (reply->ResponseCode == MPI2_SCSITASKMGMT_RSP_TM_COMPLETE ||
         reply->ResponseCode == MPI2_SCSITASKMGMT_RSP_TM_SUCCEEDED);
    MPS_DEBUG_PROP("MPSTmfResponseCode", (unsigned long long)reply->ResponseCode, 8);
    MPS_DEBUG_PROP("MPSTmfIOCStatus", (unsigned long long)status, 16);
    MPS_DEBUG_PROP("MPSTmfTerminationCount", (unsigned long long)reply->TerminationCount, 32);
    MPS_DEBUG_PROP("MPSTmfTaskType", (unsigned long long)taskType, 8);
    MPS_DEBUG_PROP("MPSTmfHandle", (unsigned long long)handle, 16);
    MPS_DEBUG_PROP("MPSTmfState", ok ? "ok" : "failed");
    return ok;
}

bool
MPSController::drainTaskManagedCommands(UInt16 handle, UInt16 taskMid,
                                        bool wholeTarget, UInt32 lun)
{
    /* A TMF reply does not consume the original SCSI reply. Do not recycle
     * its SMID or sense/data mapping until that completion has been observed. */
    for (UInt32 elapsed = 0; elapsed < 1000 && !fStopping; ++elapsed) {
        processReplyQueue();
        IOSimpleLockLock(fReplyLock);
        bool pending = false;
        for (UInt32 id = 1; id < fRequestCount && id < MPS_REQUEST_FRAMES; ++id) {
            const MPSCommandOwner *owner = &fCommandOwners[id];
            if ((owner->function == MPI2_FUNCTION_SCSI_IO_REQUEST) &&
                owner->handle == handle && (lun == UINT32_MAX || owner->lun == lun) &&
                (wholeTarget || id == taskMid) &&
                (owner->state == kMPSCommandPosted ||
                 owner->state == kMPSCommandQuarantined)) pending = true;
        }
        bool fault = fProtocolRecoveryPending;
        IOSimpleLockUnlock(fReplyLock);
        if (!pending && !fault) return true;
        if (fault) return false;
        IOSleep(1);
    }
    return false;
}

bool
MPSController::recoverTimedOutCommand(UInt32 smid)
{
    static const UInt8 levels[3] = {
        MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK,
        MPI2_SCSITASKMGMT_TASKTYPE_LUN_RESET,
        MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET
    };
    UInt32 i, injectFail;
    bool recovered = false;
    bool controllerReset = false;

    if (smid == 0) {
        return false;
    }
    /* The failed command belongs to exactly one target. Read its handle from
     * the command owner before the SMID is frozen, so every escalation level
     * names that target instead of the controller's current default. */
    UInt16 handle = 0;
    UInt32 lun = 0, targetGeneration = 0;
    UInt64 targetIdentity = 0;
    UInt8 targetKind = 0;
    IOSimpleLockLock(fReplyLock);
    if (smid < 128 &&
        fCommandOwners[smid].function == MPI2_FUNCTION_SCSI_IO_REQUEST)
    {
        handle = fCommandOwners[smid].handle;
        lun = fCommandOwners[smid].lun;
        targetIdentity = fCommandOwners[smid].targetIdentity;
        targetGeneration = fCommandOwners[smid].targetGeneration;
        targetKind = fCommandOwners[smid].targetKind;
    }
    IOSimpleLockUnlock(fReplyLock);
    if (handle == 0) handle = (UInt16)fAttachedDevHandle;
    MPSTargetSlot *slot = findTarget(mpsTargetKey(handle, lun));
    const bool sameBinding = slot && targetIdentity && slot->sasAddress == targetIdentity &&
        slot->generation == targetGeneration && (UInt8)slot->kind == targetKind;
    if (!sameBinding)
        MPS_DEBUG_LOG("MPS: stale command binding; skip device TMF, require IOC DMA-stop smid=%u\n", (unsigned)smid);
    if (slot != NULL) {
        slot->failureCount++;
        publishTargetProperties();
    }

    /* Freeze the SMID until the firmware confirms the task is gone; it must
     * not write a late reply into a reused request frame (risk R6). */
    fFrozenSmid = (UInt16)smid;
    MPS_DEBUG_PROP("MPSFrozenSmid", (unsigned long long)smid, 16);

    /* mps_tmf_fail=N makes the first N levels look unsuccessful, so the next
     * level is really submitted. N=1 exercises LUN_RESET, N=2 TARGET_RESET,
     * N=3 falls through to resetAndReinitialize(). It applies once. */
    injectFail = fInjectTmfFail;
    fInjectTmfFail = 0;
    if (injectFail != 0) {
        MPS_DEBUG_PROP("MPSTmfInjectedFail", (unsigned long long)injectFail, 8);
    }

    for (i = 0; sameBinding && i < 3 && !recovered; i++) {
        if (injectFail > i) {
            IOSleep(50);
            continue;
        }
        /* A reply that carries an error still proves the firmware accepted the
         * request; the drain is what proves the command is gone. Without a
         * reply, the next escalation level is tried. */
        if (sendTaskMgmt(levels[i], (UInt16)smid, MPS_SM_TMF_BASE + i, handle, NULL, lun) &&
            drainTaskManagedCommands(handle, (UInt16)smid, i != 0, i == 2 ? UINT32_MAX : lun)) {
            recovered = true;
        } else {
            IOSleep(50);
        }
    }
    if (!recovered) {
        controllerReset = true;
        MPS_DEBUG_PROP("MPSResetInitiator", "io-timeout");
        recovered = resetAndReinitialize();
    }
    bool removed;
    IOSimpleLockLock(fReplyLock);
    removed = fRemovalPending;
    IOSimpleLockUnlock(fReplyLock);
    if (controllerReset || removed || !recovered) {
        /* An IOC reset unbinds every medium and a removal invalidates one; the
         * unlocked worker performs OFFLINE -> ONLINE per target before clients
         * issue new requests against the new media generation. */
        invalidateTargets();
        IOSimpleLockLock(fReplyLock);
        fHotplugPending = true;
        IOSimpleLockUnlock(fReplyLock);
        if (!recovered) {
            recordProtocolFault(kMPSProtocolManagementTimeout, smid);
        }
        MPS_DEBUG_PROP("MPSRecoveryState", recovered ? "media-revalidation" : "failed");
        return false;
    }
    if (recovered) {
        fFrozenSmid = 0;
        fDeviceOffline = false;
        MPS_DEBUG_PROP("MPSFrozenSmid", 0ULL, 16);
        MPS_DEBUG_PROP("MPSDeviceOffline", false);
    } else {
        /* Recovery failed: keep the SMID frozen and the device offline. The
         * firmware may still own the original request frame (risk R6). */
        fDeviceOffline = true;
        MPS_DEBUG_PROP("MPSDeviceOffline", true);
    }
    MPS_DEBUG_PROP("MPSRecoveryState", recovered ? "ok" : "failed");
    return recovered;
}

void
MPSController::watchdogTimeout(IOTimerEventSource *sender)
{
    if (fStopping) return;
    /* Startup can still be constructing or unwinding fAsync while this
     * timer is armed. Runtime ownership begins only after bring-up finishes. */
    if (!fStarting) {
        kickAsync(); /* Reap held maps after a late reply or a proved reset. */
        publishAsyncStats();
    }
    if (fResumeGraceTicks != 0) fResumeGraceTicks--;
    /* The window must stay open for the whole wake rebuild: a long discovery
     * can hold one PortEnable request for tens of seconds. It only starts
     * counting down once the wake path has re-enabled acceptance. */
    if (fWakeSettleTicks != 0 && !fSuspended && !fReinitInProgress)
        fWakeSettleTicks--;
    MPS_DEBUG_PROP("MPSWakeSettleTicks", (unsigned long long)fWakeSettleTicks, 32);
    if (!fStarting && !fReinitInProgress && !fSuspended && fRegs != NULL &&
        fResumeGraceTicks == 0) {
        UInt32 state = readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK;
        MPS_DEBUG_PROP("MPSWatchdogIOCState", (unsigned long long)state, 32);
        if (state != MPI2_IOC_STATE_OPERATIONAL)
            recordProtocolFault(kMPSProtocolIocNotOperational, 0);
    }
    serviceProtocolState();
    UInt32 batches, descriptors, maxBatch, indexWrites;
    IOSimpleLockLock(fReplyLock);
    batches = fReplyBatches;
    descriptors = fReplyDescriptors;
    maxBatch = fReplyMaxBatch;
    indexWrites = fReplyIndexWrites;
    IOSimpleLockUnlock(fReplyLock);
    MPS_DEBUG_PROP("MPSReplyBatches", (unsigned long long)batches, 32);
    MPS_DEBUG_PROP("MPSReplyDescriptors", (unsigned long long)descriptors, 32);
    MPS_DEBUG_PROP("MPSReplyMaxBatch", (unsigned long long)maxBatch, 32);
    MPS_DEBUG_PROP("MPSReplyIndexWrites", (unsigned long long)indexWrites, 32);
    MPS_DEBUG_PROP("MPSHardwareReplyQueues", 1ULL, 32);
    bool schedule = false;
    if (fReplyLock != NULL) {
        IOSimpleLockLock(fReplyLock);
        if (fHotplugCooldown != 0) fHotplugCooldown--;
        if (fRemovedMediaCount != 0) {
            fRemovedAgeTicks++;
            /* Without SMP there is no per-phy reset behind an expander, and a
             * disk that returns is only re-listed by a fresh PortEnable
             * discovery. Spend at most two resets on that, at 8 s and 45 s
             * after the medium was retired. */
            if ((fRemovedAgeTicks == 8 || fRemovedAgeTicks == 45) &&
                fRediscoveryResets < 2) {
                fRediscoverRequest = true;
                fHotplugPending = true;
            }
        } else {
            fRemovedAgeTicks = 0;
            fRediscoveryResets = 0;
            fRediscoverRequest = false;
        }
        /* Discovery cannot depend on firmware events alone: an expander can
         * reattach a disk without reporting an add, and a device that was
         * removed by REMOVE_DEVICE is only re-listed by enumeration. A slow
         * periodic snapshot keeps hotplug event-independent at a bounded cost. */
        if (++fIdleScanTicks >= kMPSIdleScanTicks) {
            fIdleScanTicks = 0;
            if (!fStarting && !fReinitInProgress && !fHotplugRunning && !fSuspended)
                fHotplugPending = true;
        }
        if (!fStarting && !fReinitInProgress && !fHotplugRunning &&
            !fSuspended && fHotplugPending && fHotplugCooldown == 0 &&
            fHotplugCall != NULL) {
            /* Consume only this scheduling token. An event arriving while
             * the worker runs sets it again and must survive completion. */
            fHotplugPending = false;
            fHotplugRunning = true;
            schedule = true;
        }
        IOSimpleLockUnlock(fReplyLock);
    }
    MPS_DEBUG_PROP("MPSWatchdogState", fProtocolRecoveryPending ? "protocol-recovery" :
                fTopologyChanged ? "topology-change" :
                !fMediaPresent ? "offline" : "ok");
    publishTargetCounters();
    if (schedule) thread_call_enter(fHotplugCall);
    if (sender != NULL) sender->setTimeoutMS(1000);
}

void
MPSController::hotplugCall(thread_call_param_t param0,
                           thread_call_param_t param1)
{
    (void)param1;
    ((MPSController *)param0)->hotplugThread();
}

bool
MPSController::mediaRemovalPending(void)
{
    IOSimpleLockLock(fReplyLock);
    bool pending = fRemovalPending;
    IOSimpleLockUnlock(fReplyLock);
    return pending;
}

void
MPSController::hotplugThread(void)
{
    struct MPSNotifyAction {
        UInt32 index;
        MPSBlockDev *nub;
        bool online;
        IOReturn status;
    };
    MPSNotifyAction actions[2 * kMPSMaxTargets];
    UInt32 generation = 0, actionCount = 0, live = 0, cooldown = 1;
    bool retry = false, resetNeeded = false, probeFailed = false, dirty = false;
    bool publishedNew = false;
    bool retryRequested = false, physicalChange = false;
    UInt16 pendingRemoval = 0;
    const char *state = "stopped";

    if (fIOLock == NULL) return;
    IOLockLock(fIOLock);
    IOSimpleLockLock(fReplyLock);
    generation = fTopologyGeneration;
    if (fStopping || fStarting || fSuspended) {
        /* A queued worker may run after PM quiesced the device. Preserve
         * every physical/IR latch until wake can actually examine it. */
        if (!fStopping) {
            fHotplugPending = true;
            retry = true;
        }
        IOSimpleLockUnlock(fReplyLock);
        goto done;
    }
    if (fFirmwareDownloadActive && !fProtocolRecoveryPending) {
        fHotplugPending = true;
        retry = true;
        state = "firmware-download-in-progress";
        IOSimpleLockUnlock(fReplyLock);
        goto done;
    }
    fRemovalPending = false;
    retryRequested = fHotplugRetryRequested;
    fHotplugRetryRequested = false;
    physicalChange = fPhysicalChangePending;
    fPhysicalChangePending = false;
    pendingRemoval = fPendingRemovalHandle;
    fPendingRemovalHandle = 0;
    IOSimpleLockUnlock(fReplyLock);

    /* The IOC may reuse both handle and SATA synthetic address within one
     * scan interval. Retain historical identity but revoke every candidate's
     * probe/generation before reopening admission. The interrupt context never
     * reads the mutable target table. This conservative policy also covers
     * non-default targets and several coalesced physical events. */
    if (physicalChange) {
        for (UInt32 i = 0; i < kMPSMaxTargets; ++i)
            if (fTargets[i].handle || fTargets[i].nub) invalidateTarget(&fTargets[i]);
        dirty = true;
    }

    /* 1. One topology snapshot, merged into the persistent target table.
     * A handle that is still listed keeps its nub and medium; a vanished one
     * is marked for the guarded removal sequence. */
    if (!allocatePageBuffer() || !readSasIOUnitPage0()) {
        if (fProtocolRecoveryPending) {
            /* A protocol fault blocks every config-page read and every posted
             * command. An I/O waiter would run its own reset under this lock,
             * so a fault still pending here has no owner: drive one bounded
             * reset, or the controller stays frozen with healthy targets in
             * the table. */
            fFaultRecoveries++;
            MPS_DEBUG_PROP("MPSResetInitiator", "protocol-fault");
            bool ok = resetAndReinitialize();
            MPS_DEBUG_PROP("MPSFaultRecoveryState", ok ? "ok" : "failed");
            MPS_DEBUG_PROP("MPSFaultRecoveryCount",
                        (unsigned long long)fFaultRecoveries, 32);
            cooldown = 5;
            retry = true;
            state = ok ? "fault-recovered" : "fault-recovery-failed";
            goto done;
        }
        state = "topology-read-failed";
        retry = true;
        goto done;
    }
    /* A physical change (or a new insertion outside an IOC reset) restores the
     * single probe budget and the reset budget, so a disk plugged back in is
     * tried again. */
    if (physicalChange || retryRequested) {
        for (UInt32 i = 0; i < kMPSMaxTargets; i++) {
            fTargets[i].probeRetries = 0;
            fTargets[i].probeState = kMPSProbeOk;
        }
        fHotplugAttempts = 0;
    }
    /* A TARGET_NOT_RESPONDING event can name a handle that GET_NEXT_HANDLE
     * still lists. Promote that latch into the table after the scan, which is
     * the only context allowed to change it. */
    if (pendingRemoval != 0) {
        MPSTargetSlot *slot = findTarget(pendingRemoval);
        if (slot != NULL) requestTargetRemoval(slot);
    }

    /* 2. Retire what the fabric dropped: TARGET_RESET, drain of the aborted
     * commands, then REMOVE_DEVICE, each naming this slot's handle. A removal
     * that cannot be proved is this target's failure, not a controller
     * protocol fault, and escalates to the bounded IOC reset. */
    for (UInt32 i = 0; i < kMPSMaxTargets; i++) {
        MPSTargetSlot *slot = &fTargets[i];
        if (!slot->removalRequested) continue;
        dirty = true;
        if (!retireTarget(slot)) {
            fRemovalFailures++;
            MPS_DEBUG_PROP("MPSTargetRemovalFailures",
                        (unsigned long long)fRemovalFailures, 32);
            MPS_DEBUG_PROP("MPSRemovalStage", slot->removalState);
            resetNeeded = true;
            break;
        }
    }

    /* 3. Probe and publish every present target that is not ready. One
     * per-target budget covers a slow spin-up and a broken disk alike: a
     * not-ready pass only asks for another scan, while a definite failure (or
     * a device that never became ready) uses the bounded IOC reset and then
     * converges to offline. Probing stays serialized by fIOLock, but no sleep
     * happens under it: the one-second cooldown paces every retry. */
    if (!resetNeeded && !fProtocolRecoveryPending) {
        for (UInt32 i = 0; i < kMPSMaxTargets; i++) {
            MPSTargetSlot *slot = &fTargets[i];
            if (slot->handle == 0 || !slot->present || (slot->managementOnly && slot->peripheralType == 0)) continue;
            dirty = true;
            if (!slot->probeValid) {
                if (slot->probeRetries >= kMPSMaxProbeRetries) continue;
                MPSProbeResult result = probeTarget(slot);
                recordProbeResult(slot, result);
                if (result != kMPSProbeOk) {
                    if (result == kMPSProbeNotReady &&
                        slot->probeRetries < kMPSMaxProbeRetries) {
                        /* The device is on the fabric but still spinning up.
                         * It is neither a failure nor a reason to reset the
                         * IOC: retry at the worker's own cadence. */
                        retry = true;
                        continue;
                    }
                    /* A definite failure, or a not-ready device that used up
                     * the whole budget, follows the existing bounded policy. */
                    slot->probeState = kMPSProbeFailed;
                    slot->failureCount++;
                    if (slot->probeRetries >= kMPSProbeResetThreshold)
                        probeFailed = true;
                    else retry = true;
                    continue;
                }
            }
            if (slot->managementOnly || fMaintenanceMode) continue;
            const bool freshNub = (slot->nub == NULL);
            if (!publishTarget(slot)) {
                slot->failureCount++;
                probeFailed = true;
            } else if (freshNub) {
                publishedNew = true;
            }
        }
    }

    /* 3c. A disk that came back under a new handle leaves the retired slot
     * holding an offline nub and a table index. Both are dead weight once a
     * fresh medium was published; release them without the transport lock so
     * client termination may re-enter. */
    if (publishedNew) {
        UInt32 stale = 0;
        for (UInt32 i = 0; i < kMPSMaxTargets && stale < 2 * kMPSMaxTargets; i++) {
            MPSTargetSlot *slot = &fTargets[i];
            if (slot->nub == NULL || slot->handle != 0 || slot->offlinePending ||
                slot->removalRequested) continue;
            /* Transfer the table's owning reference to deferred termination. */
            actions[stale].index = i;
            actions[stale].nub = slot->nub;
            actions[stale].online = false;
            actions[stale].status = kIOReturnSuccess;
            stale++;
            memset(slot, 0, sizeof(*slot));
            dirty = true;
        }
        if (stale != 0) {
            IOLockUnlock(fIOLock);
            for (UInt32 a = 0; a < stale; a++) {
                actions[a].nub->terminate();
                actions[a].nub->release();
            }
            IOLockLock(fIOLock);
            MPS_DEBUG_PROP("MPSRetiredSlotsReleased",
                        (unsigned long long)stale, 32);
        }
    }

    /* 3b. A medium that left the fabric may return without an add event; the
     * watchdog asks for one bounded rediscovery reset after the removal. It
     * re-runs PortEnable discovery, which is the only way the IOC re-lists a
     * device behind an expander without per-phy SMP control. */
    if (fRediscoverRequest && !fStopping) {
        fRediscoverRequest = false;
        fRediscoveryResets++;
        MPS_DEBUG_PROP("MPSResetInitiator", "removed-rediscovery");
        bool ok = resetAndReinitialize();
        MPS_DEBUG_PROP("MPSRediscoveryState", ok ? "reset" : "failed");
        MPS_DEBUG_PROP("MPSRediscoveryCount",
                    (unsigned long long)fRediscoveryResets, 32);
        state = ok ? "rediscovery-reset" : "rediscovery-failed";
        retry = true;
        goto done;
    }

    /* 4. Deliver media-state changes per target. Notifications can re-enter the
     * transport, so they run with no lock held; OFFLINE precedes ONLINE for the
     * same nub. */
    for (UInt32 pass = 0; pass < 2; pass++) {
        actionCount = 0;
        for (UInt32 i = 0; i < kMPSMaxTargets && actionCount < 2 * kMPSMaxTargets; i++) {
            MPSTargetSlot *slot = &fTargets[i];
            if (slot->nub == NULL) continue;
            bool wanted;
            if (pass == 0) {
                wanted = slot->offlinePending;
            } else {
                wanted = slot->onlinePending && !slot->offlinePending &&
                         slot->probeValid && slot->present;
            }
            if (!wanted) continue;
            slot->nub->retain();
            actions[actionCount].index = i;
            actions[actionCount].nub = slot->nub;
            actions[actionCount].online = pass != 0;
            actions[actionCount].status = kIOReturnSuccess;
            actionCount++;
        }
        if (actionCount == 0) continue;
        dirty = true;
        IOLockUnlock(fIOLock);
        for (UInt32 a = 0; a < actionCount; a++) {
            actions[a].status = actions[a].nub->notifyMediaState(
                actions[a].online ? kIOMediaStateOnline : kIOMediaStateOffline);
        }
        IOLockLock(fIOLock);
        for (UInt32 a = 0; a < actionCount; a++) {
            actions[a].nub->release();
            MPSTargetSlot *slot = &fTargets[actions[a].index];
            if (slot->nub != actions[a].nub) continue;
            char name[48];
            snprintf(name, sizeof(name), "MPSTarget%uMediaStatus",
                     (unsigned)actions[a].index);
            MPS_DEBUG_PROP(name, (unsigned long long)(UInt32)actions[a].status, 32);
            if (!actions[a].online) {
                /* The storage layer has no client before matching; that is not
                 * a failed offline transition. */
                if (actions[a].status == kIOReturnSuccess ||
                    actions[a].status == kIOReturnNotAttached) {
                    slot->offlinePending = false;
                } else {
                    retry = true;
                }
            } else if (actions[a].status == kIOReturnSuccess) {
                slot->onlinePending = false;
                slot->online = true;
            } else if (actions[a].status == kIOReturnNotAttached) {
                /* Matching may still be in progress for a fresh nub. */
                retry = true;
            } else {
                slot->online = false;
                retry = true;
            }
        }
        if (fStopping) { retry = false; state = "stopped"; goto done; }
    }

    /* 5. Bounded fallback. Only a removal or probe that could not be completed
     * escalates to an IOC reset, and the reset budget is per insertion. */
    if (resetNeeded || probeFailed) {
        if (fProtocolRecoveryPending) {
            state = "protocol-recovery";
            retry = true;
            goto done;
        }
        if (fHotplugAttempts < 3) {
            fHotplugAttempts++;
            MPS_DEBUG_PROP("MPSResetInitiator",
                        resetNeeded ? "target-removal" : "target-probe");
            bool ok = resetAndReinitialize();
            MPS_DEBUG_PROP("MPSHotplugResetResult", ok ? "ok" : "failed");
            if (!ok) {
                invalidateTargets();
                state = "recovery-failed";
                retry = fHotplugAttempts < 3;
            } else {
                /* The next pass re-scans and re-probes every target. */
                state = "targets-revalidating";
                retry = true;
            }
            goto done;
        }
        state = "recovery-exhausted";
        retry = true;
        goto done;
    }

    live = liveTargetCount();
    fMediaPresent = live != 0;
    fDeviceOffline = false;
    {
        /* A present target that still has no medium is reported as such: the
         * controller is usable, that target is not. */
        UInt32 unpublished = 0;
        for (UInt32 i = 0; i < kMPSMaxTargets; i++)
            if (fTargets[i].handle != 0 && fTargets[i].present &&
                fTargets[i].nub == NULL) unpublished++;
        state = live == 0 ? "absent" : unpublished != 0 ? "online-partial" : "online";
    }

done:
    /* Publication state is derived from the table, so it stays truthful even
     * when this pass ended on an error path. */
    live = liveTargetCount();
    fMediaPresent = live != 0;
    {
        /* A retired medium keeps its nub for the next insertion; its age drives
         * the bounded rediscovery reset in the watchdog. */
        UInt32 removedMedia = 0;
        for (UInt32 i = 0; i < kMPSMaxTargets; i++)
            if (fTargets[i].handle == 0 && fTargets[i].nub != NULL) removedMedia++;
        IOSimpleLockLock(fReplyLock);
        fRemovedMediaCount = removedMedia;
        IOSimpleLockUnlock(fReplyLock);
    }
    MPS_DEBUG_PROP("MPSHotplugState", state);
    /* A wake that the worker had to finish must not leave the power state
     * claiming recovery is still pending. */
    if (!fSuspended && live != 0) MPS_DEBUG_PROP("MPSPowerState", "on");
    MPS_DEBUG_PROP("MPSMediaPresent", fMediaPresent);
    MPS_DEBUG_PROP("MPSDeviceOffline", fDeviceOffline);
    MPS_DEBUG_PROP("MPSMediaGeneration", (unsigned long long)fMediaGeneration, 32);
    MPS_DEBUG_PROP("MPSHotplugAttempts", (unsigned long long)fHotplugAttempts, 32);
    MPS_DEBUG_PROP("MPSHotplugHandle", (unsigned long long)fAttachedDevHandle, 16);
    MPS_DEBUG_PROP("MPSHotplugBlockDev", live != 0);
    /* A periodic pass that changed nothing must not rewrite the whole target
     * property set; the watchdog republishes the counters anyway. */
    if (dirty) publishTargetProperties();
    IOSimpleLockLock(fReplyLock);
    if (generation == fTopologyGeneration && !fHotplugPending && !retry) {
        fTopologyChanged = false;
    }
    if (retry) {
        fHotplugPending = true;
        fHotplugCooldown = cooldown;
    }
    fHotplugRunning = false;
    IOSimpleLockUnlock(fReplyLock);
    IOLockUnlock(fIOLock);
    drainRetiredNubs();
}

bool
MPSController::resetAndReinitialize(void)
{
    /* Called with fIOLock held. Restore transport and re-enumerate the fabric;
     * probing and publication stay with the unlocked worker, which reports
     * media state per target and must never do so under this lock. */
    bool ok = false;
    const char *state = "failed";
    if (fReinitInProgress || fStopping) return false;
    fReinitInProgress = true;
    fDeviceOffline = true;
    fUnmapEnabled = false;
    fResetCount++;
    MPS_DEBUG_PROP("MPSMediaReady", false);

    teardownInterrupts();
    if (!diagReset()) {
        state = "diag-reset-failed";
        /* Old requests can still own DMA; do not free their buffers. */
        recordProtocolFault(kMPSProtocolManagementTimeout, fTimedOutSmid);
        goto finished;
    }
    resetProtocolState();
    /* An IOC reset unbinds every medium: handles, geometry and identity have
     * to be re-established per target before any of them accepts I/O again. */
    invalidateTargets();
    /* A timed-out startup manufacturing probe used a smaller temporary
     * page. It is safe to replace only after this successful IOC reset. */
    if (fPageBuffer != NULL && fPageBuffer->getLength() < MPS_PAGE_BUF_BYTES) {
        mpsReleaseResidentDma(fPageDma);
        fPageBuffer->release();
        fPageBuffer = NULL;
        fPagePhys = 0;
    }
    freeQueueMemory();
    fReplyPostIndex = fReplyCount = 0;
    fLastReplySmid = 0xFFFF;
    fLastReplyKind = 0;
    fFrozenSmid = fTimedOutSmid = 0;
    fAttachedDevHandle = 0;
    fAttachedLun = 0;
    fCapacityBlocks = 0;
    fBlockSize = 0;

    /* IOC Init advertises the installed interrupt vector and initializeQueues
     * unmasks reply interrupts only when the source already exists. */
    if (!setupInterrupts()) {
        state = "interrupt-setup-failed";
        recordProtocolFault(kMPSProtocolInterruptUnavailable, 0);
        goto finished;
    }
    if (!initializeController()) { state = "IOC-facts-failed"; goto finished; }
    if (!initializeQueues()) { state = "queue-init-failed"; goto finished; }
    if (!allocatePageBuffer()) { state = "page-buffer-failed"; goto finished; }
    if (!sendEventNotification()) { state = "event-subscription-failed"; goto finished; }
    if (!sendPortEnable()) { state = "port-enable-failed"; goto finished; }
    /* Discovery can lag the reset reply: poll a bounded number of times and
     * let the worker publish whatever the successful snapshot contains. */
    for (UInt32 retry = 0; retry < 5 && !fStopping; retry++) {
        IOSleep(200);
        if (!readSasIOUnitPage0()) {
            if (fProtocolRecoveryPending) goto finished;
            continue;
        }
        ok = !fProtocolRecoveryPending && !fStopping;
        break;
    }
    if (ok) state = fTargetCount != 0 ? "ok" : "no-targets";
    else state = "enumeration-failed";

finished:
    fReinitInProgress = false;
    fDeviceOffline = !ok;
    MPS_DEBUG_PROP("MPSReinitState", state);
    MPS_DEBUG_PROP("MPSResetCount", (unsigned long long)fResetCount, 32);
    MPS_DEBUG_PROP("MPSResidentBuffersReady",
                fPageBuffer != NULL && fPagePhys != 0 &&
                fDataBuffer != NULL && fDataPhys != 0 && fDataBytes != 0);
    return ok;
}
