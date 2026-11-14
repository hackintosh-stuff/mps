#ifndef MPSBlockDev_h
#define MPSBlockDev_h

#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IOLocks.h>

class MPSController;

/* Transport nub for one STP/SATA or SSP target. IOStorageFamily's
 * IOBlockStorageDriver matches on device-type = Generic, then creates the
 * IOMedia object. Every request is relayed to MPSController with this nub's
 * DevHandle; the nub owns no hardware state of its own, and its identity and
 * geometry are validated by an immutable allocation-unique binding. */
class MPSBlockDev : public IOBlockStorageDevice
{
    OSDeclareDefaultStructors(MPSBlockDev)

private:
    MPSController *fController;
    IOLock        *fControllerLock;
    char           fVendor[16];
    char           fProduct[20];
    char           fRevision[8];
    MPSController *copyController(UInt64 *binding);
    void refreshIdentity(void);
    bool           fLastMediaPresent;
    /* Each nub belongs to one target; all published targets are writable. */
    UInt64         fDevHandle;
    UInt64         fBindingId;

public:
    virtual bool init(OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    virtual void free(void) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

    void setController(MPSController *controller, UInt64 handle = 0,
        UInt64 binding = 0, const char *vendor = NULL,
        const char *product = NULL, const char *revision = NULL);
    IOReturn notifyMediaState(UInt32 state);

    virtual IOReturn doEjectMedia(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn doFormatMedia(UInt64 byteCapacity) APPLE_KEXT_OVERRIDE;
    virtual UInt32 doGetFormatCapacities(UInt64 *capacities,
                                         UInt32 capacitiesMaxCount) const APPLE_KEXT_OVERRIDE;
    virtual char *getVendorString(void) APPLE_KEXT_OVERRIDE;
    virtual char *getProductString(void) APPLE_KEXT_OVERRIDE;
    virtual char *getRevisionString(void) APPLE_KEXT_OVERRIDE;
    virtual char *getAdditionalDeviceInfoString(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn reportBlockSize(UInt64 *blockSize) APPLE_KEXT_OVERRIDE;
    virtual IOReturn reportEjectability(bool *isEjectable) APPLE_KEXT_OVERRIDE;
    virtual IOReturn reportMaxValidBlock(UInt64 *maxBlock) APPLE_KEXT_OVERRIDE;
    virtual IOReturn reportMediaState(bool *mediaPresent,
                                      bool *changedState = 0) APPLE_KEXT_OVERRIDE;
    virtual IOReturn reportRemovability(bool *isRemovable) APPLE_KEXT_OVERRIDE;
    virtual IOReturn reportWriteProtection(bool *isWriteProtected) APPLE_KEXT_OVERRIDE;
    virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer,
                                      UInt64 block, UInt64 nblks,
                                      IOStorageAttributes *attributes,
                                      IOStorageCompletion *completion) APPLE_KEXT_OVERRIDE;
    virtual IOReturn doSynchronizeCache(void) APPLE_KEXT_OVERRIDE;
    virtual IOReturn doSynchronize(UInt64 block, UInt64 nblks,
                                   IOStorageSynchronizeOptions options = 0) APPLE_KEXT_OVERRIDE;
    virtual IOReturn doUnmap(IOBlockStorageDeviceExtent *extents,
                             UInt32 extentsCount,
                             IOStorageUnmapOptions options = 0) APPLE_KEXT_OVERRIDE;
};

#endif /* MPSBlockDev_h */
