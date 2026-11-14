#ifndef MPS_WIRE_H
#define MPS_WIRE_H
#include <stdint.h>
#include <stddef.h>
/* Validate before decoding packed payloads or accepting zero-padded handshakes.
 * available is the buffer actually retained by the caller, not an IOC length. */
static inline bool mpsReplyHeaderValid(const uint8_t *p, size_t available,
                                      uint8_t function, size_t minimum)
{
    return p && available >= 4 && minimum >= 4 && p[3] == function &&
        (size_t)p[2] * 4 >= minimum && (size_t)p[2] * 4 <= available;
}
static inline bool mpsFactsVersionValid(uint16_t version) { return version == 0x0200; }
#endif
