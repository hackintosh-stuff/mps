#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <string.h>
#include "MPSController.h"
#include "MPSBlockDev.h"
#include "MPI/mpi2_target.h"
#include "MPI/mpi2_sas2.h"
#include "MPSDmaSgl.h"
#include "MPSBinding.h"
#include "MPSWarpDrive.h"
#include "MPSIdentity.h"

/* Discovery and disk-level SCSI policy. The MPI wire definitions come from
 * FreeBSD; CAM's disk-level duties are implemented here for IOBlockStorage. */
enum {
    kMPSTargetConfigBytes = 4096,
    kMPSUnmapBatchDescriptors = 32
};

static UInt16 mpsReadLe16(const UInt8 *p)
{ return (UInt16)(p[0] | ((UInt16)p[1] << 8)); }
static UInt32 mpsReadLe32(const UInt8 *p)
{ return (UInt32)p[0] | ((UInt32)p[1] << 8) |
         ((UInt32)p[2] << 16) | ((UInt32)p[3] << 24); }
static UInt64 mpsReadLe64(const UInt8 *p)
{ return (UInt64)mpsReadLe32(p) | ((UInt64)mpsReadLe32(p + 4) << 32); }

bool
MPSController::readConfigPage(UInt8 pageType, UInt8 extPageType, UInt8 pageNumber,
                              UInt32 pageAddress, UInt8 *buffer, UInt32 bufferBytes,
                              UInt32 *actualBytes, UInt16 *iocStatus)
{
    if (actualBytes) *actualBytes = 0;
    if (iocStatus) *iocStatus = 0xFFFF;
    const bool extended = pageType == MPI2_CONFIG_PAGETYPE_EXTENDED;
    const UInt32 headerBytes = extended ? 8 : 4;
    if (fProtocolRecoveryPending || !buffer || bufferBytes < headerBytes ||
        bufferBytes > kMPSTargetConfigBytes || !fRequestBuffer || !fPageBuffer ||
        fPageBuffer->getLength() < kMPSTargetConfigBytes) return false;
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy();
    if (!frame) return false;
    frame += 4 * fRequestFrameSize;
    MPSConfigRequest *config = (MPSConfigRequest *)frame;
    UInt8 replyBuf[64], kind = 0, version = 0;
    UInt32 pageBytes = 0;
    for (UInt32 phase = 0; phase < 2; ++phase) {
        if (!beginManagementRequest(4)) return false;
        memset(frame, 0, fRequestFrameSize);
        config->Function = MPI2_FUNCTION_CONFIG;
        config->Action = phase ? MPI2_CONFIG_ACTION_PAGE_READ_CURRENT : MPI2_CONFIG_ACTION_PAGE_HEADER;
        config->Header.PageType = pageType;
        config->Header.PageNumber = pageNumber;
        config->Header.PageVersion = version;
        config->Header.PageLength = extended ? 0 : (UInt8)(pageBytes / 4);
        config->ExtPageType = extended ? extPageType : 0;
        config->ExtPageLength = extended ? (UInt16)(pageBytes / 4) : 0;
        config->PageAddress = pageAddress;
        if (phase) {
            memset(fPageBuffer->getBytesNoCopy(), 0, kMPSTargetConfigBytes);
            mpsMpiSimple((UInt8 *)config + 0x1c, fPagePhys, pageBytes, false);
        }
        postDefaultRequest(4);
        if (!finishManagementRequest(4, 2000, replyBuf, &kind)) return false;
        const MPSConfigReply *reply = (const MPSConfigReply *)replyBuf;
        if (kind != 1 || reply->Function != MPI2_FUNCTION_CONFIG ||
            reply->MsgLength * 4U < sizeof(*reply) || reply->Action != config->Action) {
            recordProtocolFault(kMPSTargetMalformedReply, 4);
            return false;
        }
        UInt16 status = reply->IOCStatus & MPI2_IOCSTATUS_MASK;
        if (iocStatus) *iocStatus = status;
        if (status != MPI2_IOCSTATUS_SUCCESS) {
            if (status != MPI2_IOCSTATUS_CONFIG_INVALID_PAGE)
                MPS_DEBUG_LOG("MPS: config type=%u ext=%u page=%u address=0x%08x status=0x%04x\n",
                    pageType, extPageType, pageNumber, (unsigned)pageAddress, status);
            return false;
        }
        UInt32 bytes = (extended ? (UInt32)reply->ExtPageLength : reply->Header.PageLength) * 4;
        if ((reply->Header.PageType & 0x0F) != pageType ||
            reply->Header.PageNumber != pageNumber ||
            (extended && reply->ExtPageType != extPageType) ||
            bytes < headerBytes || bytes > bufferBytes || (phase && bytes != pageBytes)) return false;
        pageBytes = bytes;
        version = reply->Header.PageVersion;
    }
    const UInt8 *page = (const UInt8 *)fPageBuffer->getBytesNoCopy();
    if (page[0] != version || page[2] != pageNumber || (page[3] & 0x0F) != pageType ||
        (extended && page[6] != extPageType) ||
        (extended ? (UInt32)mpsReadLe16(page + 4) : page[1]) * 4 != pageBytes) return false;
    memset(buffer, 0, bufferBytes);
    memcpy(buffer, page, pageBytes);
    if (actualBytes) *actualBytes = pageBytes;
    return true;
}

bool
MPSController::readExtConfigPage(UInt8 extPageType, UInt32 pageAddress,
                                 UInt8 *buffer, UInt32 bufferBytes,
                                 UInt32 *actualBytes, UInt16 *iocStatus, UInt8 pageNumber)
{
    return readConfigPage(MPI2_CONFIG_PAGETYPE_EXTENDED, extPageType, pageNumber,
                          pageAddress, buffer, bufferBytes, actualBytes, iocStatus);
}

/* WarpDrive's internal SSD visibility is independent of ordinary RAID
 * membership. Fail closed on Page10/OEM/policy errors. DirectDrive striping
 * is deliberately not used: the firmware's ordinary IR volume path is used. */
bool MPSController::refreshWarpDrivePolicy(void)
{
    if (!fWarpDrive) return true;
    fWarpDriveHidePhysical = true;
    UInt8 page[1024]; UInt32 bytes=0; UInt16 status=0xffff;
    if (!readConfigPage(MPI2_CONFIG_PAGETYPE_MANUFACTURING,0,10,0,
                        page,sizeof(page),&bytes,&status)) {
        MPS_DEBUG_PROP("MPSWarpDrivePolicy","page10-unavailable-hide");
        return !fProtocolRecoveryPending;
    }
    const int policy=mpsWarpDrivePolicy(page,bytes);
    if (policy < 0) {
        MPS_DEBUG_PROP("MPSWarpDrivePolicy","invalid-page10-hide");
        return true;
    }
    bool volume=false;
    if (policy==2) {
        // Any volume, including offline, means physical SSDs must stay hidden.
        if (readConfigPage(MPI2_CONFIG_PAGETYPE_RAID_VOLUME,0,0,0xffff,
                           page,sizeof(page),&bytes,&status)) {
            if (bytes<0x28) return false;
            volume=true;
        } else if (status==MPI2_IOCSTATUS_CONFIG_INVALID_PAGE&&!fProtocolRecoveryPending)
            volume=false;
        else return false;
    }
    fWarpDriveHidePhysical=mpsWarpDriveHidesPhysical(policy,true,volume);
    MPS_DEBUG_PROP("MPSWarpDrivePolicy",(unsigned long long)policy,8);
    MPS_DEBUG_PROP("MPSWarpDriveHidePhysical",fWarpDriveHidePhysical);
    return true;
}

MPSTargetSlot *MPSController::findBoundTarget(UInt64 binding)
{
    // Caller owns fIOLock. A firmware handle is resolved only after token proof.
    for (UInt32 i=0;i<kMPSMaxTargets;++i) {
        MPSTargetSlot *slot=&fTargets[i];
        if (mpsBindingMatches(binding,slot->bindingId,
            slot->handle && slot->present && slot->probeValid && !slot->offlinePending))
            return slot;
    }
    return NULL;
}

bool MPSController::copyBoundMedia(UInt64 binding,UInt64 *blocks,UInt64 *size)
{
    if (!blocks||!size||!fIOLock) return false;
    IOLockLock(fIOLock);
    MPSTargetSlot *slot=findBoundTarget(binding);
    bool valid=slot&&!fStopping&&!fSuspended;
    *blocks=valid?slot->capacityBlocks:0; *size=valid?slot->blockSize:0;
    IOLockUnlock(fIOLock);
    return valid;
}

bool MPSController::retireNubBinding(MPSTargetSlot *slot)
{
    if (!slot->nub) return true;
    if (fRetiredNubCount==kMPSMaxTargets) return false;
    // Transfer the table's owning reference; no extra retain to leak.
    fRetiredNubs[fRetiredNubCount++]=slot->nub;
    slot->nub=NULL; slot->online=false; slot->onlinePending=false;
    slot->offlinePending=false;
    return true;
}

void MPSController::drainRetiredNubs(void)
{
    // Called without the transport lock; client termination may re-enter.
    MPSBlockDev *nubs[kMPSMaxTargets]; UInt32 count=0;
    IOLockLock(fIOLock);
    count=fRetiredNubCount;
    memcpy(nubs,fRetiredNubs,count*sizeof(nubs[0]));
    memset(fRetiredNubs,0,sizeof(fRetiredNubs)); fRetiredNubCount=0;
    IOLockUnlock(fIOLock);
    for(UInt32 i=0;i<count;++i) {
        (void)nubs[i]->notifyMediaState(kIOMediaStateOffline);
        nubs[i]->setController(NULL);
        nubs[i]->terminate(); nubs[i]->release();
    }
}

bool
MPSController::readRaidMembership(UInt16 handle, bool *member)
{
    if (member == NULL) return false;
    *member = false;
    if ((fIocCapabilities & MPI2_IOCFACTS_CAPABILITY_INTEGRATED_RAID) == 0)
        return true;
    UInt8 page[kMPSTargetConfigBytes], seen[32] = {};
    UInt32 cursor = 0xFF;
    for (UInt32 n = 0; n < 256; n++) {
        UInt32 actual = 0;
        UInt16 status = 0xFFFF;
        if (!readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_RAID_CONFIG,
                               MPI2_RAID_PGAD_FORM_GET_NEXT_CONFIGNUM | cursor,
                               page, sizeof(page), &actual, &status)) {
            /* INVALID_PAGE is the documented end of GET_NEXT enumeration,
             * never a generic permission to expose a disk after an error. */
            if (status == MPI2_IOCSTATUS_CONFIG_INVALID_PAGE &&
                !fProtocolRecoveryPending) return true;
            MPS_DEBUG_PROP("MPSRaidFilterState", "configuration-unavailable");
            return false;
        }
        bool contains = false;
        if (!mpsRaidConfigContains(page, actual, handle, &contains)) {
            MPS_DEBUG_PROP("MPSRaidFilterState", "malformed-configuration");
            return false;
        }
        UInt8 number = page[0x0B];
        if ((seen[number / 8] & (1U << (number % 8))) != 0) return false;
        seen[number / 8] |= (UInt8)(1U << (number % 8));
        cursor = number;
        if (contains) { *member = true; return true; }
    }
    MPS_DEBUG_PROP("MPSRaidFilterState", "configuration-enumeration-limit");
    return false;
}

