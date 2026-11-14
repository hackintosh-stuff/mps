#include "MPSController.h"
#include "MPI/mpi2_min.h"
#include "MPI/mpi2_target.h"
#include "MPI/mpi2_mgmt.h"
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/task.h>
#include <string.h>

/* mps_user.c provides the protocol reference; this is a native IOUserClient
 * ABI, not a binary-compatible FreeBSD ioctl shim. SMID 1 is serialized with
 * all config/probe work by fIOLock and with block I/O by the existing fence. */
static const UInt32 kMPSControlSmid = 1;
static UInt32 mpsControlTimeout(const MPSControlInput &in)
{ return in.timeoutMs ? in.timeoutMs : 30000; }

bool MPSController::allocateControlBuffer(UInt32 bytes)
{
    if (fControlDmaBusy || !bytes || bytes > 2U * kMPSControlMaxData + 4096U)
        return false;
    if (fControlBuffer && fControlBufferBytes >= bytes) return true;
    if (fControlBuffer) {
        mpsReleaseResidentDma(fControlDma);
        fControlBuffer->release();
        fControlBuffer = NULL;
        fControlBufferPrepared = false;
        fControlBufferBytes = 0;
        fControlPhys = 0;
    }
    IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
        bytes, UINT64_C(0x00000000fffff000));
    if (!memory) return false;
    if (!memory->getBytesNoCopy() ||
        !mpsMapResidentDma(fControlDma, memory, fDmaMapper, bytes, 4096)) {
        memory->release();
        return false;
    }
    fControlBuffer = memory;
    fControlPhys = fControlDma.address;
    fControlBufferBytes = bytes;
    fControlBufferPrepared = true;
    return true;
}

void MPSController::freeControlBuffers(bool dmaStopped)
{
    if (!dmaStopped) return;
    if (fControlBuffer) {
        mpsReleaseResidentDma(fControlDma);
        fControlBuffer->release();
    }
    fControlBuffer = NULL;
    fControlBufferPrepared = fControlDmaBusy = false;
    fControlBufferBytes = 0;
    fControlPhys = 0;
    for (unsigned type = 0; type < 3; ++type) {
        DiagnosticBuffer &b = fDiagnostic[type];
        if (b.memory) {
            mpsReleaseResidentDma(b.dma);
            b.memory->release();
        }
        bzero(&b, sizeof(b));
    }
}

void MPSController::recordControlEventLocked(UInt16 event, UInt32 context,
                                            const UInt8 *payload, UInt32 bytes)
{
    if (event >= 128 ||
        fControlEventSequence == UINT64_MAX) return;
    UInt64 sequence = ++fControlEventSequence;
    MPSControlEvent &entry = fControlEvents[(sequence - 1) % kMPSControlEventCount];
    bzero(&entry, sizeof(entry));
    entry.sequence = sequence;
    entry.milliseconds = uptimeMilliseconds();
    entry.epoch = fTransportEpoch;
    entry.event = event;
    entry.context = context;
    entry.payloadBytes = bytes < kMPSControlEventPayload ? bytes : kMPSControlEventPayload;
    if (entry.payloadBytes && payload) memcpy(entry.payload, payload, entry.payloadBytes);
}

