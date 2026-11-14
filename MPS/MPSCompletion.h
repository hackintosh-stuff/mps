#ifndef MPS_COMPLETION_H
#define MPS_COMPLETION_H

#include <stdint.h>

struct MPSSenseFields {
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
};

/* SenseCount is the number of bytes the IOC actually transferred. Never
 * trust the embedded additional-length byte to extend that DMA boundary. */
static inline bool mpsDecodeSenseFields(const uint8_t *sense,
                                        uint32_t bytes,
                                        MPSSenseFields *out)
{
    if (sense == 0 || out == 0 || bytes < 8) return false;
    out->key = 0xff;
    out->asc = out->ascq = 0;
    const uint8_t response = sense[0] & 0x7f;
    if (response == 0x70 || response == 0x71) {
        out->key = sense[2] & 0x0f;
        const uint32_t declared = 8U + (uint32_t)sense[7];
        if (bytes >= 14 && declared >= 14) {
            out->asc = sense[12];
            out->ascq = sense[13];
        }
        return true;
    }
    if (response == 0x72 || response == 0x73) {
        out->key = sense[1] & 0x0f;
        out->asc = sense[2];
        out->ascq = sense[3];
        return true;
    }
    return false;
}

#endif