bool
MPSController::readSasDevicePage0(UInt32 devHandle, UInt32 *outDeviceInfo)
{
    MPSTargetSlot *slot = findTarget((UInt16)devHandle);
    if (slot && slot->kind != kMPSTargetSas) {
        bool present = false;
        UInt64 address = 0;
        if (!readAdditionalTarget(slot, &present, &address) || !present || address != slot->sasAddress)
            return false;
        fTargetDeviceInfo = slot->deviceInfo;
        fTargetDeviceFlags = slot->flags;
        fTargetSasAddress = address;
        if (outDeviceInfo) *outDeviceInfo = slot->deviceInfo;
        return true;
    }
    UInt8 page[512];
    UInt32 actual = 0;
    if (!readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
                           MPI2_SAS_DEVICE_PGAD_FORM_HANDLE | devHandle,
                           page, sizeof(page), &actual)) return false;
    if (actual < 0x22 || mpsReadLe16(page + 0x18) != devHandle ||
        page[0x17] != 0) {
        fSasDeviceMismatch = true;
        return false;
    }
    UInt16 flags = mpsReadLe16(page + 0x20);
    if ((flags & MPI2_SAS_DEVICE0_FLAGS_DEVICE_PRESENT) == 0 ||
        (flags & (MPI2_SAS_DEVICE0_FLAGS_UNSUPPORTED_DEVICE |
                  MPI2_SAS_DEVICE0_FLAGS_UNAUTHORIZED_DEVICE)) != 0) return false;
    UInt32 info = mpsReadLe32(page + 0x1C);
    if (outDeviceInfo != NULL) *outDeviceInfo = info;
    fTargetDeviceInfo = info;
    fTargetDeviceFlags = flags;
    fTargetSasAddress = mpsReadLe64(page + 0x0C);
    char address[24];
    snprintf(address, sizeof(address), "%016llx", (unsigned long long)fTargetSasAddress);
    MPS_DEBUG_PROP("MPSSasDeviceSasAddress", address);
    MPS_DEBUG_PROP("MPSSasDeviceHandle", (unsigned long long)devHandle, 16);
    MPS_DEBUG_PROP("MPSSasDeviceInfo", (unsigned long long)info, 32);
    MPS_DEBUG_PROP("MPSSasDeviceFlags", (unsigned long long)flags, 16);
    MPS_DEBUG_PROP("MPSSasDeviceParent", (unsigned long long)mpsReadLe16(page + 0x14), 16);
    MPS_DEBUG_PROP("MPSSasDeviceSlot", (unsigned long long)mpsReadLe16(page + 0x08), 16);
    MPS_DEBUG_PROP("MPSSasDeviceEnclosure", (unsigned long long)mpsReadLe16(page + 0x0A), 16);
    MPS_DEBUG_PROP("MPSSasDevicePhyNum", (unsigned long long)page[0x16], 8);
    return true;
}

UInt32
MPSController::nextTargetGeneration(void)
{
    if (fTargetGenerationSerial == UINT32_MAX) {
        recordProtocolFault(kMPSTargetMalformedReply, 0);
        return UINT32_MAX; // Never wrap into a cookie held by an old request.
    }
    return ++fTargetGenerationSerial;
}

bool
MPSController::mergeTarget(MPSTargetKind kind, UInt16 handle, UInt64 address,
                           UInt32 info, UInt16 flags, UInt32 token,
                           UInt32 maxTransfer, UInt8 resetTimeout, UInt32 lun)
{
    UInt8 wireLun[8];
    if (!handle || handle == 0xFFFF || !address || !mpsEncodeLun(wireLun, lun)) return false;
    MPSTargetSlot *slot = NULL;
    for (UInt32 i = 0; i < kMPSMaxTargets; ++i)
        if (fTargets[i].sasAddress == address && fTargets[i].kind == kind && fTargets[i].lun == lun) {
            slot = &fTargets[i]; break;
        }
    if (!slot) {
        slot = allocTargetSlot();
        if (!slot) { MPS_DEBUG_PROP("MPSTopologyFailure", "too-many-logical-targets"); return false; }
        MPSBlockDev *nub = slot->nub;
        bzero(slot, sizeof(*slot)); slot->nub = nub;
        slot->kind = kind; slot->sasAddress = address; slot->lun = lun;
        if (fBindingSerial==UINT64_MAX) return false;
        slot->bindingId=++fBindingSerial;
        slot->handle = handle; slot->generation = nextTargetGeneration();
    }
    if (slot->scanToken != token) {
        slot->scanToken = token; slot->pathCount = 0;
    }
    for (UInt32 i = 0; i < slot->pathCount; ++i)
        if (slot->paths[i].handle == handle) return true;
    if (slot->pathCount == MPSTargetSlot::kPathLimit) {
        MPS_DEBUG_PROP("MPSTopologyFailure", "too-many-paths-per-logical-target"); return false;
    }
    MPSTargetSlot::Path &path = slot->paths[slot->pathCount++];
    path.handle = handle; path.info = info; path.flags = flags;
    path.maxTransfer = maxTransfer; path.resetTimeout = resetTimeout;
    /* Only collect here. Do not change a live handle merely because GET_NEXT
     * happens to enumerate its standby path before the active path. */
    return !fProtocolRecoveryPending;
}

bool
MPSController::finalizeTargetPaths(UInt32 token)
{
    UInt32 selected[kMPSMaxTargets];
    for (UInt32 i = 0; i < kMPSMaxTargets; ++i) {
        MPSTargetSlot &slot = fTargets[i]; selected[i] = UINT32_MAX;
        if (slot.scanToken != token || !slot.pathCount) continue;
        selected[i] = 0;
        for (UInt32 p = 0; p < slot.pathCount; ++p)
            if (slot.paths[p].handle == slot.handle) { selected[i] = p; break; }
        for (UInt32 j = 0; j < i; ++j)
            if (selected[j] != UINT32_MAX && fTargets[j].lun == slot.lun &&
                fTargets[j].paths[selected[j]].handle == slot.paths[selected[i]].handle) {
                MPS_DEBUG_PROP("MPSTopologyFailure", "handle-has-conflicting-identities"); return false;
            }
    }
    /* A newly enumerated identity may occupy an old handle. Retire the old
     * binding without resetting/removing the replacement. Monotonic cookies
     * make already accepted old requests fail even if table slots are recycled. */
    for (UInt32 i = 0; i < kMPSMaxTargets; ++i) {
        MPSTargetSlot &old = fTargets[i];
        if (selected[i] != UINT32_MAX || !old.handle) continue;
        for (UInt32 j = 0; j < kMPSMaxTargets; ++j) {
            if (selected[j] == UINT32_MAX || old.lun != fTargets[j].lun ||
                old.handle != fTargets[j].paths[selected[j]].handle) continue;
            invalidateTarget(&old); old.present = false; old.removalRequested = false;
            old.handle = 0; setTargetRemovalState(&old, "handle-reassigned"); break;
        }
    }
    for (UInt32 i = 0; i < kMPSMaxTargets; ++i) {
        if (selected[i] == UINT32_MAX) continue;
        MPSTargetSlot &slot = fTargets[i];
        const MPSTargetSlot::Path &path = slot.paths[selected[i]];
        const bool changed = !slot.present || slot.handle != path.handle;
        if (changed) {
            invalidateTarget(&slot); slot.probeRetries = 0; slot.probeState = kMPSProbeOk;
            MPS_DEBUG_LOG("MPS: select path kind=%u WWID=%016llx LUN=%u handle=0x%04x -> 0x%04x paths=%u\n",
                (unsigned)slot.kind, (unsigned long long)slot.sasAddress, (unsigned)slot.lun,
                slot.handle, path.handle, (unsigned)slot.pathCount);
        }
        slot.handle = path.handle; slot.deviceInfo = path.info; slot.flags = path.flags;
        slot.maxTransferBytes = path.maxTransfer; slot.resetTimeoutSeconds = path.resetTimeout;
        slot.present = true; slot.hiddenByRaid = false; slot.removalRequested = false;

    }
    return !fProtocolRecoveryPending;
}

static bool mpsRaidVolumeUsable(const UInt8 *page, UInt32 bytes)
{
    return bytes >= 0x28 && (page[6] == MPI2_RAID_VOL_STATE_ONLINE ||
        page[6] == MPI2_RAID_VOL_STATE_DEGRADED || page[6] == MPI2_RAID_VOL_STATE_OPTIMAL) &&
        (mpsReadLe32(page + 8) & MPI2_RAIDVOL0_STATUS_FLAG_VOLUME_INACTIVE) == 0;
}

bool
MPSController::scanAdditionalTargets(UInt32 token, UInt32 *selected)
{
    UInt32 count=0;
    if (fIocCapabilities & MPI2_IOCFACTS_CAPABILITY_INTEGRATED_RAID) {
        UInt8 page[1024], identity[64];
        UInt32 cursor=0xffff, bound=fMaxDeviceHandle?fMaxDeviceHandle:0xfffe;
        bool finished=false;
        for(UInt32 n=0;n<=bound;++n) {
            UInt32 bytes=0; UInt16 status=0xffff;
            if(!readConfigPage(MPI2_CONFIG_PAGETYPE_RAID_VOLUME,0,0,cursor,
                              page,sizeof(page),&bytes,&status)) {
                if(status==MPI2_IOCSTATUS_CONFIG_INVALID_PAGE&&!fProtocolRecoveryPending) {
                    finished=true;break;
                }
                return false;
            }
            if(bytes<0x28) return false;
            UInt16 handle=mpsReadLe16(page+4);
            if(!handle||handle==0xffff||handle>bound||
               (cursor!=0xffff&&handle<=cursor)) return false;
            cursor=handle;
            if(!mpsRaidVolumeUsable(page,bytes)) continue;
            if(!readConfigPage(MPI2_CONFIG_PAGETYPE_RAID_VOLUME,0,1,
                MPI2_RAID_VOLUME_PGAD_FORM_HANDLE|handle,identity,sizeof(identity),&bytes)||
                bytes<0x38||mpsReadLe16(identity+4)!=handle) return false;
            if(!mergeTarget(kMPSTargetRaidVolume,handle,mpsReadLe64(identity+0x30),
                            0,0,token)) return false;
            *selected=handle; ++count;
        }
        if(!finished) return false;
    }
    MPS_DEBUG_PROP("MPSRaidVolumeCount",(unsigned long long)count,32);
    return true;
}