void MPSController::releaseDiagnosticEventLocked(const UInt8 *reply, UInt32 bytes)
{
    if (bytes < 20 || reply[1] >= 3 ||
        (mpsControlLe16(reply + 14) & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_DIAGNOSTIC_RELEASED)
        return;
    DiagnosticBuffer &b = fDiagnostic[reply[1]];
    if (b.memory && b.owned && b.epoch == fTransportEpoch) {
        b.owned = false;
        b.valid = true;
    }
}

bool MPSController::submitControlFrame(UInt32 flags, UInt32 high, UInt32 timeout,
                                        UInt8 *reply, UInt8 *kind)
{
    const UInt8 *frame = (const UInt8 *)fRequestBuffer->getBytesNoCopy() +
                         kMPSControlSmid * fRequestFrameSize;
    const UInt8 function = frame[3];
    const bool targetIo = function == MPI2_FUNCTION_SCSI_IO_REQUEST ||
        function == MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH ||
        function == MPI2_FUNCTION_SATA_PASSTHROUGH;
    IOSimpleLockLock(fReplyLock);
    bool posted = !(targetIo && fRemovalPending) &&
                  postOwnedRequestLocked(kMPSControlSmid, flags, high);
    IOSimpleLockUnlock(fReplyLock);
    if (!posted) { cancelUnpostedCommand(kMPSControlSmid); return false; }
    fControlDmaBusy = true;
    ++fCommandCount;
    if (!waitForSmid(timeout, kMPSControlSmid) ||
        !takeReplySlot(kMPSControlSmid, reply, kMPSControlMaxReply, kind)) {
        IOSimpleLockLock(fReplyLock);
        if (fCommandOwners[kMPSControlSmid].state == kMPSCommandPosted)
            fCommandOwners[kMPSControlSmid].state = kMPSCommandQuarantined;
        recordProtocolFaultLocked(kMPSTargetMalformedReply, kMPSControlSmid);
        IOSimpleLockUnlock(fReplyLock);
        MPS_DEBUG_LOG("MPS control: timeout; frame/payload retained, no command replay\n");
        return false;
    }
    IOSimpleLockLock(fReplyLock);
    fManagementBusy[kMPSControlSmid] = false;
    IOSimpleLockUnlock(fReplyLock);
    fControlDmaBusy = false;
    return true;
}

IOReturn MPSController::controlRequest(const UInt8 *bytes, UInt32 inputSize,
    UInt8 *response, UInt32 outputSize, UInt32 *produced)
{
    if (!bytes || !response || !produced || !fIOLock ||
        inputSize < sizeof(MPSControlInput)) return kIOReturnBadArgument;
    if (!fManagementEnabled) return kIOReturnNotPermitted;
    MPSControlInput in;
    memcpy(&in, bytes, sizeof(in));
    if (!mpsControlInputValid(in, inputSize, outputSize)) return kIOReturnBadArgument;
    MPSControlOutput *out = (MPSControlOutput *)response;
    bzero(out, outputSize);
    out->version = kMPSControlVersion;
    out->operation = in.operation;
    const bool fence = in.operation == kMPSControlPassthrough ||
        in.operation == kMPSControlReset || in.operation == kMPSControlRescan ||
        in.operation == kMPSControlWriteRegister || in.operation == kMPSControlEventMask ||
        in.operation == kMPSControlDiagRegister ||
        in.operation == kMPSControlDiagRelease || in.operation == kMPSControlDiagUnregister;
    IOReturn status = fence ? beginAsyncBarrier() : kIOReturnSuccess;
    if (status == kIOReturnSuccess) {
        IOLockLock(fIOLock);
        out->epoch = fTransportEpoch;
        if (fStopping || fStarting || isInactive()) status = kIOReturnNotAttached;
        else if (in.operation >= kMPSControlEventMask && in.epoch != fTransportEpoch)
            status = kIOReturnNotReady;
        else status = controlDispatch(in, bytes + sizeof(in),
            bytes + sizeof(in) + in.requestBytes, out);
        out->epoch = fTransportEpoch;
        IOLockUnlock(fIOLock);
        if (fence) endAsyncBarrier();
    }
    out->status = (UInt32)status;
    if (out->dataBytes > in.dataInBytes) { out->dataBytes = 0; out->status = kIOReturnOverrun; }
    *produced = sizeof(*out) + out->dataBytes;
    /* Transport/firmware errors are explicit in the returned packet. Returning
     * success here ensures IOKit copies the checked reply and sense to the caller. */
    return kIOReturnSuccess;
}

IOReturn MPSController::controlDispatch(const MPSControlInput &in, const UInt8 *request,
    const UInt8 *dataOut, MPSControlOutput *out)
{
    UInt8 *data = (UInt8 *)(out + 1);
    if (in.operation != kMPSControlPassthrough && in.dataOutBytes) return kIOReturnBadArgument;
    if (fFirmwareDownloadActive && in.operation != kMPSControlPassthrough &&
        in.operation != kMPSControlInfo && in.operation != kMPSControlTargets &&
        in.operation != kMPSControlEvents && in.operation != kMPSControlReset)
        return kIOReturnBusy;
    switch (in.operation) {
    case kMPSControlInfo: {
        if (in.requestBytes || in.dataInBytes < sizeof(MPSControlInfo)) return kIOReturnNoSpace;
        MPSControlInfo *info = (MPSControlInfo *)data;
        info->capabilities = fIocCapabilities;
        info->requestFrameBytes = fRequestFrameSize;
        info->replyFrameBytes = fReplyFrameSize;
        info->requestCredits = fRequestCredit;
        info->targetLimit = kMPSMaxTargets;
        info->featureBits = (fManagementEnabled ? 4U : 0U) | (fMaintenanceMode ? 8U : 0U) |
            (fFirmwareDownloadActive ? 16U : 0U) | (fFirmwareDownloadPoisoned ? 32U : 0U);
        info->mpiVersion = fMpiVersion;
        info->milliseconds = uptimeMilliseconds();
        memcpy(info->iocFacts, fIocFactsSnapshot, sizeof(info->iocFacts));
        info->firmware = mpsControlLe32(fIocFactsSnapshot + 0x20);
        if (fPCIDevice && !fSuspended) {
            for (UInt32 offset = 0; offset < 256; offset += 4)
                mpsControlPut32(info->pciConfig + offset, fPCIDevice->configRead32(offset));
            info->vendorDevice = mpsControlLe32(info->pciConfig);
            info->pciRevision = info->pciConfig[8];
        }
        out->dataBytes = sizeof(*info);
        return kIOReturnSuccess;
    }
    case kMPSControlTargets: {
        if (in.requestBytes || in.argument0 > kMPSMaxTargets) return kIOReturnBadArgument;
        UInt32 capacity = in.dataInBytes / sizeof(MPSControlTarget), count = 0;
        UInt32 index = (UInt32)in.argument0;
        for (; index < kMPSMaxTargets && count < capacity; ++index) {
            const MPSTargetSlot &slot = fTargets[index];
            if (!slot.handle) continue;
            MPSControlTarget &target = ((MPSControlTarget *)data)[count++];
            target.handle = slot.handle; target.kind = (UInt8)slot.kind;
            target.flags = (slot.present ? 1 : 0) | (slot.probeValid ? 2 : 0) | (slot.online ? 4 : 0);
            target.generation = slot.generation; target.wwid = slot.sasAddress;
            target.lun = slot.lun; target.blocks = slot.capacityBlocks;
            target.pathCount = slot.pathCount;
            target.targetCapabilities = (slot.managementOnly ? 1U : 0U) |
                (slot.tlrEnabled ? 2U : 0U) | ((UInt32)slot.protectionType << 8);
            target.blockSize = slot.blockSize; target.maxTransfer = slot.maxTransferBytes;
            strlcpy(target.vendor, slot.vendor, sizeof(target.vendor));
            strlcpy(target.product, slot.product, sizeof(target.product));
            strlcpy(target.revision, slot.revision, sizeof(target.revision));
            strlcpy(target.serial, slot.serial, sizeof(target.serial));
            strlcpy(target.identity, slot.identity, sizeof(target.identity));
        }
        out->dataBytes = count * sizeof(MPSControlTarget); out->nextSequence = index;
        return capacity ? kIOReturnSuccess : kIOReturnNoSpace;
    }
    case kMPSControlEvents: {
        if (in.requestBytes) return kIOReturnBadArgument;
        UInt32 capacity = in.dataInBytes / sizeof(MPSControlEvent), count = 0;
        if (!capacity) return kIOReturnNoSpace;
        IOSimpleLockLock(fReplyLock);
        UInt64 last = fControlEventSequence;
        UInt64 first = last >= kMPSControlEventCount ? last - kMPSControlEventCount + 1 : 1;
        UInt64 next = in.argument0 ? in.argument0 : first;
        if (next < first) { next = first; out->flags |= 1; }
        while (next <= last && count < capacity) {
            ((MPSControlEvent *)data)[count++] = fControlEvents[(next - 1) % kMPSControlEventCount];
            if (next == UINT64_MAX) break;
            ++next;
        }
        IOSimpleLockUnlock(fReplyLock);
        out->dataBytes = count * sizeof(MPSControlEvent); out->nextSequence = next;
        return kIOReturnSuccess;
    }
    case kMPSControlDiagQuery: case kMPSControlDiagRead:
    case kMPSControlDiagRegister: case kMPSControlDiagRelease: case kMPSControlDiagUnregister:
        return controlDiagnostic(in, request, out);
    default: break;
    }
    if (fSuspended || fReinitInProgress || !fFactsValid || !fRegs || !fReplyLock)
        return kIOReturnNotReady;
    if (in.operation == kMPSControlReset) {
        if (!fMaintenanceMode || !(in.flags & kMPSControlAllowMutation) || in.requestBytes) return kIOReturnNotPermitted;
        bool ready = resetAndReinitialize();
        fHotplugPending = true;
        return ready ? kIOReturnSuccess : kIOReturnIOError;
    }
    /* The control channel must work on an empty HBA or a tape/SES-only
     * topology. Block-media publication is not controller transport health. */
    if (fProtocolRecoveryPending ||
        (readReg(MPI2_DOORBELL_OFFSET) & MPI2_IOC_STATE_MASK) != MPI2_IOC_STATE_OPERATIONAL)
        return kIOReturnNotReady;
    if (in.operation == kMPSControlReadRegister || in.operation == kMPSControlWriteRegister) {
        if (in.requestBytes || in.dataInBytes < 4 || !fMMIO || in.argument0 > fMMIO->getLength() ||
            fMMIO->getLength() - in.argument0 < 4 || (in.argument0 & 3)) return kIOReturnBadArgument;
        if (in.operation == kMPSControlWriteRegister) {
            /* Arbitrary descriptor/queue-address writes would bypass DMA ownership.
             * Scratchpads are writable; operational registers remain driver-owned. */
            if (!fMaintenanceMode || !(in.flags & kMPSControlAllowMutation) ||
                in.argument0 < 0xB0 || in.argument0 > 0xBC || in.argument1 > UINT32_MAX)
                return kIOReturnNotPermitted;
            writeReg((UInt32)in.argument0, (UInt32)in.argument1);
        }
        if (in.dataInBytes < 4) return kIOReturnNoSpace;
        mpsControlPut32(data, readReg((UInt32)in.argument0)); out->dataBytes = 4;
        return kIOReturnSuccess;
    }
    if (in.operation == kMPSControlEventMask) {
        if (in.requestBytes != 16) return kIOReturnBadArgument;
        IOSimpleLockLock(fReplyLock);
        memcpy(fControlEventMask, request, 16);
        IOSimpleLockUnlock(fReplyLock);
        return sendEventNotification() ? kIOReturnSuccess : kIOReturnIOError;
    }
    if (in.operation == kMPSControlRescan) {
        if (in.requestBytes) return kIOReturnBadArgument;
        bool ok = discoverTopology();
        fHotplugPending = true;
        return ok ? kIOReturnSuccess : kIOReturnIOError;
    }
    if (in.operation == kMPSControlPassthrough)
        return controlPassthrough(in, request, dataOut, out);
    return kIOReturnUnsupported;
}

IOReturn MPSController::controlPassthrough(const MPSControlInput &in, const UInt8 *user,
    const UInt8 *dataOut, MPSControlOutput *out)
{
    if (in.requestBytes < 12 || user[2] || user[8] || user[9] ||
        in.requestBytes > fRequestFrameSize || !fRequestBuffer || !fSenseBuffer ||
        fControlDmaBusy) return kIOReturnBadArgument;
    const UInt8 function = user[3];
    const bool write = in.dataOutBytes != 0, read = in.dataInBytes != 0;
    const UInt32 length = write ? in.dataOutBytes : in.dataInBytes;
    bool mutation = false, legacySgl = false, scsi = false;
    UInt32 prefix = 0, descriptor = MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE, high = 0;
    switch (function) {
    case MPI2_FUNCTION_IOC_FACTS: case MPI2_FUNCTION_PORT_FACTS:
        prefix = 12; if (write || read) return kIOReturnBadArgument; break;
    case MPI2_FUNCTION_CONFIG: {
        prefix = 28;
        if (in.requestBytes != prefix || (write && read)) return kIOReturnBadArgument;
        if ((user[23] & 15) == 15 && (user[6] < 0x10 || user[6] > 0x1A)) return kIOReturnUnsupported;
        UInt8 action = user[0];
        if (action > 7) return kIOReturnBadArgument;
        mutation = action == 2 || action == 3 || action == 4;
        UInt32 words = (user[23] & 15) == 15 ? mpsControlLe16(user + 4) : user[21];
        if (action == 0 || action == 3) { if (length) return kIOReturnBadArgument; }
        else if (words * 4 != length || (mutation ? !write : !read)) return kIOReturnBadArgument;
        break;
    }
    case MPI2_FUNCTION_FW_DOWNLOAD: {
        prefix = kMPSFirmwareSgeOffset; legacySgl = true; mutation = true;
        if (in.requestBytes != prefix || !write || read || !in.argument1 ||
            (user[7] & ~1U)) return kIOReturnBadArgument;
        const UInt32 total = mpsControlLe32(user + 12), offset = mpsControlLe32(user + kMPSFirmwareContextOffset + 8);
        if (!total || offset >= total || length > total - offset ||
            bool(user[7] & 1) != (offset + length == total)) return kIOReturnBadArgument;
        if (fFirmwareDownloadPoisoned) return kIOReturnNotReady;
        if (fFirmwareDownloadActive) {
            if (fFirmwareToken != in.argument1 || fFirmwareImageType != user[0] ||
                fFirmwareTotal != total || fFirmwareOffset != offset) return kIOReturnBusy;
        } else if (offset) return kIOReturnBadArgument;
        break;
    }
    case MPI2_FUNCTION_FW_UPLOAD:
        prefix = kMPSFirmwareSgeOffset; legacySgl = true;
        if (in.requestBytes != prefix || write ||
            length > UINT32_MAX - mpsControlLe32(user + kMPSFirmwareContextOffset + 8)) return kIOReturnBadArgument;
        break;
    case MPI2_FUNCTION_RAID_ACTION:
        prefix = 20;
        mutation = true; if (write && read) return kIOReturnBadArgument; break;
    case MPI2_FUNCTION_SCSI_IO_REQUEST: case MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH:
        prefix = 96; legacySgl = true; scsi = true;
        if (in.requestBytes != prefix || (write && read) || length > kMPSControlIoBytes)
            return kIOReturnBadArgument;
        if (mpsControlLe16(user + 36) & ~UInt16(0x3F)) return kIOReturnBadArgument;
        if (!(mpsControlLe16(user + 36) & 0x3F) || (mpsControlLe16(user + 36) & 0x3F) > 32)
            return kIOReturnBadArgument;
        mutation = function == MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH || write ||
                   !mpsControlReadOnlyCdb(user[64]);
        if (function == MPI2_FUNCTION_SCSI_IO_REQUEST) {
            descriptor = MPI2_REQ_DESCRIPT_FLAGS_SCSI_IO;
            high = (UInt32)mpsControlLe16(user) << 16;
        }
        break;
    case MPI2_FUNCTION_SMP_PASSTHROUGH:
        prefix = 32; legacySgl = true;
        if (!write || !read || in.dataOutBytes > 65532 || in.dataInBytes > 65532)
            return kIOReturnBadArgument;
        mutation = dataOut[0] != 0x40 || in.dataOutBytes < 4 || dataOut[1] >= 0x80;
        break;
    case MPI2_FUNCTION_SATA_PASSTHROUGH:
        prefix = 48; legacySgl = true;
        if (in.requestBytes != prefix || (write && read) || length > kMPSControlIoBytes)
            return kIOReturnBadArgument;
        mutation = write || (user[30] != 0xEC && user[30] != 0xA1);
        break;
    case MPI2_FUNCTION_SAS_IO_UNIT_CONTROL:
        prefix = 44; mutation = true; if (length) return kIOReturnBadArgument; break;
    case MPI2_FUNCTION_SCSI_ENCLOSURE_PROCESSOR:
        prefix = 32;
        if (in.requestBytes != prefix || length || user[4] > 1) return kIOReturnBadArgument;
        mutation = user[4] == 0; break;
    case MPI2_FUNCTION_TOOLBOX:
        prefix = 112; legacySgl = true; mutation = true;
        if (user[0] != MPI2_TOOLBOX_DIAGNOSTIC_CLI_TOOL || (write && read)) return kIOReturnUnsupported;
        break;
    case MPI2_FUNCTION_SCSI_TASK_MGMT: {
        if (in.requestBytes != sizeof(MPSScsiTaskMgmtRequest) || length ||
            (!(in.flags & kMPSControlAllowMutation) || !fMaintenanceMode)) return kIOReturnNotPermitted;
        const MPSScsiTaskMgmtRequest *task = (const MPSScsiTaskMgmtRequest *)user;
        if (task->TaskType != MPI2_SCSITASKMGMT_TASKTYPE_LUN_RESET &&
            task->TaskType != MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET) return kIOReturnUnsupported;
        UInt32 lun = 0;
        if (!mpsDecodeLun(task->LUN, &lun)) return kIOReturnBadArgument;
        MPSTargetSlot *slot = findTarget(mpsTargetKey(task->DevHandle, lun));
        if (!slot || slot->sasAddress != in.argument0 || slot->generation != in.generation)
            return kIOReturnNotReady;
        UInt16 status = 0xFFFF;
        bool ok = sendTaskMgmt(task->TaskType, 0, 28, task->DevHandle, &status, lun);
        mpsControlPut16(out->reply + 14, status); out->replyBytes = 20;
        fHotplugPending = true;
        return ok ? kIOReturnSuccess : kIOReturnIOError;
    }
    default: return kIOReturnUnsupported;
    }
    if (fFirmwareDownloadActive && function != MPI2_FUNCTION_FW_DOWNLOAD)
        return kIOReturnBusy;
    if (length > 0x00FFFFFFU) return kIOReturnBadArgument;
    if (in.requestBytes != prefix || prefix > fRequestFrameSize ||
        ((legacySgl || function == MPI2_FUNCTION_CONFIG || function == MPI2_FUNCTION_RAID_ACTION) &&
         prefix + (function == MPI2_FUNCTION_SMP_PASSTHROUGH ? 24U : 12U) > fRequestFrameSize))
        return kIOReturnBadArgument;
    if (mutation && (!(in.flags & kMPSControlAllowMutation) || !fMaintenanceMode))
        return kIOReturnNotPermitted;
    UInt16 handle = (scsi || function == MPI2_FUNCTION_SATA_PASSTHROUGH)
        ? mpsControlLe16(user) : 0;
    UInt32 lun = 0;
    if (scsi && !mpsDecodeLun(user + 52, &lun)) return kIOReturnBadArgument;
    MPSTargetSlot *slot = handle ? findTarget(mpsTargetKey(handle, lun)) : NULL;
    MPSTargetSlot member = {};
    if (!slot && handle && function == MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH &&
        fMaintenanceMode && !in.generation && in.argument0) {
        UInt8 page[512]; UInt32 actual = 0;
        bool isMember = false;
        if (!allocatePageBuffer() || !readRaidMembership(handle, &isMember) || !isMember ||
            !readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
                MPI2_SAS_DEVICE_PGAD_FORM_HANDLE | handle, page, sizeof(page), &actual) ||
            actual < 0x24 || mpsControlLe16(page + 0x18) != handle ||
            ((UInt64)mpsControlLe32(page + 0x0C) | ((UInt64)mpsControlLe32(page + 0x10) << 32)) != in.argument0)
            return kIOReturnNotReady;
        member.handle = handle; member.lun = lun; member.sasAddress = in.argument0;
        member.kind = kMPSTargetSas; member.present = true; member.hiddenByRaid = true;
        slot = &member;
    }
    MPSTargetSlot physicalTarget = {}; // private, never published or inserted in fTargets
    if ((scsi || function == MPI2_FUNCTION_SATA_PASSTHROUGH) &&
        (!handle || handle == 0xFFFF)) return kIOReturnBadArgument;
    if (handle) {
        if (slot) {
            if (!slot->present || slot->sasAddress != in.argument0 ||
                slot->generation != in.generation) return kIOReturnNotReady;
        } else {
            /* Hidden RAID members do not have published target slots. Only an
             * explicitly authorized maintenance request with generation zero
             * may address them; re-read the exact firmware identity first. */
            if (!fMaintenanceMode || !(in.flags & kMPSControlAllowMutation) ||
                in.generation != 0 || !in.argument0 || !allocatePageBuffer())
                return kIOReturnNotReady;
            UInt8 page[512]; UInt32 actual = 0; UInt16 pageStatus = 0xFFFF;
            bool found = readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
                MPI2_SAS_DEVICE_PGAD_FORM_HANDLE | handle,
                page, sizeof(page), &actual, &pageStatus);
            if (!found || actual < 0x24 || mpsControlLe16(page + 0x18) != handle ||
                page[0x17] != 0) return kIOReturnNotReady;
            UInt64 identity = (UInt64)mpsControlLe32(page + 0x0C) |
                              ((UInt64)mpsControlLe32(page + 0x10) << 32);
            UInt32 info = mpsControlLe32(page + 0x1C), flags = mpsControlLe16(page + 0x20);
            if (identity != in.argument0 || !(flags & 1U) || (flags & 0x8100U) ||
                (info & 7U) != MPI2_SAS_DEVICE_INFO_END_DEVICE) return kIOReturnNotReady;
            physicalTarget.kind = kMPSTargetSas;
            physicalTarget.handle = handle; physicalTarget.lun = lun;
            physicalTarget.sasAddress = identity; physicalTarget.deviceInfo = info;
            physicalTarget.flags = (UInt16)flags;
            slot = &physicalTarget;
            mutation = true; // Physical-member access is never implicitly enabled.
        }
    }
    const UInt32 inputRegion = (in.dataOutBytes + 4095U) & ~4095U;
    const UInt32 dataRegion = (in.dataInBytes + 4095U) & ~4095U;
    const UInt32 required = inputRegion + dataRegion + 4096U;
    if (!allocateControlBuffer(required)) return kIOReturnNoResources;
    UInt8 *dma = (UInt8 *)fControlBuffer->getBytesNoCopy();
    bzero(dma, required);
    if (write) memcpy(dma, dataOut, in.dataOutBytes);
    const UInt64 address = fControlPhys + (read && !write ? inputRegion : 0);
    if (!beginManagementRequest(kMPSControlSmid)) return kIOReturnBusy;
    if (scsi) {
        if (!prepareScsiSense(kMPSControlSmid)) {
            cancelUnpostedCommand(kMPSControlSmid); return kIOReturnBusy;
        }
    }
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy() + kMPSControlSmid * fRequestFrameSize;
    bzero(frame, fRequestFrameSize); memcpy(frame, user, prefix);
    frame[2] = frame[8] = frame[9] = 0;
    /* User message flags cannot select a different sense/address space. */
    frame[7] = function == MPI2_FUNCTION_FW_DOWNLOAD ? (user[7] & 1U) : 0;
    if (scsi) {
        MPSScsiIoRequest *io = (MPSScsiIoRequest *)frame;
        io->SenseBufferLowAddress = (UInt32)senseAddress(kMPSControlSmid);
        io->SenseBufferLength = kMPSControlSenseBytes; io->SGLOffset0 = 24;
        io->SGLFlags = 0; io->SGLOffset1 = io->SGLOffset2 = io->SGLOffset3 = 0;
        io->DataLength = length; io->BidirectionalDataLength = 0;
        io->Control = write ? MPI2_SCSIIO_CONTROL_WRITE : read ? MPI2_SCSIIO_CONTROL_READ : MPI2_SCSIIO_CONTROL_NODATATRANSFER;
        /* MPI2_SCSIIO_CONTROL_ADDCDBLEN_SHIFT=26: 32-byte CDBs carry four additional dwords. */
        if ((io->IoFlags & 0x3F) > 16)
            io->Control |= (((io->IoFlags & 0x3F) - 16U + 3U) / 4U) << 26;
        io->IoFlags &= 0x3F; io->EEDPFlags = 0;
        if (!applyScsiTargetPolicy(frame, slot)) {
            cancelUnpostedCommand(kMPSControlSmid); return kIOReturnUnsupported;
        }
    }
    if (function == MPI2_FUNCTION_FW_DOWNLOAD || function == MPI2_FUNCTION_FW_UPLOAD) {
        bzero(frame + 16, 4);
        if (function == MPI2_FUNCTION_FW_UPLOAD) bzero(frame + 12, 4);
        mpsManagementFirmwareContext(frame + kMPSFirmwareContextOffset,
            mpsControlLe32(user + kMPSFirmwareContextOffset + 8), length);
    }
    if (function == MPI2_FUNCTION_SATA_PASSTHROUGH) {
        mpsControlPut32(frame + 24, length);
        UInt16 pt = mpsControlLe16(frame + 4) & ~UInt16(3);
        mpsControlPut16(frame + 4, pt | (write ? 2 : read ? 1 : 0)); frame[6] = 0;
    }
    if (function == MPI2_FUNCTION_TOOLBOX) { frame[12] = 0; mpsControlPut32(frame + 16, length); }
    if (function == MPI2_FUNCTION_CONFIG) frame[1] = 0; // MPI/system SGL.
    if (function == MPI2_FUNCTION_SMP_PASSTHROUGH) {
        frame[0] &= ~0x80; frame[6] = 0;
        mpsControlPut16(frame + 4, (UInt16)in.dataOutBytes);
        // Separate request/response buffers: only the response ends the list.
        mpsManagementSimple64(frame + 32, fControlPhys, in.dataOutBytes, true, false);
        mpsManagementSimple64(frame + 44, fControlPhys + inputRegion, in.dataInBytes, false, true);
    } else if (legacySgl || function == MPI2_FUNCTION_CONFIG || function == MPI2_FUNCTION_RAID_ACTION) {
        mpsManagementSimple64(frame + prefix, address, length, write, true);
    }
    if (function == MPI2_FUNCTION_FW_DOWNLOAD) {
        fFirmwareDownloadActive = true; fFirmwareToken = in.argument1;
        fFirmwareImageType = user[0]; fFirmwareTotal = mpsControlLe32(user + 12);
        fFirmwareOffset = mpsControlLe32(user + kMPSFirmwareContextOffset + 8);
    }
    UInt8 kind = 0;
    if (!submitControlFrame(descriptor, high, mpsControlTimeout(in), out->reply, &kind)) {
        if (function == MPI2_FUNCTION_FW_DOWNLOAD) fFirmwareDownloadPoisoned = true;
        return kIOReturnTimeout;
    }
    if (kind == 3) return kIOReturnAborted;
    if (kind == 1) {
        UInt32 replyBytes = out->reply[2] * 4U;
        if (replyBytes < 20 || replyBytes > kMPSControlMaxReply) return kIOReturnOverrun;
        out->replyBytes = replyBytes;
    } else if (kind != 2) return kIOReturnIOError;
    UInt32 actual = in.dataInBytes;
    if (scsi && kind == 1) {
        if (out->replyBytes < MPS_SCSIIO_REPLY_MIN_BYTES) return kIOReturnUnderrun;
        const MPSScsiIoReply *reply = (const MPSScsiIoReply *)out->reply;
        actual = read ? reply->TransferCount : 0;
        if (reply->SenseCount > kMPSControlSenseBytes) return kIOReturnOverrun;
        if (reply->SCSIState & MPI2_SCSI_STATE_AUTOSENSE_VALID) out->senseBytes = reply->SenseCount;
    } else if (function == MPI2_FUNCTION_SMP_PASSTHROUGH && kind == 1)
        actual = mpsControlLe16(out->reply + 4);
    else if (function == MPI2_FUNCTION_SATA_PASSTHROUGH && kind == 1) {
        if (out->replyBytes < 48) return kIOReturnUnderrun;
        UInt32 transferred = mpsControlLe32(out->reply + 44);
        if (transferred > length) return kIOReturnOverrun;
        actual = read ? transferred : 0;
    }
    else if (function == MPI2_FUNCTION_TOOLBOX && kind == 1) {
        if (out->replyBytes < 24) return kIOReturnUnderrun;
        actual = read ? mpsControlLe32(out->reply + 20) : 0;
    } else if (function == MPI2_FUNCTION_FW_UPLOAD && kind == 1) {
        if (out->replyBytes < 24) return kIOReturnUnderrun;
        const UInt32 imageBytes = mpsControlLe32(out->reply + 20);
        const UInt32 offset = mpsControlLe32(user + kMPSFirmwareContextOffset + 8);
        actual = offset >= imageBytes ? 0 : imageBytes - offset;
        if (actual > in.dataInBytes) actual = in.dataInBytes;
    }
    if (actual > in.dataInBytes || out->senseBytes > kMPSControlSenseBytes) return kIOReturnOverrun;
    if (out->senseBytes) memcpy(out->sense,
        (UInt8 *)fSenseBuffer->getBytesNoCopy() + kMPSControlSmid * kMPSControlSenseBytes, out->senseBytes);
    if (actual) memcpy(out + 1, dma + inputRegion, actual);
    out->dataBytes = actual;
    if (function == MPI2_FUNCTION_FW_DOWNLOAD) {
        const bool success = kind == 1 &&
            (mpsControlLe16(out->reply + 14) & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS;
        if (!success) fFirmwareDownloadPoisoned = true;
        else {
            fFirmwareOffset += length;
            if (fFirmwareOffset == fFirmwareTotal) fFirmwareDownloadActive = false;
        }
    }
    if (mutation && !fFirmwareDownloadActive) { fTopologyChanged = true; fHotplugPending = true; }
    const UInt16 status = kind == 1 ? mpsControlLe16(out->reply + 14) & MPI2_IOCSTATUS_MASK : 0;
    if (mutation || status != MPI2_IOCSTATUS_SUCCESS)
        MPS_DEBUG_LOG("MPS control: function=0x%02x handle=0x%04x mutation=%u IOC=0x%04x\n",
            function, handle, mutation, status);
    if (status != MPI2_IOCSTATUS_SUCCESS && status != MPI2_IOCSTATUS_SCSI_DATA_UNDERRUN)
        return kIOReturnIOError;
    if ((function == MPI2_FUNCTION_SMP_PASSTHROUGH || function == MPI2_FUNCTION_SATA_PASSTHROUGH) &&
        kind == 1 && out->reply[13] != 0) return kIOReturnDeviceError;
    if (scsi && kind == 1) {
        const MPSScsiIoReply *reply = (const MPSScsiIoReply *)out->reply;
        if (reply->SCSIState & MPI2_SCSI_STATE_TERMINATED) return kIOReturnAborted;
        if ((reply->SCSIState & (MPI2_SCSI_STATE_NO_SCSI_STATUS | MPI2_SCSI_STATE_AUTOSENSE_FAILED)) ||
            ((reply->SCSIState & MPI2_SCSI_STATE_RESPONSE_INFO_VALID) &&
             (reply->ResponseInfo & MPI2_SCSI_RI_MASK_REASONCODE))) return kIOReturnDeviceError;
        if (reply->SCSIStatus != MPI2_SCSI_STATUS_GOOD) return kIOReturnDeviceError;
    }
    return kIOReturnSuccess;
}

