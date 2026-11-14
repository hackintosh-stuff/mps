#include "../MPS/MPI/mpi2_mgmt.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Golden wire bytes are literal and decoded independently of the serializer.
 * This tests the actual kernel helper, without an IOC or a synthetic success. */
int main()
{
    uint8_t bytes[64];
    memset(bytes, 0xa5, sizeof(bytes));
    const uint8_t simpleRead[] = {0x34,0x12,0x00,0xd3,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11};
    assert(mpsManagementSimple64(bytes, UINT64_C(0x1122334455667788), 0x1234, false, true));
    assert(memcmp(bytes, simpleRead, sizeof(simpleRead)) == 0);
    assert(bytes[12] == 0xa5);
    const uint8_t smp[] = {
        0x04,0x00,0x00,0x56,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,
        0x20,0x00,0x00,0xd3,0x00,0x20,0x00,0x00,0x00,0x00,0x00,0x00};
    assert(mpsManagementSimple64(bytes, 0x1000, 4, true, false));
    assert(mpsManagementSimple64(bytes + 12, 0x2000, 32, false, true));
    assert(memcmp(bytes, smp, sizeof(smp)) == 0);
    // The outgoing list has EOB and direction, but neither LAST nor EOL.
    assert((bytes[3] & 0xc5) == 0x44);
    assert((bytes[15] & 0xc5) == 0xc1);
    assert(mpsManagementSimple64(bytes, 0x1000, 0x00ffffff, true, true));
    assert(bytes[0] == 255 && bytes[1] == 255 && bytes[2] == 255 && bytes[3] == 0xd7);
    memset(bytes, 0xa5, sizeof(bytes));
    assert(!mpsManagementSimple64(bytes, 0x1000, 0x01000000, false, true));
    for (unsigned i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == 0xa5);
    assert(!mpsManagementSimple64(bytes, UINT64_MAX, 2, false, true));
    assert(!mpsManagementSimple64(NULL, 0, 1, false, true));
    const uint8_t context[] = {0x00,0x00,0x0c,0x00,0,0,0,0,0x78,0x56,0x34,0x12,0x00,0x02,0x00,0x00};
    mpsManagementFirmwareContext(bytes, 0x12345678, 512);
    assert(memcmp(bytes, context, sizeof(context)) == 0);
    assert(bytes[16] == 0xa5);
    assert(kMPSFirmwareContextOffset + sizeof(MPSFirmwareTransactionContext) == kMPSFirmwareSgeOffset);
    uint8_t frame[48] = {};
    frame[0] = 1; frame[3] = 9; frame[7] = 1; frame[12] = 4;
    mpsManagementFirmwareContext(frame + kMPSFirmwareContextOffset, 0, 4);
    assert(mpsManagementSimple64(frame + kMPSFirmwareSgeOffset, 0x1000, 4, true, true));
    const uint8_t firmware[] = {
        1,0,0,9,0,0,0,1,0,0,0,0,4,0,0,0,0,0,0,0,
        0,0,12,0,0,0,0,0,0,0,0,0,4,0,0,0,
        4,0,0,0xd7,0,0x10,0,0,0,0,0,0};
    assert(memcmp(frame, firmware, sizeof(frame)) == 0);
    puts("management kernel serializers: golden Simple64/SMP/firmware TC and invalid boundaries PASS (host only)");
}