bool
MPSController::readAdditionalTarget(MPSTargetSlot *slot, bool *present, UInt64 *address)
{
    if (!slot || slot->kind == kMPSTargetSas) return false;
    *present = false;
    *address = 0;
    UInt8 page[1024];
    UInt32 bytes = 0;
    UInt16 status = 0xFFFF;
    if (slot->kind == kMPSTargetRaidVolume) {
        if (!readConfigPage(MPI2_CONFIG_PAGETYPE_RAID_VOLUME, 0, 0,
            MPI2_RAID_VOLUME_PGAD_FORM_HANDLE | slot->handle,
            page, sizeof(page), &bytes, &status)) return status == MPI2_IOCSTATUS_CONFIG_INVALID_PAGE;
        if (bytes < 0x28 || mpsReadLe16(page + 4) != slot->handle) return false;
        const bool usable = mpsRaidVolumeUsable(page, bytes);
        if (!readConfigPage(MPI2_CONFIG_PAGETYPE_RAID_VOLUME, 0, 1,
            MPI2_RAID_VOLUME_PGAD_FORM_HANDLE | slot->handle,
            page, sizeof(page), &bytes, &status) || bytes < 0x38 ||
            mpsReadLe16(page + 4) != slot->handle) return false;
        *address = mpsReadLe64(page + 0x30);
        *present = usable;
    } else return false;
    return true;
}

bool
MPSController::discoverLogicalUnits(UInt32 token)
{
    const UInt32 allocation = 8 + 8 * kMPSMaxTargets;
    UInt32 luns[kMPSMaxTargets];
    for (UInt32 i = 0; i < kMPSMaxTargets; ++i) {
        MPSTargetSlot *base = &fTargets[i];
        if (!base->handle || base->lun || base->scanToken != token || !base->present ||
            base->kind == kMPSTargetRaidVolume ||
            (base->kind == kMPSTargetSas && (base->deviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE))) continue;
        if (!allocateResidentBuffers()) return false;
        fAttachedDevHandle = base->handle;
        fAttachedLun = 0;
        UInt8 cdb[12] = {0xA0};
        mpsWriteBe32(cdb + 6, allocation);
        IOReturn status = kIOReturnNotReady;
        UInt32 actual = 0;
        bool ok = issueScsiCommand(cdb, sizeof(cdb), MPI2_SCSIIO_CONTROL_READ,
                                   NULL, allocation, 8, &status, &actual, true);
        if (!ok) {
            if (fProtocolRecoveryPending) return false;
            /* Unsupported REPORT LUNS keeps the legacy LUN-zero path. A
             * temporary NOT READY must not discard previously known LUNs. */
            for (UInt32 j = 0; j < kMPSMaxTargets; ++j)
                if (fTargets[j].handle != 0 && fTargets[j].lun &&
                    fTargets[j].kind == base->kind && fTargets[j].sasAddress == base->sasAddress)
                {
                    UInt32 lun = fTargets[j].lun;
                    for (UInt32 p = 0; p < base->pathCount; ++p) {
                        const MPSTargetSlot::Path &path = base->paths[p];
                        if (!mergeTarget(base->kind, path.handle, base->sasAddress, path.info,
                            path.flags, token, path.maxTransfer, path.resetTimeout, lun)) return false;
                    }
                }
            MPS_DEBUG_PROP("MPSReportLunsStatus", (unsigned long long)(UInt32)status, 32);
            continue;
        }
        const UInt8 *data = (const UInt8 *)fDataBuffer->getBytesNoCopy();
        if (actual < 8) return false;
        UInt32 bytes = mpsReadBe32(data);
        if ((bytes & 7) || bytes > actual - 8 || bytes / 8 > kMPSMaxTargets) {
            MPS_DEBUG_PROP("MPSTopologyFailure", "REPORT-LUNS-truncated-or-malformed"); return false;
        }
        UInt32 count = 0;
        for (UInt32 off = 8; off < bytes + 8; off += 8) {
            UInt32 lun = 0;
            if (!mpsDecodeLun(data + off, &lun)) {
                MPS_DEBUG_PROP("MPSSkippedLunAddress", "unsupported-SAM-address-method"); continue;
            }
            bool duplicate = false;
            for (UInt32 j = 0; j < count; ++j) if (luns[j] == lun) duplicate = true;
            if (!duplicate) luns[count++] = lun;
        }
        bool zeroPresent = false;
        for (UInt32 j = 0; j < count; ++j) {
            if (!luns[j]) { zeroPresent = true; continue; }
            for (UInt32 p = 0; p < base->pathCount; ++p) {
                const MPSTargetSlot::Path &path = base->paths[p];
                if (!mergeTarget(base->kind, path.handle, base->sasAddress, path.info,
                    path.flags, token, path.maxTransfer, path.resetTimeout, luns[j])) return false;
            }
        }
        /* Retain an addressable LUN-zero anchor even when it is not a disk;
         * it owns physical removal and future REPORT LUNS, not an IOMedia. */
        if (!zeroPresent) {
            base->managementOnly = true;
            if (base->nub && base->online) { base->online = false; base->offlinePending = true; }
        } else if (base->managementOnly && base->peripheralType == 0) {
            base->managementOnly = false; base->probeValid = false;
        }
    }
    fAttachedLun = 0;
    return true;
}

bool
MPSController::readSasIOUnitPage0(void)
{
    UInt8 page[kMPSTargetConfigBytes];
    UInt32 actual = 0, links = 0, selected = 0, candidates = 0, cursor = 0xFFFF;
    UInt32 disks = 0;
    bool expander = false;
    UInt16 iouStatus = 0xFFFF;
    if (!readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_IO_UNIT, 0,
                           page, sizeof(page), &actual)) return false;
    if (!refreshWarpDrivePolicy()) return false;
    if (actual < 0x10) return false;
    UInt32 phys = page[0x0C];
    if (phys > (actual - 0x10) / 0x14) return false;
    for (UInt32 i = 0; i < phys; i++) {
        const UInt8 *phy = page + 0x10 + i * 0x14;
        if ((phy[3] & 0x0F) >= 8) links++;
        char name[32];
        snprintf(name, sizeof(name), "MPSPhy%uDevHandle", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)mpsReadLe16(phy + 8), 16);
        snprintf(name, sizeof(name), "MPSPhy%uLinkRate", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)(phy[3] & 0x0F), 8);
    }
    MPS_DEBUG_PROP("MPSNumPhys", (unsigned long long)phys, 8);
    fSasDeviceMismatch = false;

    /* GET_NEXT_HANDLE reaches the terminal disk behind a backplane/expander.
     * IOC PHY handles alone can name the expander, not its attached disk.
     * The scan is merged into the persistent target table instead of replacing
     * it: a target that stays present keeps its nub and its medium, and only
     * its current device page fields are refreshed. */
    bool finished = false;
    const UInt32 token = ++fScanSerial;
    UInt32 bound = fMaxDeviceHandle != 0 ? fMaxDeviceHandle : 0xFFFE;
    for (UInt32 n = 0; n <= bound; n++) {
        UInt16 status = 0xFFFF;
        if (!readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
                               MPI2_SAS_DEVICE_PGAD_FORM_GET_NEXT_HANDLE | cursor,
                               page, sizeof(page), &actual, &status)) {
            if (status == MPI2_IOCSTATUS_CONFIG_INVALID_PAGE &&
                !fProtocolRecoveryPending) { finished = true; break; }
            return false;
        }
        if (actual < 0x22) return false;
        UInt16 handle = mpsReadLe16(page + 0x18);
        if (handle == 0 || handle == 0xFFFF || handle > bound ||
            (cursor != 0xFFFF && handle <= cursor)) return false;
        cursor = handle;
        UInt32 info = mpsReadLe32(page + 0x1C);
        UInt16 flags = mpsReadLe16(page + 0x20);
        if ((info & 7) == 2 || (info & 7) == 3) expander = true;
        /* SES/vSEP enclosure processors behind an expander are SSP end
         * devices too, but not block disks. Counting them as standalone
         * targets makes the single-target guard refuse the real disk. */
        if ((info & 7) == MPI2_SAS_DEVICE_INFO_END_DEVICE &&
            (info & (MPI2_SAS_DEVICE_INFO_SATA_DEVICE | MPI2_SAS_DEVICE_INFO_STP_TARGET |
                     MPI2_SAS_DEVICE_INFO_SSP_TARGET)) != 0 &&
            (info & (MPI2_SAS_DEVICE_INFO_ATAPI_DEVICE | MPI2_SAS_DEVICE_INFO_SEP)) == 0 &&
            (flags & MPI2_SAS_DEVICE0_FLAGS_DEVICE_PRESENT) != 0) disks++;
        if ((info & 7) != MPI2_SAS_DEVICE_INFO_END_DEVICE ||
            (info & (MPI2_SAS_DEVICE_INFO_SATA_DEVICE | MPI2_SAS_DEVICE_INFO_STP_TARGET |
                     MPI2_SAS_DEVICE_INFO_SSP_TARGET)) == 0 ||
            page[0x17] != 0 ||
            (flags & MPI2_SAS_DEVICE0_FLAGS_DEVICE_PRESENT) == 0 ||
            (flags & (MPI2_SAS_DEVICE0_FLAGS_UNSUPPORTED_DEVICE |
                      MPI2_SAS_DEVICE0_FLAGS_UNAUTHORIZED_DEVICE)) != 0) continue;
        bool raid = false;
        if (!readRaidMembership(handle, &raid)) return false;
        if (fWarpDrive ? fWarpDriveHidePhysical : raid) {
            MPS_DEBUG_PROP("MPSRaidExcludedHandle", (unsigned long long)handle, 16);
            continue;
        }
        const UInt64 address = mpsReadLe64(page + 0x0C);
        if (!mergeTarget(kMPSTargetSas, handle, address, info, flags, token)) return false;
        candidates++;
        selected = handle;
    }
    if (!finished || !scanAdditionalTargets(token, &selected) || !finalizeTargetPaths(token) ||
        !discoverLogicalUnits(token) || !finalizeTargetPaths(token)) return false;
    selected = 0;
    for (UInt32 i = 0; i < kMPSMaxTargets; ++i)
        if (fTargets[i].scanToken == token && !fTargets[i].lun && fTargets[i].present)
            selected = fTargets[i].handle;

    /* Every handle the firmware stopped listing is a removal candidate. The
     * medium stays invalid until the guarded TARGET_RESET/REMOVE_DEVICE
     * sequence finishes, so queued I/O cannot reach a replacement device. */
    for (UInt32 i = 0; i < kMPSMaxTargets; i++) {
        MPSTargetSlot *slot = &fTargets[i];
        if (slot->handle == 0 || slot->scanToken == token || !slot->present)
            continue;
        slot->present = false;
        slot->probeValid = false;
        if (slot->online || slot->nub != NULL) {
            slot->online = false;
            slot->offlinePending = true;
        }
        slot->onlinePending = false;
        requestTargetRemoval(slot);
    }
    fTargetCount = 0;
    for (UInt32 i = 0; i < kMPSMaxTargets; i++)
        if (fTargets[i].handle != 0) fTargetCount++;

    MPS_DEBUG_PROP("MPSStandaloneTargetCount", (unsigned long long)candidates, 32);
    MPS_DEBUG_PROP("MPSTargetCount", (unsigned long long)fTargetCount, 32);
    publishTargetProperties();
    /* Every eligible target stays in the table; publication is per target.
     * The selected handle's own device page is a cross-check, not a gate: one
     * unreadable page must not keep the other targets unpublished. */
    if (selected != 0 && !readSasDevicePage0(selected)) {
        MPS_DEBUG_PROP("MPSSasDevicePageState", "unavailable");
    } else {
        MPS_DEBUG_REMOVE("MPSSasDevicePageState");
    }
    fAttachedDevHandle = selected;
    fAttachedLun = 0;
    fLinkUpPhys = links;
    fHasExpander = expander;
    fDiscoveredDiskCount = disks;
    if (selected == 0) {
        fTargetDeviceInfo = 0;
        fTargetDeviceFlags = 0;
        fTargetSasAddress = 0;
    }
    fIdentityLostCount = 0;
    MPS_DEBUG_PROP("MPSLinkUpPhys", (unsigned long long)links, 8);
    MPS_DEBUG_PROP("MPSAttachedDevHandle", (unsigned long long)selected, 16);
    MPS_DEBUG_PROP("MPSAttachedDeviceInfo", (unsigned long long)fTargetDeviceInfo, 32);
    MPS_DEBUG_PROP("MPSTopologyHasExpander", fHasExpander);
    MPS_DEBUG_PROP("MPSRaidFilterState", "volumes-visible-members-hidden");
    MPS_DEBUG_REMOVE("MPSTopologyFailure");
    return true;
}

