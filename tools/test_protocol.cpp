/* Host-only wire-format regressions; no IOKit, controller, or disk required.
 *
 * Build: c++ -std=c++17 -O1 -g -Wall -Wextra -Werror \
 *   -fsanitize=address,undefined tools/test_protocol.cpp -o /tmp/mps-protocol
 * Run: /tmp/mps-protocol
 *
 * Golden layouts come from SCSI CDB/VPD and FreeBSD MPI2 RAID definitions.
 * SAS2 SGL golden/decoder checks live separately in test_sas2.cpp.
 */
#include "../MPS/MPSScsi.h"
#include "../MPS/MPSCompletion.h"
#include "../MPS/MPSPerformance.h"
#include "../MPS/MPSControlABI.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>

static unsigned checks;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        std::cerr << "line " << __LINE__ << ": " #condition "\n"; \
        std::exit(1); \
    } \
} while (0)

static void cdbTests()
{
    uint8_t cdb[16];
    CHECK(mpsBuildReadWriteCdb(cdb, false, 0x12345678, 0x1234, false) == 10);
    const uint8_t read10[16] = {
        0x28, 0, 0x12, 0x34, 0x56, 0x78, 0, 0x12, 0x34, 0, 0, 0, 0, 0, 0, 0
    };
    CHECK(std::memcmp(cdb, read10, sizeof(cdb)) == 0);
    CHECK(mpsBuildReadWriteCdb(cdb, true, 0x12345678, 0x1234, true) == 10);
    CHECK(cdb[0] == 0x2a && cdb[1] == 8);

    // Last 512-byte sector below 2 TiB, followed by a transfer crossing it.
    CHECK(mpsBuildReadWriteCdb(cdb, false, UINT64_C(0xffffffff), 1, false) == 10);
    CHECK(mpsBuildReadWriteCdb(cdb, true, UINT64_C(0xffffffff), 2, true) == 16);
    const uint8_t write16[16] = {
        0x8a, 8, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 2, 0, 0
    };
    CHECK(std::memcmp(cdb, write16, sizeof(cdb)) == 0);
    CHECK(mpsBuildReadWriteCdb(cdb, false, UINT64_C(0x1122334455667788),
                              0x12345678, false) == 16);
    const uint8_t read16[16] = {
        0x88, 0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x12, 0x34, 0x56, 0x78, 0, 0
    };
    CHECK(std::memcmp(cdb, read16, sizeof(cdb)) == 0);
    CHECK(mpsBuildReadWriteCdb(cdb, false, 0, 65535, false) == 10);
    CHECK(mpsBuildReadWriteCdb(cdb, false, 0, 65536, false) == 16);
    CHECK(mpsBuildReadWriteCdb(cdb, false, UINT64_MAX, 1, false) == 16);
    CHECK(mpsBuildReadWriteCdb(cdb, false, UINT64_MAX, 2, false) == 0);
    CHECK(mpsBuildReadWriteCdb(cdb, false, 0, 0, false) == 0);

    CHECK(mpsBuildSynchronizeCdb(cdb, UINT64_C(0x100000000), 0x10203) == 16);
    const uint8_t sync16[16] = {
        0x91, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 2, 3, 0, 0
    };
    CHECK(std::memcmp(cdb, sync16, sizeof(cdb)) == 0);
    CHECK(mpsBuildSynchronizeCdb(cdb, 0x12345678, 0x1234) == 10);
    CHECK(cdb[0] == 0x35 && cdb[2] == 0x12 && cdb[7] == 0x12);
    CHECK(mpsBuildSynchronizeCdb(cdb, 0, UINT64_C(0x100000000)) == 10);
    CHECK(cdb[0] == 0x35);
    for (unsigned i = 1; i < sizeof(cdb); ++i) CHECK(cdb[i] == 0);
    CHECK(mpsBuildSynchronizeCdb(cdb, UINT64_MAX, 2) == 0);
}

