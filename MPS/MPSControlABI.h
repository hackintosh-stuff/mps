#ifndef MPS_CONTROL_ABI_H
#define MPS_CONTROL_ABI_H
#include <stdint.h>
#include <stddef.h>
/* Native macOS control ABI. No user pointers, size_t, kernel addresses or
 * implicit host-native ioctl encodings cross this interface. Multi-byte fields
 * are little endian (the supported platform is x86_64). One structure method,
 * selector zero, carries this header, request prefix, then outgoing payload. */
enum {
    kMPSControlVersion = 1, kMPSControlConnectType = 0x4d505301,
    kMPSControlMaxRequest = 256, kMPSControlMaxReply = 256,
    kMPSControlSenseBytes = 96, kMPSControlMaxData = 16 * 1024 * 1024,
    kMPSControlIoBytes = 65536, kMPSControlMaxDiagBytes = 4 * 1024 * 1024,
    kMPSControlAllowMutation = 1, kMPSControlEventCount = 128,
    kMPSControlEventPayload = 128
};
enum MPSControlOperation {
    kMPSControlInfo = 1, kMPSControlTargets, kMPSControlEvents,
    kMPSControlEventMask, kMPSControlPassthrough, kMPSControlReset,
    kMPSControlRescan, kMPSControlReadRegister, kMPSControlWriteRegister,
    kMPSControlDiagRegister, kMPSControlDiagRelease, kMPSControlDiagQuery,
    kMPSControlDiagRead, kMPSControlDiagUnregister
};
struct MPSControlInput {
    uint32_t version, operation, flags, timeoutMs;
    uint32_t requestBytes, dataOutBytes, dataInBytes, reserved;
    uint64_t argument0, argument1;
    // epoch: adapter transport snapshot; generation: target cookie, or diagnostic uniqueId.
    // FW_DOWNLOAD uses argument1 as its nonzero segmented-transfer token.
    uint32_t epoch, generation, reserved2[2];
};
struct MPSControlOutput {
    uint32_t version, operation, status, replyBytes;
    uint32_t senseBytes, dataBytes, epoch, flags;
    uint64_t nextSequence;
    uint32_t reserved[6];
    uint8_t reply[kMPSControlMaxReply];
    uint8_t sense[kMPSControlSenseBytes];
    /* dataBytes payload bytes follow the fixed structure. */
};
struct MPSControlEvent {
    uint64_t sequence, milliseconds;
    uint32_t epoch, event, context, payloadBytes;
    uint8_t payload[kMPSControlEventPayload];
};
struct MPSControlTarget {
    uint16_t handle;
    uint8_t kind, flags; /* bit0 present, bit1 probed, bit2 online */
    uint32_t generation;
    uint64_t wwid, lun, blocks;
    uint32_t blockSize, maxTransfer;
    char vendor[16], product[24], revision[8], serial[32], identity[128];
    uint32_t pathCount, targetCapabilities; // bit0 management-only, bit1 TLR, protection type in bits8..15
};
struct MPSControlInfo {
    uint32_t vendorDevice, capabilities, firmware, requestFrameBytes;
    uint32_t replyFrameBytes, requestCredits, targetLimit, featureBits;
    uint64_t milliseconds;
    uint32_t mpiVersion, pciRevision, reserved[4];
    uint8_t iocFacts[128], pciConfig[256];
};
struct MPSControlDiagInfo {
    uint32_t type, uniqueId, bytes, flags; /* allocated=1, IOC-owned=2, valid=4 */
    uint32_t epoch, reserved[3];
};
static_assert(sizeof(MPSControlInput) == 64, "control input ABI");
static_assert(offsetof(MPSControlOutput, reply) == 64, "control output ABI");
static_assert(sizeof(MPSControlOutput) == 416, "control output size");
static_assert(sizeof(MPSControlEvent) == 160, "event ABI");
static_assert(sizeof(MPSControlTarget) == 256, "target ABI");
static_assert(sizeof(MPSControlInfo) == 448, "adapter ABI");
static inline bool mpsControlInputValid(const MPSControlInput &in, uint64_t bytes,
                                       uint64_t outputBytes)
{
    return in.version == kMPSControlVersion && in.operation >= kMPSControlInfo &&
        in.operation <= kMPSControlDiagUnregister &&
        !(in.flags & ~uint32_t(kMPSControlAllowMutation)) && !in.reserved &&
        !in.reserved2[0] && !in.reserved2[1] &&
        (!in.timeoutMs || (in.timeoutMs >= 100 && in.timeoutMs <= 300000)) &&
        in.requestBytes <= kMPSControlMaxRequest &&
        in.dataOutBytes <= kMPSControlMaxData && in.dataInBytes <= kMPSControlMaxData &&
        bytes == sizeof(in) + uint64_t(in.requestBytes) + in.dataOutBytes &&
        outputBytes >= sizeof(MPSControlOutput) + uint64_t(in.dataInBytes) &&
        outputBytes <= sizeof(MPSControlOutput) + uint64_t(kMPSControlMaxData);
}
static inline uint16_t mpsControlLe16(const uint8_t *p)
{ return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8); }
static inline uint32_t mpsControlLe32(const uint8_t *p)
{ return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
static inline void mpsControlPut16(uint8_t *p, uint16_t n)
{ p[0] = uint8_t(n); p[1] = uint8_t(n >> 8); }
static inline void mpsControlPut32(uint8_t *p, uint32_t n)
{ for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(n >> (i * 8)); }
static inline void mpsControlPut64(uint8_t *p, uint64_t n)
{ for (unsigned i = 0; i < 8; ++i) p[i] = uint8_t(n >> (i * 8)); }
/* Inquiry, capacity, sense, reporting and read CDBs are the only commands
 * permitted without the caller's explicit mutation acknowledgement. */
static inline bool mpsControlReadOnlyCdb(uint8_t opcode)
{
    switch (opcode) {
        case 0x00: case 0x03: case 0x08: case 0x12: case 0x1A: case 0x1C:
        case 0x25: case 0x28: case 0x3C: case 0x4D: case 0x5A: case 0x88:
        case 0x9E: case 0xA0: case 0xA8: return true;
        default: return false;
    }
}
#endif