bool
MPSController::scsiInquiryVpd(UInt8 page, UInt8 *buf, UInt32 len,
                              UInt32 *actualBytes)
{
    if (actualBytes != NULL) *actualBytes = 0;
    if (buf == NULL || len < 4 || len > 0xFFFF || fDataBuffer == NULL) return false;
    UInt8 cdb[6] = { 0x12, 1, page, (UInt8)(len >> 8), (UInt8)len, 0 };
    UInt32 actual = 0;
    IOReturn status = kIOReturnIOError;
    if (!issueScsiCommand(cdb, sizeof(cdb), MPI2_SCSIIO_CONTROL_READ,
                          NULL, len, 8, &status, &actual, true)) return false;
    if (actual < 4 || actual > len) return false;
    const UInt8 *data = (const UInt8 *)fDataBuffer->getBytesNoCopy();
    if (!mpsVpdPayload(data, actual, page, 0, NULL)) return false;
    memset(buf, 0, len);
    memcpy(buf, data, actual);
    if (actualBytes != NULL) *actualBytes = actual;
    return true;
}

static bool
mpsHexIdentity(char *out, size_t capacity, const char *prefix,
                 const UInt8 *bytes, size_t length)
{
    static const char hex[] = "0123456789abcdef";
    size_t prefixBytes = strlen(prefix);
    if (length == 0 || prefixBytes + 1 >= capacity ||
        length > (capacity - prefixBytes - 2) / 2) return false;
    bool nonzero = false;
    for (size_t i = 0; i < length; i++) nonzero |= bytes[i] != 0;
    if (!nonzero) return false;
    memcpy(out, prefix, prefixBytes);
    out[prefixBytes++] = ':';
    for (size_t i = 0; i < length; i++) {
        out[prefixBytes++] = hex[bytes[i] >> 4];
        out[prefixBytes++] = hex[bytes[i] & 0x0F];
    }
    out[prefixBytes] = 0;
    return true;
}

bool
MPSController::scsiSataIdentify(UInt8 identify[512])
{
    if ((fTargetDeviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE) == 0 ||
        fDataBuffer == NULL || fRequestBuffer == NULL || !beginManagementRequest(5))
        return false;
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy() + 5 * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    MPSSataPassthroughRequest *request = (MPSSataPassthroughRequest *)frame;
    request->DevHandle = (UInt16)fAttachedDevHandle;
    request->Function = MPI2_FUNCTION_SATA_PASSTHROUGH;
    request->PassthroughFlags = MPI2_SATA_PT_REQ_PT_FLAGS_PIO | MPI2_SATA_PT_REQ_PT_FLAGS_READ;
    request->DataLength = 512;
    request->CommandFIS[0] = 0x27;
    request->CommandFIS[1] = 0x80;
    request->CommandFIS[2] = 0xEC;
    mpsMpiSimple((UInt8 *)request + 0x30, fDataPhys, 512, false);
    memset(fDataBuffer->getBytesNoCopy(), 0, 512);
    postDefaultRequest(5);
    UInt8 replyBytes[64], kind = 0;
    if (!finishManagementRequest(5, 15000, replyBytes, &kind)) return false;
    const MPSSataPassthroughReply *reply = (const MPSSataPassthroughReply *)replyBytes;
    if (kind != 1 || reply->Function != MPI2_FUNCTION_SATA_PASSTHROUGH ||
        reply->MsgLength * 4U < sizeof(*reply) || reply->DevHandle != fAttachedDevHandle) {
        recordProtocolFault(kMPSTargetMalformedReply, 5);
        return false;
    }
    if ((reply->IOCStatus & MPI2_IOCSTATUS_MASK) != MPI2_IOCSTATUS_SUCCESS ||
        reply->SASStatus != 0 || (reply->StatusFIS[2] & 1) != 0 ||
        reply->TransferCount != 512) return false;
    memcpy(identify, fDataBuffer->getBytesNoCopy(), 512);
    return mpsReadLe16(identify) != 0xFFFF;
}

bool
MPSController::probeStableIdentity(void)
{
    UInt8 page[512];
    UInt32 actual = 0;
    fTargetIdentityValid = false;
    fTargetIdentity[0] = 0;
    if (!allocateResidentBuffers()) return false;
    /* SATA addresses exposed by the IOC can follow the port. Prefer the
     * device's model/serial identity, as the FreeBSD SATA mapping path does. */
    if (scsiSataIdentify(page)) {
        UInt8 key[60];
        char model[41], serialText[21];
        for (UInt32 i = 0; i < 40; i++) {
            key[i] = page[54 + (i ^ 1)];
            model[i] = (char)key[i];
        }
        for (UInt32 i = 0; i < 20; i++) {
            key[40 + i] = page[20 + (i ^ 1)];
            serialText[i] = (char)key[40 + i];
        }
        model[40] = serialText[20] = 0;
        for (UInt32 i = 40; i > 0 && model[i - 1] == ' '; i--) model[i - 1] = 0;
        for (UInt32 i = 20; i > 0 && serialText[i - 1] == ' '; i--) serialText[i - 1] = 0;
        strlcpy(fTargetModel, model, sizeof(fTargetModel));
        strlcpy(fTargetSerial, serialText, sizeof(fTargetSerial));
        if (mpsSerialIsTrusted(key+40,20) && strcmp(serialText,model)!=0)
            fTargetIdentityValid = mpsHexIdentity(fTargetIdentity,
            sizeof(fTargetIdentity), "ata-model-serial", key, sizeof(key));
        MPS_DEBUG_PROP("MPSSataSolidState", mpsReadLe16(page + 434) == 1);
    }
    if (!fTargetIdentityValid && !fProtocolRecoveryPending &&
        scsiInquiryVpd(0x83, page, sizeof(page), &actual)) {
        UInt32 end = 4 + mpsReadBe16(page + 2), best = 0, bestRank = 0;
        bool valid = true;
        for (UInt32 offset = 4; offset < end;) {
            if (end - offset < 4 || page[offset + 3] > end - offset - 4) {
                valid = false; break;
            }
            UInt8 type = page[offset + 1] & 0x0F;
            UInt8 association = (page[offset + 1] >> 4) & 3;
            UInt8 codeSet = page[offset] & 0x0F;
            UInt8 length = page[offset + 3];
            UInt32 rank = type == 3 ? 4 : type == 2 ? 3 : type == 8 ? 2 : type == 1 ? 1 : 0;
            if (mpsVpd83IdentifierTrusted(association,codeSet,type,page+offset+4,length) &&
                rank > bestRank) { best = offset; bestRank = rank; }
            offset += 4 + length;
        }
        if (valid && bestRank != 0) {
            char prefix[16];
            snprintf(prefix, sizeof(prefix), "vpd83-%u", (unsigned)(page[best + 1] & 0x0F));
            fTargetIdentityValid = mpsHexIdentity(fTargetIdentity, sizeof(fTargetIdentity),
                                                  prefix, page + best + 4, page[best + 3]);
        }
    }
    if (!fTargetIdentityValid && !fProtocolRecoveryPending &&
        !(fTargetDeviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE) &&
        scsiInquiryVpd(0x80, page, sizeof(page), &actual)) {
        UInt32 serialBytes = mpsReadBe16(page + 2);
        while (serialBytes != 0 && page[4 + serialBytes - 1] == ' ') serialBytes--;
        if (mpsSerialIsTrusted(page+4,serialBytes)) {
            UInt8 key[164] = {};
            memcpy(key, fVendor, 16);
            memcpy(key + 16, fProduct, 20);
            memcpy(key + 36, page + 4, serialBytes);
            fTargetIdentityValid = mpsHexIdentity(fTargetIdentity, sizeof(fTargetIdentity),
                                                  "vpd80", key, 36 + serialBytes);
        }
    }
    MPSTargetSlot *identitySlot = findTarget(currentTargetKey());
    if (!fTargetIdentityValid && !fProtocolRecoveryPending && identitySlot &&
        identitySlot->kind != kMPSTargetSas && identitySlot->sasAddress != 0) {
        bool present = false; UInt64 address = 0;
        if (readAdditionalTarget(identitySlot, &present, &address) && present &&
            address == identitySlot->sasAddress) {
            snprintf(fTargetIdentity, sizeof(fTargetIdentity), "%s-%016llx-lun%u",
                "raid-wwid",
                (unsigned long long)address, (unsigned)identitySlot->lun);
            fTargetIdentityValid = true;
        }
    }
    MPS_DEBUG_PROP("MPSStableIdentityValid", fTargetIdentityValid);
    if (fTargetIdentityValid) MPS_DEBUG_PROP("MPSStableIdentity", fTargetIdentity);
    else MPS_DEBUG_REMOVE("MPSStableIdentity");
    return fTargetIdentityValid && !fProtocolRecoveryPending;
}

MPSTargetSlot *
MPSController::findTarget(UInt64 handle)
{
    if (handle == 0) return NULL;
    for (UInt32 i = 0; i < kMPSMaxTargets; i++)
        if (fTargets[i].handle != 0 && fTargets[i].key() == handle) return &fTargets[i];
    return NULL;
}