static void vpdAndRaidTests()
{
    // Block Limits page: 4096 blocks, three descriptors, 8-block hint,
    // alignment two. Logical Block Provisioning page: LBPU and LBPRZ.
    std::array<uint8_t, 36> limits{};
    limits[1] = 0xb0; limits[3] = 32;
    limits[22] = 0x10; limits[27] = 3; limits[31] = 8;
    limits[32] = 0x80; limits[35] = 2;
    std::array<uint8_t, 8> provisioning{{0, 0xb2, 0, 4, 0, 0x84, 0, 0}};
    MPSUnmapLimits out{};
    CHECK(mpsParseUnmapLimits(limits.data(), limits.size(),
                              provisioning.data(), provisioning.size(), &out));
    CHECK(out.maxBlocks == 4096 && out.maxDescriptors == 3);
    CHECK(out.optimalGranularity == 8 && out.alignmentValid && out.alignment == 2);
    CHECK(out.readsZero);
    CHECK(!mpsParseUnmapLimits(limits.data(), 35, provisioning.data(), 8, &out));
    CHECK(!mpsParseUnmapLimits(limits.data(), 36, provisioning.data(), 7, &out));
    limits[1] = 0xb1;
    CHECK(!mpsParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));
    limits[1] = 0xb0;
    limits[3] = 31;
    CHECK(!mpsParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));
    limits[3] = 33;
    CHECK(!mpsParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));
    limits[3] = 32;
    provisioning[5] = 4;
    CHECK(!mpsParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));
    provisioning[5] = 0x80;
    CHECK(mpsParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));
    CHECK(!out.readsZero);
    limits[22] = 0;
    CHECK(!mpsParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));
    limits[22] = 0x10; limits[27] = 0;
    CHECK(!mpsParseUnmapLimits(limits.data(), 36, provisioning.data(), 8, &out));

    CHECK(mpsValidBlockExtent(99, 1, 100));
    CHECK(!mpsValidBlockExtent(99, 2, 100));
    CHECK(!mpsValidBlockExtent(100, 1, 100));
    CHECK(!mpsValidBlockExtent(UINT64_MAX - 2, 4, UINT64_MAX));
    CHECK(mpsValidBlockExtent(UINT64_MAX - 2, 2, UINT64_MAX));
    CHECK(mpsValidBlockExtent(UINT64_MAX, 0, 0));

    // Genuine extended-page header: number@2, type@3, ext type@6.
    // This deliberately differs from the prototype's former wrong offsets.
    std::array<uint8_t, 64> raid{};
    raid[3] = 0x0f; raid[4] = 16; raid[6] = 0x16; raid[0x2c] = 2;
    raid[0x32] = 0xcd; raid[0x33] = 0xab; // volume ABCD
    raid[0x38] = 1;                     // physical member
    raid[0x3a] = 0xcd; raid[0x3b] = 0xab;
    raid[0x3e] = 0x34; raid[0x3f] = 0x12;
    bool contains = false;
    CHECK(mpsRaidConfigContains(raid.data(), raid.size(), 0x1234, &contains) && contains);
    CHECK(mpsRaidConfigContains(raid.data(), raid.size(), 0xabcd, &contains) && contains);
    CHECK(mpsRaidConfigContains(raid.data(), raid.size(), 0x4321, &contains) && !contains);
    CHECK(mpsRaidConfigContains(raid.data(), raid.size(), 0, &contains) && !contains);
    CHECK(!mpsRaidConfigContains(raid.data(), 63, 0x1234, &contains));
    raid[0x2c] = 3;
    CHECK(!mpsRaidConfigContains(raid.data(), 64, 0x1234, &contains));
    raid[0x2c] = 2; raid[2] = 1;
    CHECK(!mpsRaidConfigContains(raid.data(), 64, 0x1234, &contains));
    raid[2] = 0; raid[6] = 0x12;
    CHECK(!mpsRaidConfigContains(raid.data(), 64, 0x1234, &contains));
    raid[6] = 0x16; raid[0x38] = 4;
    CHECK(!mpsRaidConfigContains(raid.data(), 64, 0x1234, &contains));
    raid[0x38] = 2;
    CHECK(mpsRaidConfigContains(raid.data(), 64, 0x1234, &contains) && contains);
    raid[0x38] = 3;
    CHECK(mpsRaidConfigContains(raid.data(), 64, 0x1234, &contains) && contains);
    CHECK(!mpsRaidConfigContains(nullptr, 64, 0x1234, &contains));
}

static void senseTests()
{
    std::array<uint8_t, 14> fixed{{0x70, 0, 5, 0, 0, 0, 0, 6, 0, 0, 0, 0, 0x20, 1}};
    std::array<uint8_t, 8> descriptor{{0x72, 6, 0x29, 2, 0, 0, 0, 0}};
    MPSSenseFields first{}, second{};
    CHECK(mpsDecodeSenseFields(fixed.data(), fixed.size(), &first));
    CHECK(first.key == 5 && first.asc == 0x20 && first.ascq == 1);
    CHECK(mpsDecodeSenseFields(descriptor.data(), descriptor.size(), &second));
    CHECK(second.key == 6 && second.asc == 0x29 && second.ascq == 2);
    CHECK(first.key == 5 && first.asc == 0x20 && first.ascq == 1);
    fixed[0] = 0xf1; descriptor[0] = 0x73;
    CHECK(mpsDecodeSenseFields(fixed.data(), fixed.size(), &first));
    CHECK(mpsDecodeSenseFields(descriptor.data(), descriptor.size(), &second));

    // Actual short allocations let ASan detect any attempted ASC/ASCQ read.
    const std::array<uint8_t, 8> shortFixed{{0x70, 0, 3, 0, 0, 0, 0, 255}};
    CHECK(mpsDecodeSenseFields(shortFixed.data(), shortFixed.size(), &first));
    CHECK(first.key == 3 && first.asc == 0 && first.ascq == 0);
    CHECK(mpsDecodeSenseFields(fixed.data(), 13, &first));
    CHECK(first.asc == 0 && first.ascq == 0);
    fixed[7] = 0;
    CHECK(mpsDecodeSenseFields(fixed.data(), fixed.size(), &first));
    CHECK(first.asc == 0 && first.ascq == 0);
    CHECK(!mpsDecodeSenseFields(descriptor.data(), 7, &second));
    descriptor[0] = 0x55;
    CHECK(!mpsDecodeSenseFields(descriptor.data(), 8, &second));
    CHECK(!mpsDecodeSenseFields(nullptr, 8, &second));
}