/* Diagnostic DMA survives the command that posts it. Only a successful
 * release, a checked unsolicited release notification, or a proved IOC stop
 * relinquishes firmware ownership. No close/unregister path guesses this. */
IOReturn MPSController::controlDiagnostic(const MPSControlInput &in,
    const UInt8 *request, MPSControlOutput *out)
{
    if (in.argument0 >= 3 || !fReplyLock) return kIOReturnBadArgument;
    const UInt32 type = (UInt32)in.argument0;
    DiagnosticBuffer &buffer = fDiagnostic[type];
    UInt8 *data = (UInt8 *)(out + 1);
    bool owned = false, valid = false;
    IOSimpleLockLock(fReplyLock);
    owned = buffer.owned; valid = buffer.valid;
    IOSimpleLockUnlock(fReplyLock);
    if (in.operation == kMPSControlDiagQuery) {
        if (in.requestBytes || in.dataInBytes < sizeof(MPSControlDiagInfo))
            return kIOReturnBadArgument;
        MPSControlDiagInfo *info = (MPSControlDiagInfo *)data;
        info->type = type; info->uniqueId = buffer.uniqueId; info->bytes = buffer.bytes;
        info->flags = (buffer.memory ? 1 : 0) | (owned ? 2 : 0) | (valid ? 4 : 0);
        info->epoch = buffer.epoch;
        out->dataBytes = sizeof(*info);
        return kIOReturnSuccess;
    }
    if (in.operation != kMPSControlDiagRegister &&
        (!buffer.memory || in.generation != buffer.uniqueId)) return kIOReturnNotFound;
    if (in.operation == kMPSControlDiagRead) {
        if (in.requestBytes || !buffer.memory || !valid || owned ||
            in.argument1 > buffer.bytes || in.dataInBytes > buffer.bytes - in.argument1)
            return owned ? kIOReturnBusy : kIOReturnBadArgument;
        if (in.dataInBytes) memcpy(data,
            (const UInt8 *)buffer.memory->getBytesNoCopy() + in.argument1, in.dataInBytes);
        out->dataBytes = in.dataInBytes;
        return kIOReturnSuccess;
    }
    if (!(in.flags & kMPSControlAllowMutation) || fSuspended || fReinitInProgress ||
        fProtocolRecoveryPending || fStopping || !fRequestBuffer || !fFactsValid ||
        fControlDmaBusy) return kIOReturnNotReady;
    if (in.operation == kMPSControlDiagUnregister && !owned) {
        if (in.requestBytes) return kIOReturnBadArgument;
        DiagnosticBuffer retired;
        IOSimpleLockLock(fReplyLock);
        retired = buffer;
        bzero(&buffer, sizeof(buffer));
        IOSimpleLockUnlock(fReplyLock);
        mpsReleaseResidentDma(retired.dma);
        retired.memory->release();
        return kIOReturnSuccess;
    }
    if (in.operation == kMPSControlDiagRegister) {
        const UInt32 capabilities[3] = {
            MPI2_IOCFACTS_CAPABILITY_DIAG_TRACE_BUFFER,
            MPI2_IOCFACTS_CAPABILITY_SNAPSHOT_BUFFER,
            MPI2_IOCFACTS_CAPABILITY_EXTENDED_BUFFER};
        if (!(fIocCapabilities & capabilities[type])) return kIOReturnUnsupported;
        /* A 100-byte descriptor: uniqueId, extendedType, padding and 23 product
         * words are represented by the request prefix below. Addresses and
         * lengths are always generated by the driver. */
        if (in.requestBytes != 100 || !in.argument1 || in.argument1 > kMPSControlMaxDiagBytes || (in.argument1 & 3) ||
            buffer.memory || owned || !mpsControlLe32(request) || (request[4] > 2) ||
            request[5] || request[6] || request[7] || fRequestFrameSize < 128)
            return kIOReturnBadArgument;
        UInt32 unique = mpsControlLe32(request);
        for (unsigned other = 0; other < 3; ++other)
            if (fDiagnostic[other].memory && fDiagnostic[other].uniqueId == unique)
                return kIOReturnExclusiveAccess;
        IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone,
            (UInt32)in.argument1, UINT64_C(0x00000000fffff000));
        if (!memory) return kIOReturnNoMemory;
        MPSResidentDma candidate = {};
        if (!memory->getBytesNoCopy() ||
            !mpsMapResidentDma(candidate, memory, fDmaMapper, (UInt32)in.argument1, 4)) {
            memory->release(); return kIOReturnNoResources;
        }
        memset(memory->getBytesNoCopy(), 0, (size_t)in.argument1);
        IOSimpleLockLock(fReplyLock);
        buffer.memory = memory; buffer.address = candidate.address;
        buffer.dma = candidate;
        buffer.bytes = (UInt32)in.argument1; buffer.uniqueId = unique;
        buffer.prepared = true; buffer.epoch = fTransportEpoch;
        IOSimpleLockUnlock(fReplyLock);
    } else if (in.requestBytes || !buffer.memory) return kIOReturnBadArgument;
    if (in.operation == kMPSControlDiagRelease && !owned) return kIOReturnSuccess;
    if (!beginManagementRequest(kMPSControlSmid)) return kIOReturnBusy;
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy() + kMPSControlSmid * fRequestFrameSize;
    bzero(frame, fRequestFrameSize);
    frame[1] = (UInt8)type;
    const bool posting = in.operation == kMPSControlDiagRegister;
    frame[3] = posting ? MPI2_FUNCTION_DIAG_BUFFER_POST : MPI2_FUNCTION_DIAG_RELEASE;
    if (posting) {
        frame[0] = request[4];
        mpsControlPut64(frame + 12, buffer.address);
        mpsControlPut32(frame + 20, buffer.bytes);
        memcpy(frame + 36, request + 8, 92);
        IOSimpleLockLock(fReplyLock);
        buffer.owned = true; buffer.valid = false;
        IOSimpleLockUnlock(fReplyLock);
    }
    UInt8 kind = 0;
    if (!submitControlFrame(MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE, 0,
                            mpsControlTimeout(in), out->reply, &kind)) {
        /* Conservative ownership persists even if we cannot distinguish a
         * failed post from a lost completion. Recovery owns the DMA proof. */
        return kIOReturnTimeout;
    }
    if (kind != 1 || out->reply[1] != type || out->reply[2] * 4U < 20 ||
        out->reply[2] * 4U > kMPSControlMaxReply)
        return kIOReturnIOError;
    out->replyBytes = out->reply[2] * 4U;
    UInt16 status = mpsControlLe16(out->reply + 14) & MPI2_IOCSTATUS_MASK;
    IOSimpleLockLock(fReplyLock);
    if (status == MPI2_IOCSTATUS_DIAGNOSTIC_RELEASED ||
        (!posting && status == MPI2_IOCSTATUS_SUCCESS)) {
        buffer.owned = false; buffer.valid = true;
    } else if (posting && status != MPI2_IOCSTATUS_SUCCESS) {
        buffer.owned = false; buffer.valid = false;
    }
    owned = buffer.owned;
    IOSimpleLockUnlock(fReplyLock);
    MPS_DEBUG_LOG("MPS diagnostic: type=%u operation=%u IOC=0x%04x owned=%u bytes=%u\n",
        (unsigned)type, (unsigned)in.operation, status, owned, (unsigned)buffer.bytes);
    if (status != MPI2_IOCSTATUS_SUCCESS && status != MPI2_IOCSTATUS_DIAGNOSTIC_RELEASED)
        return kIOReturnIOError;
    if (in.operation == kMPSControlDiagUnregister && !owned) {
        DiagnosticBuffer retired;
        IOSimpleLockLock(fReplyLock);
        retired = buffer;
        bzero(&buffer, sizeof(buffer));
        IOSimpleLockUnlock(fReplyLock);
        mpsReleaseResidentDma(retired.dma);
        retired.memory->release();
    }
    return kIOReturnSuccess;
}

