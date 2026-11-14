#ifndef MPS_BINDING_H
#define MPS_BINDING_H
#include <stdint.h>
#include <string.h>
/* Allocation-unique identity within one retained controller. Firmware handles,
 * SAS addresses (including SATA placeholders) and table indexes are not tokens.
 * Caller snapshots the immutable nub token before taking the transport lock. */
static inline bool mpsBindingMatches(uint64_t expected, uint64_t actual, bool live)
{ return expected != 0 && expected == actual && live; }

/* This comparison deliberately uses the last published identity even while
 * probeValid/present are false. A SATA port address can outlive its disk. */
static inline bool mpsPublishedIdentityChanged(bool historical,
    const char *oldIdentity, const char *newIdentity, uint64_t oldBlocks,
    uint64_t newBlocks, uint32_t oldSize, uint32_t newSize)
{
    return historical && (strcmp(oldIdentity,newIdentity)!=0 ||
        oldBlocks!=newBlocks || oldSize!=newSize);
}
static inline bool mpsTopologyRequiresReprobe(uint8_t reason)
{ return reason == 1 || reason == 2 || reason == 3; }
#endif