bool
MPSController::probeScsiIdentity(void)
{
    if (fProtocolRecoveryPending || !allocatePageBuffer()) return false;
    bool ready = scsiTur();
    MPS_DEBUG_PROP("MPSTurStatus", ready ? "ok" : "failed");
    bool ok = !fProtocolRecoveryPending && scsiInquiry();
    MPSTargetSlot *target = findTarget(currentTargetKey());
    if (ok && target && target->managementOnly) {
        UInt8 pages[256]; UInt32 actual = 0;
        target->tlrEnabled = false;
        if (target->peripheralType == 1 && target->kind == kMPSTargetSas &&
            (target->deviceInfo & MPI2_SAS_DEVICE_INFO_SSP_TARGET) &&
            (fIocCapabilities & MPI2_IOCFACTS_CAPABILITY_TLR) &&
            scsiInquiryVpd(0, pages, sizeof(pages), &actual)) {
            for (UInt32 i = 4; i < actual; ++i)
                if (pages[i] == 0x90) target->tlrEnabled = true;
        }
        memcpy(target->vendor, fVendor, sizeof(target->vendor));
        memcpy(target->product, fProduct, sizeof(target->product));
        memcpy(target->revision, fRevision, sizeof(target->revision));
        target->probeValid = !fProtocolRecoveryPending;
        return target->probeValid;
    }
    if (ok) ok = probeStableIdentity();
    MPS_DEBUG_PROP("MPSScsiProbeState", ok ? "ok" : "failed");
    return ok;
}

bool
MPSController::checkTargetIdentity(void)
{
    /* Identity comparison is per target: probeTarget() compares the freshly
     * probed identity, capacity and block size with the slot that owns this
     * device and advances that slot's media generation when they differ. */
    if (!fTargetIdentityValid || fCapacityBlocks == 0 || fBlockSize == 0 ||
        fCapacityBlocks > UINT64_MAX / fBlockSize) {
        MPS_DEBUG_PROP("MPSIdentityState", "unverified");
        return false;
    }
    MPS_DEBUG_PROP("MPSIdentityState", "verified");
    return true;
}

bool
MPSController::probeTargetCapabilities(void)
{
    UInt8 limits[256], provisioning[256];
    UInt32 limitBytes = 0, provisioningBytes = 0;
    fUnmapEnabled = false;
    fUnmapLimits = MPSUnmapLimits{};
    if (scsiInquiryVpd(0xB0, limits, sizeof(limits), &limitBytes) &&
        scsiInquiryVpd(0xB2, provisioning, sizeof(provisioning), &provisioningBytes)) {
        fUnmapEnabled = mpsParseUnmapLimits(limits, limitBytes, provisioning,
                                           provisioningBytes, &fUnmapLimits);
    }
    MPSTargetSlot *capabilityTarget = findTarget(currentTargetKey());
    fNativeFuaSupported = (fTargetDeviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE) != 0 &&
        (fTargetDeviceFlags & MPI2_SAS_DEVICE0_FLAGS_SATA_FUA_SUPPORTED) != 0;
    if ((fTargetDeviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE) == 0 &&
        !fProtocolRecoveryPending) {
        UInt8 cdb[10] = {0x5A, 0x08, 0x08, 0, 0, 0, 0, 0, 64, 0};
        IOReturn status = kIOReturnIOError;
        UInt32 actual = 0;
        if (issueScsiCommand(cdb, sizeof(cdb), MPI2_SCSIIO_CONTROL_READ,
                             NULL, 64, 8, &status, &actual, true) && actual >= 8) {
            const UInt8 *mode = (const UInt8 *)fDataBuffer->getBytesNoCopy();
            fNativeFuaSupported = (mode[3] & 0x10) != 0;
        }
    }
    if (capabilityTarget && capabilityTarget->kind == kMPSTargetSas &&
        (capabilityTarget->deviceInfo & MPI2_SAS_DEVICE_INFO_SATA_DEVICE)) {
        UInt8 characteristics[64]; UInt32 actual = 0;
        capabilityTarget->solidState = capabilityTarget->rotationKnown = false;
        if (scsiInquiryVpd(0xB1, characteristics, sizeof(characteristics), &actual) && actual >= 6) {
            const UInt16 rate = mpsReadBe16(characteristics + 4);
            capabilityTarget->solidState = rate == 1;
            capabilityTarget->rotationKnown = rate == 1 || (rate >= 0x401 && rate <= 0xFFFE);
        }
    }
    MPS_DEBUG_PROP("MPSNativeFuaSupported", fNativeFuaSupported);
    MPS_DEBUG_PROP("MPSFuaPolicy", fNativeFuaSupported ? "native" : "ordered-cache-flush");
    MPS_DEBUG_PROP("MPSUnmapEnabled", fUnmapEnabled);
    MPS_DEBUG_PROP("MPSUnmapMaxLbaCount", (unsigned long long)fUnmapLimits.maxBlocks, 32);
    MPS_DEBUG_PROP("MPSUnmapMaxDescriptors", (unsigned long long)fUnmapLimits.maxDescriptors, 32);
    MPS_DEBUG_PROP("MPSUnmapOptimalGranularity", (unsigned long long)fUnmapLimits.optimalGranularity, 32);
    MPS_DEBUG_PROP("MPSUnmapAlignment", (unsigned long long)fUnmapLimits.alignment, 32);
    MPS_DEBUG_PROP("MPSUnmapAlignmentValid", fUnmapLimits.alignmentValid);
    MPS_DEBUG_PROP("MPSUnmapReadsZero", fUnmapLimits.readsZero);
    return !fProtocolRecoveryPending;
}

bool
MPSController::prepareTargetMedia(void)
{
    if (fAttachedDevHandle == 0 || fRequestBuffer == NULL || fProtocolRecoveryPending ||
        !allocateResidentBuffers() || !checkTargetIdentity() || !probeTargetCapabilities()) {
        MPS_DEBUG_PROP("MPSMediaReady", false);
        return false;
    }
    UInt32 blocks = fCapacityBlocks < 8 ? (UInt32)fCapacityBlocks : 8;
    if (blocks > fDataBytes / fBlockSize) blocks = fDataBytes / fBlockSize;
    /* One read per probe pass. A device that is still spinning up answers
     * NOT READY or BUSY, and the hotplug worker owns the single per-target
     * retry budget at its one-second cooldown. Retrying here too would create
     * a second budget for the same target and sleep under fIOLock. */
    (void)scsiTur();
    IOReturn status = kIOReturnIOError;
    bool ready = blocks != 0 &&
        scsiReadWrite(false, 0, blocks, fBlockSize, 9, false, false, &status);
    MPS_DEBUG_PROP("MPSMediaReadyStatus", (unsigned long long)(UInt32)status, 32);
    ready = ready && !fProtocolRecoveryPending && fPageBuffer != NULL &&
            fDataBuffer != NULL && fDataBytes != 0;
    MPS_DEBUG_PROP("MPSMediaReady", ready);
    return ready;
}

IOReturn
MPSController::scsiSynchronizeCache(UInt64 lba, UInt64 blocks, UInt32 smid, bool barrier)
{
    UInt8 cdb[16];
    UInt32 length = mpsBuildSynchronizeCdb(cdb, lba, blocks);
    if (length == 0) return kIOReturnBadArgument;
    IOReturn status = kIOReturnNotReady;
    if (!issueScsiCommand(cdb, length, MPI2_SCSIIO_CONTROL_NODATATRANSFER,
                          NULL, 0, smid, &status)) return status;
    MPS_DEBUG_PROP("MPSSyncBarrier", barrier);
    return kIOReturnSuccess;
}

IOReturn
MPSController::scsiUnmap(const IOBlockStorageDeviceExtent *extents,
                          UInt32 extentsCount, UInt32 smid)
{
    if (extents == NULL || extentsCount == 0 || extentsCount > kMPSUnmapBatchDescriptors ||
        extentsCount > fUnmapLimits.maxDescriptors) return kIOReturnBadArgument;
    UInt8 cdb[10] = {}, params[8 + 16 * kMPSUnmapBatchDescriptors] = {};
    UInt32 dataLen = 8 + 16 * extentsCount;
    UInt64 totalBlocks = 0;
    cdb[0] = 0x42;
    mpsWriteBe16(cdb + 7, (UInt16)dataLen);
    mpsWriteBe16(params, (UInt16)(dataLen - 2));
    mpsWriteBe16(params + 2, (UInt16)(dataLen - 8));
    for (UInt32 i = 0; i < extentsCount; i++) {
        UInt64 count = extents[i].blockCount;
        if (count == 0 || count > UINT32_MAX ||
            !mpsValidBlockExtent(extents[i].blockStart, count, fCapacityBlocks) ||
            count > fUnmapLimits.maxBlocks - totalBlocks) return kIOReturnBadArgument;
        totalBlocks += count;
        mpsWriteBe64(params + 8 + 16 * i, extents[i].blockStart);
        mpsWriteBe32(params + 16 + 16 * i, (UInt32)count);
    }
    IOReturn status = kIOReturnNotReady;
    bool ok = issueScsiCommand(cdb,sizeof(cdb),MPI2_SCSIIO_CONTROL_WRITE,
                               params,dataLen,smid,&status);
    if (!ok) {
        fUnmapEnabled = false;
        MPS_DEBUG_PROP("MPSUnmapEnabled", false);
        MPS_DEBUG_PROP("MPSUnmapDisabled", "command-failed");
        return status;
    }
    return kIOReturnSuccess;
}

IOReturn
MPSController::synchronizeCacheNow(UInt64 block, UInt64 nblks, bool barrier,
                                   UInt32 expectedGeneration, UInt64 handle, UInt64 binding)
{
    if (fIOLock == NULL) return kIOReturnNotReady;
    IOReturn status = kIOReturnNotReady;
    IOLockLock(fIOLock);
    /* The generation and the capacity are the target's, not the controller's:
     * a flush queued for a departed disk must not be sent to its replacement. */
    MPSTargetSlot *slot = findTarget(handle);
    if (slot == NULL || !slot->present || !slot->probeValid ||
        slot->generation != expectedGeneration || !mpsBindingMatches(binding,slot->bindingId,true) || slot->offlinePending ||
        mediaRemovalPending())
        status = kIOReturnNoMedia;
    else if (!fDeviceOffline && !fStopping && !fSuspended && !fReinitInProgress &&
             !fProtocolRecoveryPending) {
        if (block >= slot->capacityBlocks ||
            (nblks != 0 && nblks > slot->capacityBlocks - block))
            status = kIOReturnBadArgument;
        else if (!selectTarget(handle)) status = kIOReturnNoMedia;
        else status = scsiSynchronizeCache(block, nblks, 29, barrier);
        if (slot->generation != expectedGeneration || !slot->present ||
            slot->offlinePending) status = kIOReturnNoMedia;
    }
    IOLockUnlock(fIOLock);
    return status;
}

