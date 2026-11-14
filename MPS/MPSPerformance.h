#ifndef MPS_PERFORMANCE_H
#define MPS_PERFORMANCE_H

#include <stdint.h>

/* Keep management/probe/synchronous I/O (1..31) isolated from the async pool.
 * Request frame, sense and reply-owner arrays all have 128 entries. */
enum {
    kMPSAsyncFirstSmid = 32,
    kMPSAsyncMaxCommands = 32,
    kMPSAsyncRequestCount = 64,
    kMPSAsyncTransferBytes = 65536,
    kMPSReplyBatchBudget = 32,
    kMPSReplyReserve = 8,
    kMPSAsyncTimeoutMs = 15000
};

static inline bool mpsIsAsyncSmid(uint32_t smid)
{
    return smid >= kMPSAsyncFirstSmid &&
           smid - kMPSAsyncFirstSmid < kMPSAsyncMaxCommands;
}

static inline uint32_t mpsConfiguredCommandDepth(int requested)
{
    if (requested <= 0) return kMPSAsyncMaxCommands;
    return (uint32_t)requested < kMPSAsyncMaxCommands ?
           (uint32_t)requested : (uint32_t)kMPSAsyncMaxCommands;
}

/* Conservatively reserve all low SMIDs/credits, plus reply capacity for
 * management, TMFs and asynchronous events. No unsigned subtraction wraps.
 * IOC Facts, allocated resources and the same limits after reset all apply. */
static inline uint32_t
mpsAsyncCommandLimit(uint32_t allocated, uint32_t requestCount,
                     uint32_t requestCredit, uint32_t postDepth,
                     uint32_t freeDepth)
{
    if (requestCount <= kMPSAsyncFirstSmid ||
        requestCredit <= kMPSAsyncFirstSmid ||
        postDepth <= kMPSReplyReserve + 1 ||
        freeDepth <= kMPSReplyReserve + 1) return 0;
    uint32_t limit = allocated < kMPSAsyncMaxCommands ?
                     allocated : (uint32_t)kMPSAsyncMaxCommands;
    const uint32_t bounds[] = {
        requestCount - kMPSAsyncFirstSmid,
        requestCredit - kMPSAsyncFirstSmid,
        postDepth - 1 - kMPSReplyReserve,
        freeDepth - 1 - kMPSReplyReserve,
        128 - kMPSAsyncFirstSmid
    };
    for (unsigned i = 0; i < sizeof(bounds) / sizeof(bounds[0]); ++i)
        if (bounds[i] < limit) limit = bounds[i];
    return limit;
}

/* After all issued chunks retire, only the prefix before the first error is
 * reportable. Out-of-order successes beyond a failed chunk are not a prefix. */
static inline uint64_t
mpsCompletedPrefix(uint64_t successfulBytes, uint64_t firstErrorOffset)
{
    return successfulBytes < firstErrorOffset ? successfulBytes : firstErrorOffset;
}

#endif