static void performanceLimitTests()
{
    CHECK(mpsConfiguredCommandDepth(0) == 32);
    CHECK(mpsConfiguredCommandDepth(-1) == 32);
    CHECK(mpsConfiguredCommandDepth(1) == 1);
    CHECK(mpsConfiguredCommandDepth(4) == 4);
    CHECK(mpsConfiguredCommandDepth(32) == 32);
    CHECK(mpsConfiguredCommandDepth(33) == 32);
    CHECK(mpsConfiguredCommandDepth(2147483647) == 32);
    CHECK(!mpsIsAsyncSmid(0));
    CHECK(!mpsIsAsyncSmid(31));
    CHECK(mpsIsAsyncSmid(32));
    CHECK(mpsIsAsyncSmid(63));
    CHECK(!mpsIsAsyncSmid(64));
    CHECK(!mpsIsAsyncSmid(UINT32_MAX));
    CHECK(mpsAsyncCommandLimit(32, 128, 128, 144, 144) == 32);
    CHECK(mpsAsyncCommandLimit(32, 33, 128, 144, 144) == 1);
    CHECK(mpsAsyncCommandLimit(32, 128, 128, 10, 144) == 1);
    CHECK(mpsAsyncCommandLimit(32, 128, 128, 144, 9) == 0);

    // Signed wide arithmetic is an independent oracle: narrow unsigned
    // subtraction in the production helper must not wrap below a reservation.
    const uint32_t allocations[] = {0, 1, 4, 32, UINT32_MAX};
    const uint32_t requests[] = {0, 31, 32, 33, 64, 128, UINT32_MAX};
    const uint32_t replies[] = {0, 9, 10, 40, 144, UINT32_MAX};
    for (uint32_t allocated : allocations)
      for (uint32_t frames : requests)
        for (uint32_t credit : requests)
          for (uint32_t post : replies)
            for (uint32_t free : replies) {
                int64_t expected = 32;
                const int64_t bounds[] = {
                    allocated, (int64_t)frames - 32, (int64_t)credit - 32,
                    (int64_t)post - 9, (int64_t)free - 9, 96
                };
                for (int64_t bound : bounds) expected = std::min(expected, bound);
                expected = std::max(INT64_C(0), expected);
                const uint32_t actual = mpsAsyncCommandLimit(
                    allocated, frames, credit, post, free);
                CHECK(actual == (uint32_t)expected);
                CHECK(actual <= allocated && actual <= 32);
                if (actual) {
                    CHECK(32 + actual <= frames && 32 + actual <= credit);
                    CHECK(actual + 9 <= post && actual + 9 <= free);
                    CHECK(mpsIsAsyncSmid(32 + actual - 1));
                    CHECK(32 + actual - 1 < 128);
                }
            }

    // Completion order must not turn successful chunks beyond a hole into
    // reportable bytes. Include a partially copied first failing read chunk.
    for (unsigned failed = 0; failed < 4; ++failed) {
        const uint64_t partials[] = {0, 1, 65535};
        for (uint64_t partial : partials) {
            std::array<unsigned, 4> order{{0, 1, 2, 3}};
            do {
                uint64_t sum = 0, firstError = UINT64_MAX;
                for (unsigned index : order) {
                    if (index == failed) {
                        sum += partial;
                        firstError = (uint64_t)index * 65536 + partial;
                    } else sum += 65536;
                }
                CHECK(mpsCompletedPrefix(sum, firstError) ==
                      (uint64_t)failed * 65536 + partial);
            } while (std::next_permutation(order.begin(), order.end()));
        }
    }
    CHECK(mpsCompletedPrefix(0, UINT64_MAX) == 0);
    CHECK(mpsCompletedPrefix(UINT64_C(4) * 65536, UINT64_MAX) == 262144);
    CHECK(mpsCompletedPrefix(65536, 131072) == 65536);
    CHECK(mpsCompletedPrefix(UINT64_MAX, UINT64_MAX) == UINT64_MAX);
}