IOReturn
MPSController::unmapBlocksNow(const IOBlockStorageDeviceExtent *extents, UInt32 extentsCount,
                              UInt32 expectedGeneration, UInt64 handle, UInt64 binding)
{
    if (extentsCount && extents == NULL) return kIOReturnBadArgument;
    if (fIOLock == NULL) return kIOReturnNotReady;
    IOReturn status = kIOReturnSuccess;
    IOLockLock(fIOLock);
    MPSTargetSlot *slot = findTarget(handle);
    if (slot == NULL || !slot->present || !slot->probeValid ||
        slot->generation != expectedGeneration || !mpsBindingMatches(binding,slot->bindingId,true) || slot->offlinePending ||
        mediaRemovalPending()) {
        status = kIOReturnNoMedia; goto finished;
    }
    if (fDeviceOffline || fStopping || fSuspended || fReinitInProgress ||
        fProtocolRecoveryPending || fDataBuffer == NULL) {
        status = kIOReturnNotReady; goto finished;
    }
    if (!selectTarget(handle)) { status = kIOReturnNoMedia; goto finished; }
    if (!fUnmapEnabled) { status = kIOReturnUnsupported; goto finished; }
    /* Validate every input before issuing the first destructive command. */
    for (UInt32 i = 0; i < extentsCount; i++) {
        if (!mpsValidBlockExtent(extents[i].blockStart, extents[i].blockCount,
                                 fCapacityBlocks)) {
            status = kIOReturnBadArgument; goto finished;
        }
    }
    {
        UInt32 index = 0;
        UInt64 consumed = 0;
        while (index < extentsCount) {
            IOBlockStorageDeviceExtent batch[kMPSUnmapBatchDescriptors];
            UInt32 count = 0;
            UInt64 budget = fUnmapLimits.maxBlocks;
            while (index < extentsCount && count < kMPSUnmapBatchDescriptors &&
                   count < fUnmapLimits.maxDescriptors && budget != 0) {
                UInt64 left = extents[index].blockCount - consumed;
                if (left == 0) { index++; consumed = 0; continue; }
                UInt64 take = left < budget ? left : budget;
                if (take > UINT32_MAX) take = UINT32_MAX;
                batch[count].blockStart = extents[index].blockStart + consumed;
                batch[count].blockCount = take;
                count++;
                consumed += take;
                budget -= take;
                if (consumed == extents[index].blockCount) { index++; consumed = 0; }
            }
            if (count == 0) break;
            if (slot->generation != expectedGeneration || !slot->present ||
                slot->offlinePending || mediaRemovalPending()) {
                status = kIOReturnNoMedia; break;
            }
            status = scsiUnmap(batch, count, 30);
            if (status != kIOReturnSuccess) break;
        }
    }
    if (slot->generation != expectedGeneration || !slot->present ||
        slot->offlinePending || mediaRemovalPending())
        status = kIOReturnNoMedia;
finished:
    IOLockUnlock(fIOLock);
    return status;
}

/* ------------------------------------------------------------------------ */
/* per-target table, publication, invalidation and removal.                  */

MPSTargetSlot *
MPSController::allocTargetSlot(void)
{
    /* Prefer a slot that never had a device. A slot whose device is gone is
     * reusable only once its nub has been told the medium is offline, so the
     * storage layer never sees two devices under one medium without a
     * transition. */
    for (UInt32 i = 0; i < kMPSMaxTargets; i++)
        if (fTargets[i].handle == 0 && fTargets[i].nub == NULL) return &fTargets[i];
    return NULL;
}

bool
MPSController::selectTarget(UInt64 handle)
{
    /* The management and synchronous-command chain is single-threaded under
     * fIOLock and therefore bound to one handle at a time. Selecting a target
     * copies its policy into the controller fields those helpers read; it does
     * not change the target table. */
    MPSTargetSlot *slot = findTarget(handle);
    if (slot == NULL || !slot->present || !slot->probeValid) return false;
    fAttachedDevHandle = slot->handle;
    fAttachedLun = slot->lun;
    fCapacityBlocks = slot->capacityBlocks;
    fBlockSize = slot->blockSize;
    fTargetDeviceInfo = slot->deviceInfo;
    fTargetDeviceFlags = slot->flags;
    fTargetSasAddress = slot->sasAddress;
    fUnmapEnabled = slot->unmapEnabled;
    fUnmapLimits = slot->unmapLimits;
    fNativeFuaSupported = slot->nativeFua;
    fTargetIdentityValid = slot->identityValid;
    strlcpy(fTargetIdentity, slot->identity, sizeof(fTargetIdentity));
    strlcpy(fTargetModel, slot->model, sizeof(fTargetModel));
    strlcpy(fTargetSerial, slot->serial, sizeof(fTargetSerial));
    memcpy(fVendor, slot->vendor, sizeof(fVendor));
    memcpy(fProduct, slot->product, sizeof(fProduct));
    memcpy(fRevision, slot->revision, sizeof(fRevision));
    return true;
}

void
MPSController::publishTargetProperties(void)
{
    for (UInt32 i = 0; i < kMPSMaxTargets; i++) {
        MPSTargetSlot *slot = &fTargets[i];
        if (slot->handle == 0 && slot->nub == NULL) continue;
        char name[48], address[24];
        snprintf(name, sizeof(name), "MPSTarget%uPathCount", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->pathCount, 32);
        snprintf(name, sizeof(name), "MPSTarget%uLUN", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->lun, 32);
        snprintf(name, sizeof(name), "MPSTarget%uProtectionType", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->protectionType, 8);
        snprintf(name, sizeof(name), "MPSTarget%uTLR", (unsigned)i);
        MPS_DEBUG_PROP(name, slot->tlrEnabled);
        snprintf(name, sizeof(name), "MPSTarget%uManagementOnly", (unsigned)i);
        MPS_DEBUG_PROP(name, slot->managementOnly);
        snprintf(name, sizeof(name), "MPSTarget%uKind", (unsigned)i);
        MPS_DEBUG_PROP(name, slot->kind == kMPSTargetRaidVolume ? "RAID-volume" :
                            "SAS-SATA");
        snprintf(name, sizeof(name), "MPSTarget%uMDTS", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->maxTransferBytes, 32);
        snprintf(name, sizeof(name), "MPSTarget%uResetTimeoutSeconds", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->resetTimeoutSeconds, 8);
        const char *removal = slot->removalState[0] != 0 ? slot->removalState :
                              slot->removalRequested ? "requested" :
                              slot->present ? "live" : "gone";
        snprintf(name, sizeof(name), "MPSTarget%uHandle", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->handle, 16);
        snprintf(name, sizeof(name), "MPSTarget%uDeviceInfo", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->deviceInfo, 32);
        snprintf(name, sizeof(name), "MPSTarget%uSasAddress", (unsigned)i);
        snprintf(address, sizeof(address), "%016llx",
                 (unsigned long long)slot->sasAddress);
        MPS_DEBUG_PROP(name, address);
        snprintf(name, sizeof(name), "MPSTarget%uPresent", (unsigned)i);
        MPS_DEBUG_PROP(name, slot->present);
        snprintf(name, sizeof(name), "MPSTarget%uProbed", (unsigned)i);
        MPS_DEBUG_PROP(name, slot->probeValid);
        snprintf(name, sizeof(name), "MPSTarget%uProbeState", (unsigned)i);
        MPS_DEBUG_PROP(name, slot->probeState == kMPSProbeOk ? "ok" :
                          slot->probeState == kMPSProbeNotReady ?
                          "not-ready" : "failed");
        snprintf(name, sizeof(name), "MPSTarget%uProbeRetries", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->probeRetries, 32);
        snprintf(name, sizeof(name), "MPSTarget%uOnline", (unsigned)i);
        MPS_DEBUG_PROP(name, slot->online);
        snprintf(name, sizeof(name), "MPSTarget%uBlocks", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->capacityBlocks, 64);
        snprintf(name, sizeof(name), "MPSTarget%uBlockSize", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->blockSize, 32);
        snprintf(name, sizeof(name), "MPSTarget%uIdentity", (unsigned)i);
        MPS_DEBUG_PROP(name, slot->identityValid ? slot->identity : "unknown");
        snprintf(name, sizeof(name), "MPSTarget%uModel", (unsigned)i);
        MPS_DEBUG_PROP(name, slot->model[0] != 0 ? slot->model : "unknown");
        snprintf(name, sizeof(name), "MPSTarget%uSerial", (unsigned)i);
        MPS_DEBUG_PROP(name, slot->serial[0] != 0 ? slot->serial : "unknown");
        snprintf(name, sizeof(name), "MPSTarget%uRemovalState", (unsigned)i);
        MPS_DEBUG_PROP(name, removal);
        snprintf(name, sizeof(name), "MPSTarget%uGeneration", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->generation, 32);
        snprintf(name, sizeof(name), "MPSTarget%uFailureCount", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->failureCount, 32);
        snprintf(name, sizeof(name), "MPSTarget%uWriteCount", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->writeCount, 32);
    }
}

void
MPSController::publishTargetCounters(void)
{
    MPS_DEBUG_PROP("MPSScanCount", (unsigned long long)fScanSerial, 32);
    MPS_DEBUG_PROP("MPSRemovedMediaCount", (unsigned long long)fRemovedMediaCount, 32);
    MPS_DEBUG_PROP("MPSRemovalFailures", (unsigned long long)fRemovalFailures, 32);
    MPS_DEBUG_PROP("MPSFaultRecoveries", (unsigned long long)fFaultRecoveries, 32);
    MPS_DEBUG_PROP("MPSRediscoveryResets", (unsigned long long)fRediscoveryResets, 32);
    /* Called from the watchdog: the counters move without a topology event, so
     * they are republished periodically instead of only on publication. */
    UInt32 probeRetries = 0;
    for (UInt32 i = 0; i < kMPSMaxTargets; i++)
        probeRetries += fTargets[i].probeRetries;
    MPS_DEBUG_PROP("MPSProbeRetries", (unsigned long long)probeRetries, 32);
    for (UInt32 i = 0; i < kMPSMaxTargets; i++) {
        MPSTargetSlot *slot = &fTargets[i];
        if (slot->handle == 0 && slot->nub == NULL) continue;
        char name[48];
        snprintf(name, sizeof(name), "MPSTarget%uWriteCount", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->writeCount, 32);
        snprintf(name, sizeof(name), "MPSTarget%uFailureCount", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->failureCount, 32);
        snprintf(name, sizeof(name), "MPSTarget%uProbeState", (unsigned)i);
        MPS_DEBUG_PROP(name, slot->probeState == kMPSProbeOk ? "ok" :
                          slot->probeState == kMPSProbeNotReady ?
                          "not-ready" : "failed");
        snprintf(name, sizeof(name), "MPSTarget%uProbeRetries", (unsigned)i);
        MPS_DEBUG_PROP(name, (unsigned long long)slot->probeRetries, 32);
        snprintf(name, sizeof(name), "MPSTarget%uOnline", (unsigned)i);
        MPS_DEBUG_PROP(name, slot->online);
    }
}

