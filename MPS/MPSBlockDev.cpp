#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include "MPSBlockDev.h"
#include "MPSController.h"

#define super IOBlockStorageDevice
OSDefineMetaClassAndStructors(MPSBlockDev, IOBlockStorageDevice)

static char kMPSDeviceInfo[] = "MPS Fusion-MPT logical unit";

bool
MPSBlockDev::init(OSDictionary *properties)
{
    fController = NULL;
    fControllerLock = NULL;
    fLastMediaPresent = false;
    fDevHandle = 0;
    fBindingId = 0;
    bzero(fVendor, sizeof(fVendor));
    bzero(fProduct, sizeof(fProduct));
    bzero(fRevision, sizeof(fRevision));
    if (!super::init(properties)) return false;
    fControllerLock = IOLockAlloc();
    return fControllerLock != NULL;
}

void
MPSBlockDev::free(void)
{
    setController(NULL);
    if (fControllerLock != NULL) {
        IOLockFree(fControllerLock);
        fControllerLock = NULL;
    }
    super::free();
}

MPSController *
MPSBlockDev::copyController(UInt64 *binding)
{
    if (fControllerLock == NULL) return NULL;
    IOLockLock(fControllerLock);
    MPSController *controller = fController;
    *binding = fBindingId;
    if (controller != NULL) controller->retain();
    IOLockUnlock(fControllerLock);
    return controller;
}

void
MPSBlockDev::refreshIdentity(void)
{
    /* Identity strings are captured only at the creation of this binding.
     * Never query a current firmware handle while holding the nub lock. */
}

IOReturn
MPSBlockDev::notifyMediaState(UInt32 state)
{
    if (state == kIOMediaStateOnline) refreshIdentity();
    /* Deliver to the matched storage driver and preserve its actual result.
     * messageClients() does not expose each recipient's failure. In
     * particular, ONLINE is rejected while the old IOMedia still exists.
     * The controller must call this with no transport lock held. */
    IOBlockStorageDriver *driver = NULL;
    OSIterator *iter = getClientIterator();
    if (iter != NULL) {
        OSObject *object;
        while ((object = iter->getNextObject()) != NULL) {
            driver = OSDynamicCast(IOBlockStorageDriver, object);
            if (driver != NULL) {
                driver->retain();
                break;
            }
        }
        iter->release();
    }
    if (driver == NULL) {
        return kIOReturnNotAttached;
    }
    /* Initial matching may have accepted the new media before a deferred
     * ONLINE notification. OFFLINE is always delivered explicitly. */
    if (state == kIOMediaStateOnline &&
        driver->getMediaState() == kIOMediaStateOnline) {
        driver->release();
        return kIOReturnSuccess;
    }
    IOReturn result = messageClient(kIOMessageMediaStateHasChanged, driver,
                                    (void *)(uintptr_t)state);
    driver->release();
    return result;
}

bool
MPSBlockDev::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }

    /* IOBlockStorageDriver matches on this property; set it explicitly as
     * well as through IOBlockStorageDevice::init so the nub cannot lose it
     * when a future SDK changes the base implementation. */
    MPS_DEBUG_PROP(kIOBlockStorageDeviceTypeKey, kIOBlockStorageDeviceTypeGeneric);
    MPS_DEBUG_PROP("Physical Interconnect", "SAS");
    MPS_DEBUG_PROP("MPSTransport", "MPSController");
    return true;
}

void
MPSBlockDev::stop(IOService *provider)
{
    setController(NULL);
    super::stop(provider);
}

void
MPSBlockDev::setController(MPSController *controller, UInt64 handle,
    UInt64 binding, const char *vendor, const char *product, const char *revision)
{
    if (fControllerLock == NULL) return;
    if (controller != NULL) controller->retain();
    IOLockLock(fControllerLock);
    MPSController *previous = fController;
    /* Rebinding may update the route of the same identity, never its token.
     * The controller retires a nub before allocating a replacement identity. */
    if (controller != NULL && fBindingId != 0 && fBindingId != binding) {
        IOLockUnlock(fControllerLock);
        controller->release();
        return;
    }
    fController = controller;
    if (controller != NULL) {
        fDevHandle = handle;
        if (!fBindingId) {
            fBindingId = binding;
            if (vendor) strlcpy(fVendor,vendor,sizeof(fVendor));
            if (product) strlcpy(fProduct,product,sizeof(fProduct));
            if (revision) strlcpy(fRevision,revision,sizeof(fRevision));
        }
    }
    IOLockUnlock(fControllerLock);
    if (previous != NULL) previous->release();
    if (controller != NULL) {
        refreshIdentity();
        MPS_DEBUG_PROP("MPSBlockDevHandle", (unsigned long long)(UInt16)fDevHandle, 16);
        MPS_DEBUG_PROP("MPSBlockDevLUN", (unsigned long long)(fDevHandle >> 16), 32);
    }
}

IOReturn
MPSBlockDev::doEjectMedia(void)
{
    return kIOReturnUnsupported;
}

IOReturn
MPSBlockDev::doFormatMedia(UInt64 byteCapacity)
{
    return kIOReturnUnsupported;
}

UInt32
MPSBlockDev::doGetFormatCapacities(UInt64 *capacities,
                                  UInt32 capacitiesMaxCount) const
{
    return 0;
}

char *
MPSBlockDev::getVendorString(void)
{
    return fVendor;
}

char *
MPSBlockDev::getProductString(void)
{
    return fProduct;
}

char *
MPSBlockDev::getRevisionString(void)
{
    return fRevision;
}