static void managementAndLunTests()
{
    MPSControlInput in = {};
    in.version = kMPSControlVersion; in.operation = kMPSControlInfo;
    CHECK(mpsControlInputValid(in, sizeof(in), sizeof(MPSControlOutput)));
    const auto valid = [](const MPSControlInput &x) {
        return mpsControlInputValid(x, sizeof(x) + uint64_t(x.requestBytes) + x.dataOutBytes,
                                    sizeof(MPSControlOutput) + uint64_t(x.dataInBytes));
    };
    for (unsigned op = 0; op <= kMPSControlDiagUnregister + 1; ++op) {
        in.operation = op;
        CHECK(valid(in) == (op >= kMPSControlInfo && op <= kMPSControlDiagUnregister));
    }
    in.operation = kMPSControlPassthrough;
    in.requestBytes = 96; in.dataOutBytes = 65536; in.dataInBytes = 0;
    CHECK(valid(in));
    CHECK(!mpsControlInputValid(in, 64 + 96 + 65535, 416));
    CHECK(!mpsControlInputValid(in, UINT64_MAX, 416));
    in.dataInBytes = kMPSControlMaxData;
    CHECK(valid(in));
    CHECK(!mpsControlInputValid(in, 64 + 96 + 65536, 416 + uint64_t(in.dataInBytes) - 1));
    in.dataInBytes++; CHECK(!valid(in)); in.dataInBytes = 0;
    for (unsigned bit = 0; bit < 32; ++bit) {
        in.flags = 1U << bit; CHECK(valid(in) == (bit == 0));
    }
    in.flags = 0;
    const uint32_t timeouts[] = {0, 1, 99, 100, 1000, 300000, 300001, UINT32_MAX};
    for (auto timeout : timeouts) {
        in.timeoutMs = timeout;
        CHECK(valid(in) == (!timeout || (timeout >= 100 && timeout <= 300000)));
    }
    in.timeoutMs = 0;
    in.version = 2; CHECK(!valid(in)); in.version = 1;
    in.reserved = 1; CHECK(!valid(in)); in.reserved = 0;
    for (unsigned i = 0; i < 2; ++i) { in.reserved2[i] = 1; CHECK(!valid(in)); in.reserved2[i] = 0; }
    in.requestBytes = 257; CHECK(!valid(in)); in.requestBytes = 256; CHECK(valid(in));
    in.dataOutBytes = kMPSControlMaxData + 1; CHECK(!valid(in));
    in.dataOutBytes = kMPSControlMaxData; CHECK(valid(in));
    CHECK(!mpsControlInputValid(in, 64 + 256 + uint64_t(in.dataOutBytes), UINT64_MAX));
    CHECK(mpsControlReadOnlyCdb(0x12)); CHECK(mpsControlReadOnlyCdb(0x88));
    CHECK(!mpsControlReadOnlyCdb(0x8A)); CHECK(!mpsControlReadOnlyCdb(0x42));
    CHECK(!mpsControlReadOnlyCdb(0x04)); CHECK(!mpsControlReadOnlyCdb(0x3B));
    uint8_t lun[8]; uint32_t decoded = 0;
    const uint32_t edges[] = {0, 1, 255, 256, 16383, 16384, 65535, 0xFFFFFE, 0xFFFFFF};
    for (uint32_t number : edges) {
        CHECK(mpsEncodeLun(lun, number));
        CHECK(mpsDecodeLun(lun, &decoded) && decoded == number);
        CHECK(uint16_t(mpsTargetKey(0x1234, number)) == 0x1234);
        CHECK((mpsTargetKey(0x1234, number) >> 16) == number);
    }
    CHECK(!mpsEncodeLun(lun, 0x1000000));
    CHECK(!mpsDecodeLun(nullptr, &decoded)); CHECK(!mpsDecodeLun(lun, nullptr));
    for (uint32_t number = 0; number < 65536; ++number) {
        CHECK(mpsEncodeLun(lun, number));
        CHECK(mpsDecodeLun(lun, &decoded) && decoded == number);
        lun[7] = 1; CHECK(!mpsDecodeLun(lun, &decoded));
    }
    CHECK(mpsEncodeLun(lun, 0x123456));
    const uint8_t extended[8] = {0xD2, 0x12, 0x34, 0x56, 0, 0, 0, 0};
    CHECK(std::memcmp(lun, extended, 8) == 0);
    lun[0] = 0x80; CHECK(!mpsDecodeLun(lun, &decoded));

}

int main()
{
    managementAndLunTests();
    performanceLimitTests();
    cdbTests();
    vpdAndRaidTests();
    senseTests();
    std::cout << "Protocol checks passed: " << checks << " assertions (CDB/VPD/RAID/sense/scheduler/native ABI/LUN; host only).\n";
}