void
MPSController::setTargetRemovalState(MPSTargetSlot *slot, const char *state)
{
    if (slot == NULL || state == NULL) return;
    strlcpy(slot->removalState, state, sizeof(slot->removalState));
}

void
MPSController::requestTargetRemoval(MPSTargetSlot *slot)
{
    if (slot == NULL || slot->handle == 0 || slot->removalRequested) return;
    slot->removalRequested = true;
    setTargetRemovalState(slot, "requested");
    publishTargetProperties();
}

void
MPSController::invalidateTarget(MPSTargetSlot *slot)
{
    if (slot == NULL) return;
    slot->probeValid = false;
    slot->generation = nextTargetGeneration();
    slot->onlinePending = false;
    if (slot->nub != NULL) {
        slot->online = false;
        slot->offlinePending = true;
    }
}

void
MPSController::invalidateTargets(void)
{
    /* A controller-wide event (IOC reset, protocol fault) unbinds every
     * medium. Transport failure and publication state are separate: setting
     * DeviceOffline must never suppress the later OFFLINE notification. */
    for (UInt32 i = 0; i < kMPSMaxTargets; i++) invalidateTarget(&fTargets[i]);
    fMediaPresent = false;
    fDeviceOffline = true;
    MPS_DEBUG_PROP("MPSDeviceOffline", true);
    MPS_DEBUG_PROP("MPSMediaPresent", false);
    MPS_DEBUG_PROP("MPSMediaReady", false);
    publishTargetProperties();
}

UInt32
MPSController::liveTargetCount(void)
{
    UInt32 live = 0;
    for (UInt32 i = 0; i < kMPSMaxTargets; i++) {
        MPSTargetSlot *slot = &fTargets[i];
        if (slot->handle != 0 && slot->present && slot->probeValid &&
            slot->nub != NULL) live++;
    }
    return live;
}

MPSProbeResult
MPSController::probeTarget(MPSTargetSlot *slot)
{
    if (slot == NULL || slot->handle == 0 || !slot->present)
        return kMPSProbeFailed;
    /* The shared probe chain is read-only (TUR, INQUIRY, VPD, READ CAPACITY,
     * MODE SENSE, READ) and serialised by fIOLock. Its controller-level state
     * belongs to the slot being probed and is copied back before the next
     * target uses the chain. */
    fAttachedDevHandle = slot->handle;
    fAttachedLun = slot->lun;
    fTargetIdentityValid = false;
    fTargetIdentity[0] = 0;
    fTargetModel[0] = 0;
    fTargetSerial[0] = 0;
    fCapacityBlocks = 0;
    fBlockSize = 0;
    fUnmapEnabled = false;
    fUnmapLimits = MPSUnmapLimits{};
    fNativeFuaSupported = false;
    /* The topology enumeration already captured this target's device page
     * fields. Reuse them instead of re-reading config pages while the start
     * path still owns the shared management SMID. */
    fTargetDeviceInfo = slot->deviceInfo;
    fTargetDeviceFlags = slot->flags;
    fTargetSasAddress = slot->sasAddress;
    UInt32 index = (UInt32)(slot - fTargets);
    char name[48];
    const char *stage = NULL;
    IOReturn status = kIOReturnSuccess;
    /* Each stage starts with a clean classification: an expected fallback
     * within one stage (RC16 rejected by a SATA bridge, then RC10) must not
     * decide the outcome of a later stage. */
    fProbeInProgress = true;
    beginProbeStage();
    if (!probeScsiIdentity()) {
        stage = "scsi-identity";
    } else if (slot->managementOnly) {
        fProbeInProgress = false;
        slot->probeValid = true;
        return kMPSProbeOk;
    } else {
        beginProbeStage();
        if (!probeCapacity()) {
            stage = "capacity";
        } else {
            beginProbeStage();
            if (!probeBlockRead()) {
                stage = "block-read";
            } else {
                beginProbeStage();
                if (!prepareTargetMedia()) stage = "media";
            }
        }
    }
    status = fLastProbeStatus;
    fProbeInProgress = false;
    if (stage != NULL) {
        /* Only a decoded NOT READY / UNIT ATTENTION / BUSY means the device is
         * on the fabric but still becoming ready. A chain that failed without
         * any decoded status (timeout, missing buffer, malformed reply) or
         * with a real SCSI error is a definite failure. */
        MPSProbeResult result =
            (status == kIOReturnNotReady || status == kIOReturnBusy)
                ? kMPSProbeNotReady : kMPSProbeFailed;
        snprintf(name, sizeof(name), "MPSTarget%uProbe", index);
        MPS_DEBUG_PROP(name, result == kMPSProbeNotReady ? "not-ready" : "failed");
        snprintf(name, sizeof(name), "MPSTarget%uProbeStage", index);
        MPS_DEBUG_PROP(name, stage);
        snprintf(name, sizeof(name), "MPSTarget%uProbeStatus", index);
        MPS_DEBUG_PROP(name, (unsigned long long)(UInt32)status, 32);
        return result;
    }
    bool changed = mpsPublishedIdentityChanged(slot->identityValid,
        slot->identity,fTargetIdentity,slot->capacityBlocks,fCapacityBlocks,
        slot->blockSize,fBlockSize);
    if (changed) {
        if (fBindingSerial==UINT64_MAX || !retireNubBinding(slot)) return kMPSProbeFailed;
        slot->bindingId=++fBindingSerial;
    }
    slot->capacityBlocks = fCapacityBlocks;
    slot->blockSize = fBlockSize;
    slot->identityValid = fTargetIdentityValid;
    memcpy(slot->vendor, fVendor, sizeof(slot->vendor));
    memcpy(slot->product, fProduct, sizeof(slot->product));
    memcpy(slot->revision, fRevision, sizeof(slot->revision));
    strlcpy(slot->identity, fTargetIdentity, sizeof(slot->identity));
    /* A SATA disk reports an ATA model; an SSP disk behind an expander only
     * has its INQUIRY product string. */
    strlcpy(slot->model, fTargetModel[0] != 0 ? fTargetModel : fProduct,
            sizeof(slot->model));
    strlcpy(slot->serial, fTargetSerial, sizeof(slot->serial));
    slot->unmapEnabled = fUnmapEnabled;
    slot->unmapLimits = fUnmapLimits;
    slot->nativeFua = fNativeFuaSupported;
    if (!slot->probeValid || changed) {
        /* A republished medium gets a new generation: I/O accepted for the old
         * one must never reach this device, even when its identity matches. */
        slot->generation = nextTargetGeneration();
        slot->onlinePending = false;
        if (slot->nub != NULL) {
            if (slot->online) {
                slot->online = false;
                slot->offlinePending = true;
            }
        }
    }
    slot->probeValid = true;
    snprintf(name, sizeof(name), "MPSTarget%uProbe", index);
    MPS_DEBUG_PROP(name, "ok");
    snprintf(name, sizeof(name), "MPSTarget%uProbeStatus", index);
    MPS_DEBUG_PROP(name, 0ULL, 32);
    return kMPSProbeOk;
}

bool
MPSController::publishTarget(MPSTargetSlot *slot)
{
    if (fMaintenanceMode || slot == NULL || slot->handle == 0 || !slot->present || !slot->probeValid ||
        slot->blockSize == 0 || slot->capacityBlocks == 0 ||
        fIOLock == NULL || fPageBuffer == NULL || fDataBuffer == NULL ||
        fPagePhys == 0 || fDataPhys == 0 || fDataBytes == 0) return false;
    char location[64];
    snprintf(location, sizeof(location), "%u-%016llx-%u", (unsigned)slot->kind,
             (unsigned long long)slot->sasAddress, (unsigned)slot->lun);
    if (slot->nub != NULL) {
        slot->nub->setLocation(location);
        /* The nub stayed attached while the medium was offline. Re-point it at
         * this target's current handle and geometry, and let the unlocked
         * worker deliver OFFLINE -> ONLINE around the transition. */
        slot->nub->setController(this,slot->key(),slot->bindingId,slot->vendor,slot->product,slot->revision);
        slot->onlinePending = true;
        setTargetRemovalState(slot, "live");
        publishTargetProperties();
        return true;
    }
    MPSBlockDev *nub = new MPSBlockDev;
    if (nub == NULL) return false;
    if (!nub->init(NULL)) {
        nub->release();
        return false;
    }
    nub->setController(this,slot->key(),slot->bindingId,slot->vendor,slot->product,slot->revision);
    nub->setLocation(location);
    if (!nub->attach(this)) {
        nub->release();
        return false;
    }
    if (!nub->start(this)) {
        nub->detach(this);
        nub->release();
        return false;
    }
    slot->nub = nub;
    /* Matching an IOBlockStorageDriver against this device reads the media
     * state directly, so its first publication needs no notification. */
    slot->online = true;
    slot->offlinePending = false;
    slot->onlinePending = false;
    setTargetRemovalState(slot, "live");
    nub->registerService();
    publishTargetProperties();
    return true;
}

bool
MPSController::ensureTargetsPublished(void)
{
    UInt32 published = 0;
    bool allOk = true;
    MPS_DEBUG_PROP("MPSPublicationMode", fMaintenanceMode ? "maintenance-no-block-media" : "block-media");
    for (UInt32 i = 0; i < kMPSMaxTargets; i++) {
        MPSTargetSlot *slot = &fTargets[i];
        if (slot->handle == 0 || !slot->present || (slot->managementOnly && slot->peripheralType == 0)) continue;
        if (!slot->probeValid) {
            MPSProbeResult result = probeTarget(slot);
            recordProbeResult(slot, result);
            if (result != kMPSProbeOk) {
                /* A still-spinning disk is not a failed target; the hotplug
                 * worker owns its remaining probes. Only a definite failure
                 * counts against this device. */
                if (result == kMPSProbeFailed) slot->failureCount++;
                allOk = false;
                continue;
            }
        }
        if (slot->managementOnly || fMaintenanceMode) continue;
        if (!publishTarget(slot)) {
            slot->failureCount++;
            allOk = false;
            continue;
        }
        published++;
    }
    /* Point the controller's default handle at the first published target;
     * the legacy synchronous helpers use it. Per-request routing never
     * depends on this default. */
    UInt64 defaultHandle = 0;
    for (UInt32 i = 0; i < kMPSMaxTargets && defaultHandle == 0; i++)
        if (fTargets[i].nub != NULL && fTargets[i].present && fTargets[i].probeValid) defaultHandle = fTargets[i].key();
    fAttachedDevHandle = (UInt16)defaultHandle;
    fAttachedLun = (UInt32)(defaultHandle >> 16);
    if (defaultHandle != 0) {
        MPSTargetSlot *slot = findTarget(defaultHandle);
        if (slot != NULL) {
            fCapacityBlocks = slot->capacityBlocks;
            fBlockSize = slot->blockSize;
            MPS_DEBUG_PROP("MPSMediaBlocks", slot->capacityBlocks, 64);
            MPS_DEBUG_PROP("MPSMediaBlockSize", (unsigned long long)slot->blockSize, 32);
        }
    }
    fMediaPresent = published != 0;
    MPS_DEBUG_PROP("MPSMediaPresent", fMediaPresent);
    MPS_DEBUG_PROP("MPSPublishedTargetCount", published, 32);
    publishTargetProperties();
    return allOk && published != 0;
}

