#ifndef MPS_RESIDENT_DMA_H
#define MPS_RESIDENT_DMA_H

#include <IOKit/IODMACommand.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMapper.h>
#include <stdint.h>

/* Long-lived IOC-visible memory. This is an IOVA, never an unqualified host
 * physical address. Each record remains prepared for exactly the lifetime of
 * its original descriptor. The caller may detach only before posting, after
 * its last checked completion/release, or after proving the IOC stopped DMA.
 *
 * IODMACommand.h documents getMemoryDescriptor() as the current descriptor and
 * getIOMemoryDescriptor() as "the memory descriptor to be used for DMA". A
 * different DMA descriptor is a copy/bounce mapping. Reject it: the request,
 * reply and free/post rings are concurrently shared with firmware, so a later
 * synchronize() cannot make such a mapping coherent.
 */
struct MPSResidentDma {
    IODMACommand *command;
    IOMemoryDescriptor *memory;
    UInt64 address;
    UInt32 bytes;
    bool memoryPrepared;
};

static inline void mpsReleaseResidentDma(MPSResidentDma &mapping)
{
    if (mapping.command) {
        (void)mapping.command->complete(true, false);
        (void)mapping.command->clearMemoryDescriptor(false);
        mapping.command->release();
    }
    if (mapping.memory && mapping.memoryPrepared)
        (void)mapping.memory->complete();
    mapping.command = NULL;
    mapping.memory = NULL;
    mapping.address = 0;
    mapping.bytes = 0;
    mapping.memoryPrepared = false;
}

/* Failure never falls back to a physical address. A failed candidate has not
 * been posted, so all partial mapping resources can be unwound immediately.
 * Do not call this to replace an existing live mapping. */
static inline bool mpsMapResidentDma(MPSResidentDma &mapping,
    IOMemoryDescriptor *memory, IOMapper *mapper, UInt32 bytes, UInt32 alignment)
{
    if (mapping.command)
        return mapping.memory == memory && mapping.bytes == bytes;
    if (!memory || !bytes || !alignment || (alignment & (alignment - 1)) ||
        memory->getLength() < bytes) return false;
    mapping.memory = memory;
    mapping.bytes = bytes;
    mapping.memoryPrepared = memory->prepare() == kIOReturnSuccess;
    if (!mapping.memoryPrepared) {
        mpsReleaseResidentDma(mapping);
        return false;
    }
    /* The original allocation explicitly uses kIOMemoryMapperNone so its
     * physical mask constrains host allocation rather than a default IOVA.
     * This does not prevent the IODMACommand's separate per-device mapping.
     * The original allocation is low32/physically contiguous/page-aligned.
     * This physical query validates only that allocation invariant. Only the
     * subsequent mapped IOVA is returned to the controller. The invariant
     * also excludes per-page address/alignment bounce in IODMACommand. */
    IOByteCount hostBytes = 0;
    UInt64 hostAddress = memory->getPhysicalSegment(0, &hostBytes, kIOMemoryMapperNone);
    if (!hostAddress || (hostAddress & 4095) || hostBytes < bytes ||
        hostAddress > UINT32_MAX || bytes - 1 > UINT32_MAX - hostAddress) {
        mpsReleaseResidentDma(mapping);
        return false;
    }
    mapping.command = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 32, bytes, IODMACommand::kMapped,
        bytes, 4096, mapper);
    if (!mapping.command ||
        mapping.command->setMemoryDescriptor(memory, false) != kIOReturnSuccess ||
        mapping.command->prepare(0, bytes, true, false) != kIOReturnSuccess ||
        mapping.command->getIOMemoryDescriptor() != memory) {
        mpsReleaseResidentDma(mapping);
        return false;
    }
    IODMACommand::Segment64 segment;
    UInt64 offset = 0;
    UInt32 count = 1;
    if (mapping.command->gen64IOVMSegments(&offset, &segment, &count) != kIOReturnSuccess ||
        count != 1 || offset != bytes || segment.fLength != bytes ||
        !segment.fIOVMAddr || (segment.fIOVMAddr & 4095) ||
        (segment.fIOVMAddr & (alignment - 1)) || segment.fIOVMAddr > UINT32_MAX ||
        bytes - 1 > UINT32_MAX - segment.fIOVMAddr) {
        mpsReleaseResidentDma(mapping);
        return false;
    }
    mapping.address = segment.fIOVMAddr;
    return true;
}

#endif