bool MPSController::shutdownDevices(void)
{
    bool clean = true;
    const UInt64 deadline = uptimeMilliseconds() + 60000;
    bool volume = false;
    for (UInt32 i = 0; i < kMPSMaxTargets; ++i)
        if (fTargets[i].handle && fTargets[i].present && fTargets[i].kind == kMPSTargetRaidVolume) volume = true;
    if (volume && (fIocCapabilities & MPI2_IOCFACTS_CAPABILITY_INTEGRATED_RAID) &&
        fRequestBuffer && fRequestFrameSize >= 32 && beginManagementRequest(kMPSControlSmid)) {
        UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy() + kMPSControlSmid * fRequestFrameSize;
        bzero(frame, fRequestFrameSize); frame[0] = MPI2_RAID_ACTION_SYSTEM_SHUTDOWN_INITIATED;
        frame[3] = MPI2_FUNCTION_RAID_ACTION;
        mpsControlPut32(frame + 20, 0xD3000000U);
        UInt8 reply[kMPSControlMaxReply] = {}, kind = 0;
        clean = submitControlFrame(MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE, 0, 5000, reply, &kind) &&
            kind == 1 && reply[2] * 4U >= 20 && (mpsControlLe16(reply + 14) & MPI2_IOCSTATUS_MASK) == 0;
        MPS_DEBUG_LOG("MPS: IR shutdown notification=%u\n", clean);
    }
    for (UInt32 i = 0; i < kMPSMaxTargets && !fProtocolRecoveryPending; ++i) {
        MPSTargetSlot *slot = &fTargets[i];
        if (!slot->handle || !slot->present || !slot->probeValid || slot->lun || slot->managementOnly ||
            slot->kind != kMPSTargetSas || !(slot->deviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE) ||
            !fShutdownSsuPolicy || (fShutdownSsuPolicy == 1 && !slot->solidState) ||
            (fShutdownSsuPolicy == 2 && (!slot->rotationKnown || slot->solidState))) continue;
        if (uptimeMilliseconds() >= deadline) { clean = false; break; }
        if (!selectTarget(slot->key())) { clean = false; continue; }
        UInt8 cdb[6] = {0x1B, 0, 0, 0, 0, 0};
        IOReturn status = kIOReturnNotReady;
        if (!issueScsiCommand(cdb, sizeof(cdb), MPI2_SCSIIO_CONTROL_NODATATRANSFER,
                              NULL, 0, 9, &status)) clean = false;
        MPS_DEBUG_LOG("MPS: shutdown SSU handle=0x%04x SSD=%u status=0x%x\n",
                      slot->handle, slot->solidState, (unsigned)status);
    }
    return clean && !fProtocolRecoveryPending;
}