bool
MPSController::targetDevicePresent(UInt16 handle, bool *present, UInt64 *sasAddress)
{
    /* true: the firmware answered (the device is present or absent);
     * false: the page could not be read, so its state is unknown. */
    if (present != NULL) *present = false;
    if (sasAddress != NULL) *sasAddress = 0;
    MPSTargetSlot *slot = findTarget(handle);
    if (slot && slot->kind != kMPSTargetSas) {
        bool exists = false; UInt64 id = 0;
        bool known = readAdditionalTarget(slot, &exists, &id);
        if (present) *present = exists;
        if (sasAddress) *sasAddress = id;
        return known;
    }
    bool member = false;
    if (!readRaidMembership(handle, &member)) return false;
    if (slot) slot->hiddenByRaid = member;
    if (member && !(fWarpDrive && !fWarpDriveHidePhysical)) return true;
    UInt8 page[512];
    UInt32 actual = 0;
    UInt16 status = 0xFFFF;
    bool found = readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
        MPI2_SAS_DEVICE_PGAD_FORM_HANDLE | handle, page, sizeof(page), &actual, &status);
    if (!found) return status == MPI2_IOCSTATUS_CONFIG_INVALID_PAGE;
    if (actual < 0x22 || mpsReadLe16(page + 0x18) != handle) return false;
    if ((mpsReadLe16(page + 0x20) & MPI2_SAS_DEVICE0_FLAGS_DEVICE_PRESENT) == 0)
        return true;
    if (present != NULL) *present = true;
    if (sasAddress != NULL) *sasAddress = mpsReadLe64(page + 0x0C);
    return true;
}

bool
MPSController::removeTargetDevice(UInt16 handle)
{
    if (fRequestBuffer == NULL || handle == 0 || fProtocolRecoveryPending) return false;
    if (!beginManagementRequest(4)) return false;
    UInt8 *frame = (UInt8 *)fRequestBuffer->getBytesNoCopy() + 4 * fRequestFrameSize;
    memset(frame, 0, fRequestFrameSize);
    MPSSasIoUnitControlRequest *request = (MPSSasIoUnitControlRequest *)frame;
    request->Function = MPI2_FUNCTION_SAS_IO_UNIT_CONTROL;
    request->Operation = MPI2_SAS_OP_REMOVE_DEVICE;
    request->DevHandle = handle;
    postDefaultRequest(4);
    UInt8 replyBytes[64], kind = 0;
    if (!finishManagementRequest(4, 2000, replyBytes, &kind)) return false;
    const MPSSasIoUnitControlReply *reply = (const MPSSasIoUnitControlReply *)replyBytes;
    if (kind != 1 || reply->Function != MPI2_FUNCTION_SAS_IO_UNIT_CONTROL ||
        reply->MsgLength * 4U < sizeof(*reply) || reply->DevHandle != handle ||
        reply->Operation != MPI2_SAS_OP_REMOVE_DEVICE) {
        recordProtocolFault(kMPSTargetMalformedReply, 4);
        return false;
    }
    return (reply->IOCStatus & MPI2_IOCSTATUS_MASK) == MPI2_IOCSTATUS_SUCCESS;
}

/* A missing page in one transport namespace is not proof of a free handle:
 * firmware may already have reassigned it to a volume or PCIe/SAS device. */
bool
MPSController::checkOtherTargetKinds(UInt16 handle, MPSTargetKind expected, bool *occupied)
{
    *occupied = false;
    UInt8 page[1024];
    for (unsigned kind = 0; kind <= kMPSTargetRaidVolume; ++kind) {
        if (kind == (unsigned)expected ||
            (kind == kMPSTargetRaidVolume &&
             !(fIocCapabilities & MPI2_IOCFACTS_CAPABILITY_INTEGRATED_RAID))) continue;
        UInt32 bytes = 0;
        UInt16 status = 0xFFFF;
        bool read = kind == kMPSTargetRaidVolume
            ? readConfigPage(MPI2_CONFIG_PAGETYPE_RAID_VOLUME, 0, 0,
                MPI2_RAID_VOLUME_PGAD_FORM_HANDLE | handle,
                page, sizeof(page), &bytes, &status)
            : readExtConfigPage(MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE,
                MPI2_SAS_DEVICE_PGAD_FORM_HANDLE | handle,
                page,sizeof(page),&bytes,&status);
        if (!read) {
            if (status == MPI2_IOCSTATUS_CONFIG_INVALID_PAGE && !fProtocolRecoveryPending) continue;
            return false;
        }
        const UInt32 offset = kind == kMPSTargetRaidVolume ? 4 : 0x18;
        if (bytes < offset + 2 || mpsReadLe16(page + offset) != handle) return false;
        *occupied = true;
        MPS_DEBUG_LOG("MPS: handle=0x%04x changed namespace %u -> %u; no target reset/remove\n",
                      handle, (unsigned)expected, kind);
        return true;
    }
    return true;
}

bool
MPSController::retireTarget(MPSTargetSlot *slot)
{
    /* Firmware expects TARGET_RESET (which aborts the commands still owned by
     * the device) followed by SAS IO Unit Control REMOVE_DEVICE. Both name the
     * removed handle explicitly: the controller's current default may point at
     * a different, healthy target.
     *
     * Presence decides the outcome. A handle that still names this device is the
     * disk coming back; a device that has left the fabric has nothing left to
     * reset, and the medium is unbound locally even if REMOVE_DEVICE cannot be
     * sent. The only result that keeps the medium bound is "commands may still
     * be in flight", which the caller escalates to a bounded IOC reset. */
    if (slot == NULL || slot->handle == 0 || !slot->removalRequested) return true;
    const UInt16 handle = slot->handle;
    if (slot->lun != 0) {
        if (!drainTaskManagedCommands(handle, 0, true, slot->lun)) {
            setTargetRemovalState(slot, "LUN-commands-owned"); return false;
        }
        slot->removalRequested = false;
        slot->present = slot->probeValid = false;
        /* Preserve the last published identity to detect a replacement even
         * when SATA reuses its handle and synthetic address. */
        slot->online = slot->onlinePending = false;
        if (slot->nub) slot->offlinePending = true;
        slot->generation = nextTargetGeneration();
        slot->handle = 0;
        setTargetRemovalState(slot, "logical-unit-removed");
        return true;
    }
    bool present = false;
    UInt64 address = 0;
    bool known = targetDevicePresent(handle, &present, &address);
    if (known && present) {
        slot->removalRequested = false;
        if (address == slot->sasAddress) {
            slot->present = true;
            setTargetRemovalState(slot, "reinserted");
        } else {
            /* Reused by a different device: the scan already adopted it, so
             * the new target must not receive the old removal. */
            setTargetRemovalState(slot, "handle-reassigned");
        }
        publishTargetProperties();
        return true;
    }
    if (fProtocolRecoveryPending) {
        /* The controller is already being repaired; a reset cannot be issued
         * from here without a second owner of the same fault. */
        setTargetRemovalState(slot, "recovery-pending");
        publishTargetProperties();
        return false;
    }
    if (!known) {
        setTargetRemovalState(slot, "presence-unknown");
        return false;
    }
    bool otherKind = false;
    if (!checkOtherTargetKinds(handle, slot->kind, &otherKind)) {
        setTargetRemovalState(slot, "handle-namespace-unknown");
        return false;
    }
    if (otherKind) {
        if (!drainTaskManagedCommands(handle, 0, true)) return false;
        slot->removalRequested = false;
        slot->present = false;
        setTargetRemovalState(slot, "handle-namespace-reassigned");
        return true;
    }
    UInt16 iocStatus = slot->hiddenByRaid ? MPI2_IOCSTATUS_SUCCESS : 0xFFFF;
    if (!slot->hiddenByRaid) (void)sendTaskMgmt(MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET, 0, 28, handle,
                       &iocStatus);
    MPS_DEBUG_PROP("MPSTargetResetStatus", (unsigned long long)iocStatus, 16);
    if (iocStatus == 0xFFFF) {
        /* No completion at all: the firmware may still own the request frame. */
        setTargetRemovalState(slot, "reset-reply-timeout");
        publishTargetProperties();
        return false;
    }
    /* A reset that answered with a non-success status usually means the device
     * is already gone; the drain below is the evidence that matters. The TMF
     * reply consumes its own SMID but not the SCSI replies it aborted. */
    if (!drainTaskManagedCommands(handle, 0, true) || fProtocolRecoveryPending) {
        setTargetRemovalState(slot, "commands-still-owned");
        publishTargetProperties();
        return false;
    }
    known = targetDevicePresent(handle, &present, &address);
    if (known && present) {
        slot->removalRequested = false;
        if (address == slot->sasAddress) {
            slot->present = true;
            setTargetRemovalState(slot, "reinserted-during-reset");
        } else {
            setTargetRemovalState(slot, "handle-reassigned-during-reset");
        }
        publishTargetProperties();
        return true;
    }
    if (!known) {
        setTargetRemovalState(slot, "presence-unknown-after-reset");
        return false;
    }
    /* The device is gone from the fabric. A failed or unanswered REMOVE_DEVICE
     * is not a reason to keep the medium bound: the slot is detached locally,
     * and a disk that was only reported absent is rediscovered by the next
     * scan and published again. */
    if (!checkOtherTargetKinds(handle, slot->kind, &otherKind)) {
        setTargetRemovalState(slot, "handle-namespace-unknown-after-reset");
        return false;
    }
    if (otherKind) {
        slot->removalRequested = false;
        slot->present = false;
        setTargetRemovalState(slot, "handle-namespace-reassigned-after-reset");
        return true;
    }
    const bool removed = slot->kind == kMPSTargetRaidVolume || slot->hiddenByRaid ||
                         removeTargetDevice(handle);
    MPS_DEBUG_LOG("MPS: retire kind=%u handle=0x%04x firmware-member=%u removed=%u\n",
                  (unsigned)slot->kind, handle, slot->hiddenByRaid, removed);
    slot->removalRequested = false;
    slot->present = false;
    slot->probeValid = false;
    /* identityValid/identity describe historical publication, not presence. */
    slot->generation = nextTargetGeneration();
    slot->onlinePending = false;
    if (slot->nub != NULL) {
        slot->online = false;
        slot->offlinePending = true;
    }
    slot->handle = 0;   /* free the firmware address; the nub stays offline */
    setTargetRemovalState(slot, removed ? "removed" : "removed-unconfirmed");
    publishTargetProperties();
    return true;
}
