#ifndef MPS_SCSI_H
#define MPS_SCSI_H

/* Small, allocation-free protocol helpers shared by the transport and the
 * host-side boundary checks. Integers in a SCSI CDB or VPD page are big endian. */
#include <stdint.h>
#include <stddef.h>

static inline uint16_t mpsReadBe16(const uint8_t *p)
{ return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static inline uint32_t mpsReadBe32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3]; }
static inline uint64_t mpsReadBe64(const uint8_t *p)
{ return ((uint64_t)mpsReadBe32(p) << 32) | mpsReadBe32(p + 4); }
static inline void mpsWriteBe16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void mpsWriteBe32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - i * 8)); }
static inline void mpsWriteBe64(uint8_t *p, uint64_t v)
{ mpsWriteBe32(p, (uint32_t)(v >> 32)); mpsWriteBe32(p + 4, (uint32_t)v); }

static inline unsigned
mpsBuildReadWriteCdb(uint8_t cdb[16], bool write, uint64_t lba,
                     uint32_t blocks, bool nativeFua)
{
    for (unsigned i = 0; i < 16; i++) cdb[i] = 0;
    if (blocks == 0 || (uint64_t)blocks - 1 > UINT64_MAX - lba) return 0;
    const bool extended = blocks > UINT16_MAX ||
                          lba + blocks - 1 > UINT32_MAX;
    cdb[0] = write ? (extended ? 0x8A : 0x2A) : (extended ? 0x88 : 0x28);
    if (write && nativeFua) cdb[1] = 0x08;
    if (extended) {
        mpsWriteBe64(cdb + 2, lba);
        mpsWriteBe32(cdb + 10, blocks);
        return 16;
    }
    mpsWriteBe32(cdb + 2, (uint32_t)lba);
    mpsWriteBe16(cdb + 7, (uint16_t)blocks);
    return 10;
}

static inline unsigned
mpsBuildSynchronizeCdb(uint8_t cdb[16], uint64_t lba, uint64_t blocks)
{
    for (unsigned i = 0; i < 16; i++) cdb[i] = 0;
    if (blocks != 0 && blocks - 1 > UINT64_MAX - lba) return 0;
    /* A whole-medium flush is a stronger, ordered implementation of a range
     * whose block count cannot be represented even by SYNCHRONIZE CACHE(16). */
    if (blocks > UINT32_MAX) { lba = 0; blocks = 0; }
    const bool extended = lba > UINT32_MAX || blocks > UINT16_MAX ||
                          (blocks != 0 && lba + blocks - 1 > UINT32_MAX);
    cdb[0] = extended ? 0x91 : 0x35;
    if (extended) {
        mpsWriteBe64(cdb + 2, lba);
        mpsWriteBe32(cdb + 10, (uint32_t)blocks);
        return 16;
    }
    mpsWriteBe32(cdb + 2, (uint32_t)lba);
    mpsWriteBe16(cdb + 7, (uint16_t)blocks);
    return 10;
}

struct MPSUnmapLimits {
    uint32_t maxBlocks;
    uint32_t maxDescriptors;
    uint32_t optimalGranularity;
    uint32_t alignment;
    bool alignmentValid;
    bool readsZero;
};

static inline bool
mpsVpdPayload(const uint8_t *page, uint32_t actual, uint8_t code,
              uint32_t minimum, uint32_t *payload)
{
    if (page == NULL || actual < 4 || page[1] != code) return false;
    uint32_t length = mpsReadBe16(page + 2);
    if (length < minimum || length > actual - 4) return false;
    if (payload != NULL) *payload = length;
    return true;
}

static inline bool
mpsParseUnmapLimits(const uint8_t *limits, uint32_t limitBytes,
                    const uint8_t *provisioning, uint32_t provisioningBytes,
                    MPSUnmapLimits *out)
{
    if (out == NULL) return false;
    *out = MPSUnmapLimits{};
    if (!mpsVpdPayload(limits, limitBytes, 0xB0, 32, NULL) ||
        !mpsVpdPayload(provisioning, provisioningBytes, 0xB2, 4, NULL) ||
        (provisioning[5] & 0x80) == 0) return false;
    out->maxBlocks = mpsReadBe32(limits + 20);
    out->maxDescriptors = mpsReadBe32(limits + 24);
    out->optimalGranularity = mpsReadBe32(limits + 28);
    uint32_t align = mpsReadBe32(limits + 32);
    out->alignmentValid = (align & 0x80000000U) != 0;
    out->alignment = align & 0x7FFFFFFFU;
    out->readsZero = (provisioning[5] & 0x04) != 0;
    return out->maxBlocks != 0 && out->maxDescriptors != 0;
}

/* The optimal granularity/alignment fields are hints, not permission to
 * enlarge a discard. Every generated descriptor stays inside its input. */
static inline bool
mpsValidBlockExtent(uint64_t start, uint64_t count, uint64_t capacity)
{
    return count == 0 || (start < capacity && count <= capacity - start);
}

static inline bool
mpsRaidConfigContains(const uint8_t *page, uint32_t actual,
                       uint16_t handle, bool *contains)
{
    if (contains == NULL) return false;
    *contains = false;
    if (page == NULL || actual < 0x30 || page[2] != 0 ||
        (page[3] & 0x0F) != 0x0F || page[6] != 0x16) return false;
    uint32_t declared = (uint32_t)(page[4] | ((uint16_t)page[5] << 8)) * 4;
    if (declared != actual) return false;
    uint32_t count = page[0x2C];
    if (count > (actual - 0x30) / 8) return false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *entry = page + 0x30 + 8 * i;
        uint16_t flags = (uint16_t)(entry[0] | ((uint16_t)entry[1] << 8));
        uint16_t volume = (uint16_t)(entry[2] | ((uint16_t)entry[3] << 8));
        uint16_t physical = (uint16_t)(entry[6] | ((uint16_t)entry[7] << 8));
        if ((flags & 0x0F) > 3) return false;
        if (handle != 0 && (volume == handle ||
            ((flags & 0x0F) != 0 && physical == handle))) *contains = true;
    }
    return true;
}

/* Same peripheral/flat/extended-flat LUN encodings as FreeBSD
 * mps_sas.h::mpssas_set_lun. The address key is private host state; only its
 * low 16 bits are a firmware handle. Never put the key in an MPI DevHandle. */
static inline uint64_t mpsTargetKey(uint16_t handle, uint32_t lun)
{ return ((uint64_t)lun << 16) | handle; }
static inline bool mpsEncodeLun(uint8_t out[8], uint32_t lun)
{
    for (unsigned i = 0; i < 8; ++i) out[i] = 0;
    if (lun <= 255) out[1] = (uint8_t)lun;
    else if (lun <= 0x3fff) { out[0] = 0x40 | (uint8_t)(lun >> 8); out[1] = (uint8_t)lun; }
    else if (lun <= 0xffffff) {
        out[0] = 0xd2; out[1] = (uint8_t)(lun >> 16);
        out[2] = (uint8_t)(lun >> 8); out[3] = (uint8_t)lun;
    } else return false;
    return true;
}
static inline bool mpsDecodeLun(const uint8_t in[8], uint32_t *lun)
{
    if (!in || !lun) return false;
    unsigned end = 2;
    if (in[0] == 0) *lun = in[1];
    else if ((in[0] & 0xc0) == 0x40) *lun = ((uint32_t)(in[0] & 0x3f) << 8) | in[1];
    else if (in[0] == 0xd2) { *lun = ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | in[3]; end = 4; }
    else return false;
    for (unsigned i = end; i < 8; ++i) if (in[i]) return false;
    return true;
}

#endif