char *
MPSBlockDev::getAdditionalDeviceInfoString(void)
{
    return kMPSDeviceInfo;
}

IOReturn
MPSBlockDev::reportBlockSize(UInt64 *blockSize)
{
    UInt64 size;

    if (blockSize == NULL) return kIOReturnBadArgument;
    UInt64 binding = 0;
    MPSController *controller = copyController(&binding);
    if (controller == NULL) return kIOReturnNoMedia;
    UInt64 ignored = 0;
    bool valid = controller->copyBoundMedia(binding,&ignored,&size);
    controller->release();
    if (!valid || size == 0) {
        return kIOReturnNoMedia;
    }
    *blockSize = size;
    return kIOReturnSuccess;
}

IOReturn
MPSBlockDev::reportEjectability(bool *isEjectable)
{
    if (isEjectable == NULL) {
        return kIOReturnBadArgument;
    }
    *isEjectable = false;
    return kIOReturnSuccess;
}

IOReturn
MPSBlockDev::reportMaxValidBlock(UInt64 *maxBlock)
{
    UInt64 blocks;

    if (maxBlock == NULL) return kIOReturnBadArgument;
    UInt64 binding = 0;
    MPSController *controller = copyController(&binding);
    if (controller == NULL) return kIOReturnNoMedia;
    UInt64 ignored = 0;
    bool valid = controller->copyBoundMedia(binding,&blocks,&ignored);
    controller->release();
    if (!valid || blocks == 0) {
        return kIOReturnNoMedia;
    }
    *maxBlock = blocks - 1;
    return kIOReturnSuccess;
}

IOReturn
MPSBlockDev::reportMediaState(bool *mediaPresent, bool *changedState)
{
    if (mediaPresent == NULL) {
        return kIOReturnBadArgument;
    }
    UInt64 binding = 0;
    MPSController *controller = copyController(&binding);
    UInt64 ignoredBlocks = 0, ignoredSize = 0;
    bool present = controller != NULL &&
        controller->copyBoundMedia(binding,&ignoredBlocks,&ignoredSize);
    if (controller != NULL) controller->release();

    *mediaPresent = present;
    if (changedState != NULL) {
        *changedState = (present != fLastMediaPresent);
    }
    fLastMediaPresent = present;
    return kIOReturnSuccess;
}

IOReturn
MPSBlockDev::reportRemovability(bool *isRemovable)
{
    if (isRemovable == NULL) {
        return kIOReturnBadArgument;
    }
    *isRemovable = false;
    return kIOReturnSuccess;
}

IOReturn
MPSBlockDev::reportWriteProtection(bool *isWriteProtected)
{
    if (isWriteProtected == NULL) {
        return kIOReturnBadArgument;
    }
    /* IOBlockStorageDevice declares this method pure virtual, so the nub
     * must answer it. Every published target is writable; the driver keeps
     * no write-protection state. */
    *isWriteProtected = false;
    return kIOReturnSuccess;
}

IOReturn
MPSBlockDev::doAsyncReadWrite(IOMemoryDescriptor *buffer,
                              UInt64 block, UInt64 nblks,
                              IOStorageAttributes *attributes,
                              IOStorageCompletion *completion)
{
    IOReturn status;
    bool    write, fua;

    if (buffer == NULL || completion == NULL || completion->action == NULL)
        return kIOReturnBadArgument;
    if (buffer->getDirection() != kIODirectionIn &&
        buffer->getDirection() != kIODirectionOut) return kIOReturnBadArgument;
    UInt64 binding = 0;
    MPSController *controller = copyController(&binding);
    if (controller == NULL) return kIOReturnNoMedia;

    /* A descriptor with kIODirectionOut carries data from memory to the
     * device; kIODirectionIn carries data read from the device. */
    write = (buffer->getDirection() & kIODirectionOut) != 0;
    fua = (attributes != NULL &&
           (attributes->options & kIOStorageOptionForceUnitAccess) != 0);
    /* Acceptance retains the buffer and copies the completion. Commands from
     * different requests share the bounded pool; callback runs outside locks
     * in a separate context, permitting reentrant reads or cache fences. */
    status = controller->submitAsyncBlocks(write, buffer, block, nblks,
                                           fua, completion, binding);
    controller->release();
    return status;
}

IOReturn
MPSBlockDev::doSynchronizeCache(void)
{
    /* Deprecated entry point, kept for older IOStorageFamily clients. */
    UInt64 binding = 0;
    MPSController *controller = copyController(&binding);
    if (controller == NULL) return kIOReturnNoMedia;
    IOReturn status = controller->synchronizeCache(0, 0, false, binding);
    controller->release();
    return status;
}

IOReturn
MPSBlockDev::doSynchronize(UInt64 block, UInt64 nblks,
                           IOStorageSynchronizeOptions options)
{
    bool barrier = (options & kIOStorageSynchronizeOptionBarrier) != 0;

    UInt64 binding = 0;
    MPSController *controller = copyController(&binding);
    if (controller == NULL) return kIOReturnNoMedia;
    IOReturn status = controller->synchronizeCache(block, nblks, barrier, binding);
    controller->release();
    return status;
}

IOReturn
MPSBlockDev::doUnmap(IOBlockStorageDeviceExtent *extents,
                     UInt32 extentsCount, IOStorageUnmapOptions options)
{
    UInt64 binding = 0;
    MPSController *controller = copyController(&binding);
    if (controller == NULL) return kIOReturnNoMedia;
    IOReturn status = controller->unmapBlocks(extents, extentsCount, binding);
    controller->release();
    return status;
}
