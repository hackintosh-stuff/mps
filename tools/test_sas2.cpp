// Host-only checks of actual SAS2 encoder output. No controller or kernel mock.
// The decoder below reads MPI bytes independently of the production encoder.
#include "../MPS/MPSDmaSgl.h"
#include "../MPS/MPSBinding.h"
#include "../MPS/MPSWire.h"
#include "../MPS/MPSWarpDrive.h"
#include "../MPS/MPSIdentity.h"
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <limits>

static unsigned long checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
static uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
static uint32_t le24(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }
static uint64_t le64(const uint8_t *p) {
    uint64_t n=0; for (unsigned i=0;i<8;++i) n |= uint64_t(p[i]) << (8*i); return n;
}

static void decode(const uint8_t *request, unsigned requestBytes, unsigned offset,
                   const uint8_t *storage, uint64_t storageAddress, unsigned frameBytes,
                   unsigned storageBytes, const MPSSglResult &result,
                   const std::vector<MPSDmaSegment> &expected, bool write) {
    const uint8_t *frame=request;
    unsigned begin=offset, bytes=requestBytes, next=result.requestChainOffset*4;
    unsigned segments=0, chains=0;
    uint64_t total=0;
    for (;;) {
        const unsigned end=next ? next : begin+unsigned(expected.size()-segments)*12;
        CHECK(end>=begin && end<=bytes && (end-begin)%12==0);
        const unsigned count=(end-begin)/12;
        // Main request may contain a chain with no preceding data if only one
        // SGE fits. A nonfinal chain frame must always make forward progress.
        CHECK(count || (chains==0 && next));
        for (unsigned i=0;i<count;++i) {
            CHECK(segments<expected.size());
            const uint8_t *s=frame+begin+i*12;
            const bool terminal=segments+1==expected.size();
            const bool lastInFrame=i+1==count;
            const uint8_t flags=uint8_t(0x12 | (write ? 4 : 0) |
                (terminal ? 0xc1 : lastInFrame && next ? 0x80 : 0));
            CHECK(s[3]==flags);
            CHECK(le24(s)==expected[segments].length);
            CHECK(le64(s+4)==expected[segments].address);
            total+=le24(s);
            ++segments;
        }
        if (!next) {
            if (chains) CHECK(end==bytes); // final chain length has no unused elements
            break;
        }
        CHECK(next+12<=bytes);
        const uint8_t *link=frame+next;
        CHECK(link[3]==0x32); // CHAIN | 64-bit; never data-direction/EOB/EOL.
        const unsigned length=le16(link), following=unsigned(link[2])*4;
        CHECK(length>=12 && length<=frameBytes && length%12==0);
        CHECK(following==0 || (following+12==length && following%12==0));
        const uint64_t address=le64(link+4);
        CHECK(address==storageAddress+uint64_t(chains)*frameBytes);
        CHECK(address>=storageAddress && address-storageAddress+length<=storageBytes);
        frame=storage+unsigned(address-storageAddress);
        bytes=length; begin=0; next=following;
        CHECK(++chains<=result.chainsUsed);
    }
    CHECK(chains==result.chainsUsed);
    CHECK(segments==expected.size());
    uint64_t wanted=0; for (const auto &s:expected) wanted+=s.length;
    CHECK(total==wanted);
}

static std::vector<MPSDmaSegment> segments(unsigned n, uint32_t seed) {
    std::vector<MPSDmaSegment> v;
    for (unsigned i=0;i<n;++i) {
        seed=seed*1664525U+1013904223U;
        v.push_back({UINT64_C(0x123400000)+uint64_t(i)*0x20000+(seed&0xfff),
                     1U+(seed&0x3fff)});
    }
    return v;
}
static uint32_t total(const std::vector<MPSDmaSegment> &v) {
    uint64_t n=0; for (const auto &s:v) n+=s.length;
    CHECK(n<=UINT32_MAX); return uint32_t(n);
}

static void golden() {
    std::array<uint8_t,12> simple{};
    mpsMpiSimple(simple.data(), UINT64_C(0x1122334455667788),0x345678,false);
    const uint8_t read[]={0x78,0x56,0x34,0xd3,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11};
    CHECK(std::memcmp(simple.data(),read,12)==0);
    mpsMpiSimple(simple.data(), UINT64_C(0x1122334455667788),0x345678,true);
    simple[3]^=4; CHECK(std::memcmp(simple.data(),read,12)==0);

    std::array<uint8_t,128> request{};
    std::array<uint8_t,512> chain{};
    const uint64_t address=UINT64_C(0x12345000);
    std::vector<MPSDmaSegment> v={{0x1000,512},{0x2000,1024},{0x3000,2048}};
    MPSSglResult result{};
    CHECK(mpsBuildMpiSgl(request.data(),128,96,v.data(),3,total(v),
        chain.data(),address,128,4,512,false,&result));
    CHECK(result.requestChainOffset==27 && result.chainsUsed==1);
    const uint8_t main[]={0x00,0x02,0x00,0x92,0x00,0x10,0,0,0,0,0,0,
                         0x18,0,0,0x32,0x00,0x50,0x34,0x12,0,0,0,0};
    const uint8_t data[]={0x00,0x04,0x00,0x12,0x00,0x20,0,0,0,0,0,0,
                         0x00,0x08,0x00,0xd3,0x00,0x30,0,0,0,0,0,0};
    CHECK(std::memcmp(request.data()+96,main,sizeof(main))==0);
    CHECK(std::memcmp(chain.data(),data,sizeof(data))==0);
    decode(request.data(),128,96,chain.data(),address,128,512,result,v,false);
    v=segments(12,17);
    CHECK(mpsBuildMpiSgl(request.data(),128,96,v.data(),12,total(v),
        chain.data(),address,128,4,512,true,&result));
    CHECK(result.chainsUsed==2 && result.requestChainOffset==27);
    CHECK(request[108]==120 && request[109]==0 && request[110]==27 && request[111]==0x32);
    CHECK(chain[108]==24 && chain[109]==0 && chain[110]==0 && chain[111]==0x32);
    decode(request.data(),128,96,chain.data(),address,128,512,result,v,true);
}

static void layouts() {
    for (unsigned bytes: {108U,128U,256U,512U}) {
        for (unsigned chainBytes: {128U,256U,512U}) {
            for (unsigned count=1;count<=64;++count) {
                for (bool write: {false,true}) {
                    std::array<uint8_t,512> request;
                    std::array<uint8_t,4096> chains;
                    request.fill(0xa5); chains.fill(0x5a);
                    auto v=segments(count,bytes*chainBytes+count);
                    MPSSglResult result{0xabcdef,0xbc};
                    bool ok=mpsBuildMpiSgl(request.data(),bytes,96,v.data(),count,total(v),
                        chains.data(),0x23456000,chainBytes,4,4096,write,&result);
                    const unsigned inlineSlots=(bytes-96)/12;
                    unsigned needed=0;
                    if (count>inlineSlots) {
                        unsigned remaining=count-(inlineSlots-1), slots=chainBytes/12;
                        while (remaining) { ++needed; remaining=remaining<=slots ? 0 : remaining-(slots-1); }
                    }
                    CHECK(ok==(needed<=4));
                    if (ok) {
                        CHECK(result.chainsUsed==needed);
                        decode(request.data(),bytes,96,chains.data(),0x23456000,
                            chainBytes,4096,result,v,write);
                        for (unsigned i=0;i<96;++i) CHECK(request[i]==0xa5);
                        for (unsigned i=bytes;i<request.size();++i) CHECK(request[i]==0xa5);
                    } else {
                        for (auto b:request) CHECK(b==0xa5);
                        for (auto b:chains) CHECK(b==0x5a);
                        CHECK(result.chainsUsed==0xabcdef && result.requestChainOffset==0xbc);
                    }
                }
            }
        }
    }
}

static void rejection() {
    std::array<uint8_t,4096> request, chains;
    auto good=segments(3,23);
    auto reject=[&](std::vector<MPSDmaSegment> v,unsigned requestBytes,unsigned offset,
                    unsigned expected,uint64_t chainAddress,unsigned frameBytes,
                    unsigned depth,unsigned storageBytes) {
        request.fill(0xa5);chains.fill(0x5a);
        MPSSglResult result{0xabcdef,0xbc};
        CHECK(!mpsBuildMpiSgl(request.data(),requestBytes,offset,v.data(),unsigned(v.size()),expected,
            chains.data(),chainAddress,frameBytes,depth,storageBytes,true,&result));
        for (auto b:request) CHECK(b==0xa5);
        for (auto b:chains) CHECK(b==0x5a);
        CHECK(result.chainsUsed==0xabcdef && result.requestChainOffset==0xbc);
    };
    reject(good,128,96,total(good)-1,0x1000,128,4,4096);
    reject(good,128,96,total(good)+1,0x1000,128,4,4096);
    reject(good,128,96,0,0x1000,128,4,4096);
    auto bad=good; bad[0].length=0;
    reject(bad,128,96,total(bad),0x1000,128,4,4096);
    bad=good;bad[0].length=0x1000000;
    reject(bad,128,96,total(bad),0x1000,128,4,4096);
    bad=good;bad[0].address=UINT64_C(0x7fffffffffffffff);
    reject(bad,128,96,total(bad),0x1000,128,4,4096);
    bad=good;bad[0].address=UINT64_C(0x8000000000000000);
    reject(bad,128,96,total(bad),0x1000,128,4,4096);
    reject(good,128,97,total(good),0x1000,128,4,4096);
    reject(good,128,128,total(good),0x1000,128,4,4096);
    reject(good,104,96,total(good),0x1000,128,4,4096);
    reject(good,128,96,total(good),0x1000,128,0,4096);
    reject(good,128,96,total(good),0x1000,128,4,127);
    reject(good,128,96,total(good),0,128,4,4096);
    reject(good,128,96,total(good),0x1001,128,4,4096);
    reject(good,128,96,total(good),0x1000,20,4,4096);
    reject(good,128,96,total(good),UINT64_C(0x7ffffffffffffffc),128,4,4096);
    auto many=segments(64,23);
    reject(many,128,96,total(many),0x1000,128,4,4096);
    // Request chain offset cannot exceed the one-byte DWORD field.
    reject(good,1052,1024,total(good),0x1000,128,4,4096);
    std::vector<MPSDmaSegment> max={{0x1000,0xffffff}};
    MPSSglResult result{};
    CHECK(mpsBuildMpiSgl(request.data(),128,96,max.data(),1,total(max),
        nullptr,0,0,0,0,false,&result));
    CHECK(request[96]==0xff && request[97]==0xff && request[98]==0xff && request[99]==0xd3);
}

static void bindings() {
    // These call the shared production admission predicate, not a second
    // implementation. Kernel call-site/locking coverage needs source review
    // and later hardware qualification; this host case does not simulate it.
    const uint64_t oldNub=41, replacementAtSameHandle=42;
    CHECK(!mpsBindingMatches(oldNub,replacementAtSameHandle,true));
    CHECK(!mpsBindingMatches(oldNub,oldNub,false)); // invalid before deferred OFFLINE
    CHECK(mpsBindingMatches(oldNub,oldNub,true)); // same logical unit, new handle
    CHECK(!mpsBindingMatches(0,0,true));
    CHECK(!mpsBindingMatches(0,oldNub,true));
    CHECK(!mpsBindingMatches(oldNub,0,true));
    CHECK(!mpsBindingMatches(UINT64_MAX,0,true));
    CHECK(!mpsBindingMatches(UINT64_MAX,1,true)); // wrap/recycled value is not equal
    CHECK(mpsBindingMatches(UINT64_MAX,UINT64_MAX,true));

    // A removed slot still has a historical published identity. There is no
    // probeValid parameter that could suppress replacement detection when
    // the same SATA port placeholder and firmware handle are reused.
    CHECK(mpsPublishedIdentityChanged(true,"ata:A","ata:B",1000,1000,512,512));
    CHECK(!mpsPublishedIdentityChanged(true,"ata:A","ata:A",1000,1000,512,512));
    CHECK(!mpsPublishedIdentityChanged(false,"","ata:A",0,1000,0,512));
    CHECK(mpsPublishedIdentityChanged(true,"ata:A","ata:A",1000,1001,512,512));
    CHECK(mpsPublishedIdentityChanged(true,"ata:A","ata:A",1000,1000,512,4096));
    CHECK(mpsPublishedIdentityChanged(true,"ata:A","",1000,1000,512,512));
    // Reason alone selects conservative reprobe; neither a default handle
    // nor currently selected target can hide an event on another target.
    for (unsigned reason=0;reason<256;++reason)
        CHECK(mpsTopologyRequiresReprobe(uint8_t(reason))==(reason==1 || reason==2 || reason==3));
}

static void warpDrive() {
    // Independent Page10 wire fixture: OEM at ProductSpecificInfo low byte,
    // visibility in the sixth product DWORD's low byte (offset 0x18).
    std::array<uint8_t,0x1c> page{};
    page[0]=0; page[1]=7; page[2]=10; page[3]=9; page[4]=0x80;
    page[0x18]=0;
    CHECK(mpsWarpDrivePolicy(page.data(),page.size())==0);
    page[0x18]=1;
    CHECK(mpsWarpDrivePolicy(page.data(),page.size())==1);
    page[0x18]=2;
    CHECK(mpsWarpDrivePolicy(page.data(),page.size())==2);
    page[0x18]=0xfe; // policy uses only the low two bits
    CHECK(mpsWarpDrivePolicy(page.data(),page.size())==2);
    page[0x18]=3;
    CHECK(mpsWarpDrivePolicy(page.data(),page.size())==-1);
    page[0x18]=2; page[4]=0x81;
    CHECK(mpsWarpDrivePolicy(page.data(),page.size())==-1);
    page[4]=0x80;
    for (unsigned bytes=0;bytes<page.size();++bytes)
        CHECK(mpsWarpDrivePolicy(page.data(),bytes)==-1);
    CHECK(mpsWarpDrivePolicy(nullptr,page.size())==-1);
    for (bool known: {false,true}) {
        for (bool volume: {false,true}) {
            CHECK(mpsWarpDriveHidesPhysical(-1,known,volume)); // read/parse failure defaults hide
            CHECK(mpsWarpDriveHidesPhysical(0,known,volume));
            CHECK(!mpsWarpDriveHidesPhysical(1,known,volume));
            CHECK(mpsWarpDriveHidesPhysical(3,known,volume));
        }
    }
    CHECK(mpsWarpDriveHidesPhysical(2,false,false)); // no proof of volume absence
    CHECK(mpsWarpDriveHidesPhysical(2,false,true));
    CHECK(mpsWarpDriveHidesPhysical(2,true,true));
    CHECK(!mpsWarpDriveHidesPhysical(2,true,false)); // only proved no-volume exposes
}

static void identities() {
    const auto serial=[](const char *p) {
        return mpsSerialIsTrusted(reinterpret_cast<const uint8_t *>(p),std::strlen(p));
    };
    CHECK(!mpsSerialIsTrusted(nullptr,20));
    CHECK(!serial(""));
    CHECK(!serial("                    "));
    CHECK(!serial("00000000000000000000"));
    CHECK(!serial("FFFFFFFFFFFFFFFFFFFF"));
    CHECK(!serial("unknown"));
    CHECK(!serial("UnKnOwN"));
    CHECK(!serial("n/a"));
    CHECK(!serial("none"));
    CHECK(!serial("null"));
    CHECK(!serial("default"));
    CHECK(!serial("serialnumber"));
    CHECK(!serial("0123456789"));
    CHECK(!serial("1234567890"));
    CHECK(!serial("AAAAAAAAAAAA"));
    CHECK(!serial("  UNKNOWN  "));
    CHECK(!serial("abc"));
    CHECK(!serial("...."));
    CHECK(!serial("AB\tCD"));
    CHECK(!serial("ATA PCCOOLER SSD T20")); // model text with spaces is no serial
    std::array<uint8_t,20> invalid{};
    CHECK(!mpsSerialIsTrusted(invalid.data(),invalid.size()));
    invalid.fill(0xff);
    CHECK(!mpsSerialIsTrusted(invalid.data(),invalid.size()));
    invalid.fill(0); invalid[0]='A'; invalid[1]='B'; invalid[3]='C'; invalid[4]='D';
    CHECK(!mpsSerialIsTrusted(invalid.data(),invalid.size())); // embedded NUL is invalid
    CHECK(serial("S3Z9NX0K123456"));
    CHECK(serial("  SN2026-0142  "));
    const uint8_t padded[]={' ',0,'S','N','2','0','2','6','-','0','1','4','2',0,' '};
    CHECK(mpsSerialIsTrusted(padded,sizeof(padded)));
    std::array<uint8_t,129> oversized{};
    CHECK(!mpsSerialIsTrusted(oversized.data(),oversized.size()));

    const uint8_t naa5[]={0x50,0x0a,0x0d,0x10,0x01,0x2d,0x17,0xe0};
    const uint8_t naa6[]={0x60,0x0a,0x0d,0x10,0x01,0x2d,0x17,0xe0,
                         0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
    const uint8_t eui[]={0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0x10};
    CHECK(mpsVpd83IdentifierTrusted(0,1,3,naa5,sizeof(naa5)));
    CHECK(mpsVpd83IdentifierTrusted(0,1,3,naa6,sizeof(naa6)));
    CHECK(mpsVpd83IdentifierTrusted(0,1,2,eui,8));
    CHECK(mpsVpd83IdentifierTrusted(0,1,2,eui,12));
    CHECK(mpsVpd83IdentifierTrusted(0,1,2,eui,16));
    CHECK(!mpsVpd83IdentifierTrusted(0,1,3,nullptr,8));
    CHECK(!mpsVpd83IdentifierTrusted(0,1,3,naa5,0));
    for (uint8_t association: {uint8_t(1),uint8_t(2),uint8_t(3)}) {
        CHECK(!mpsVpd83IdentifierTrusted(association,1,3,naa5,sizeof(naa5)));
        CHECK(!mpsVpd83IdentifierTrusted(association,1,2,eui,8));
    }
    CHECK(!mpsVpd83IdentifierTrusted(0,2,3,naa5,sizeof(naa5)));
    CHECK(!mpsVpd83IdentifierTrusted(0,3,3,naa6,sizeof(naa6)));
    CHECK(!mpsVpd83IdentifierTrusted(0,1,3,naa5,7));
    CHECK(!mpsVpd83IdentifierTrusted(0,1,3,naa6,8));
    CHECK(!mpsVpd83IdentifierTrusted(0,1,3,naa6,15));
    CHECK(!mpsVpd83IdentifierTrusted(0,1,2,eui,7));
    CHECK(!mpsVpd83IdentifierTrusted(0,1,2,eui,9));
    const uint8_t synthetic[]={0x44,0x33,0x22,0x11,0,0,0,0};
    CHECK(!mpsVpd83IdentifierTrusted(0,1,3,synthetic,sizeof(synthetic)));
    std::array<uint8_t,16> identifier{};
    CHECK(!mpsVpd83IdentifierTrusted(0,1,2,identifier.data(),8));
    CHECK(!mpsVpd83IdentifierTrusted(0,1,3,identifier.data(),16));
    identifier.fill(0xff);
    CHECK(!mpsVpd83IdentifierTrusted(0,1,2,identifier.data(),8));
    CHECK(!mpsVpd83IdentifierTrusted(0,1,3,identifier.data(),16));
    identifier.fill(0x55);
    CHECK(!mpsVpd83IdentifierTrusted(0,1,3,identifier.data(),8));
    identifier.fill(0); identifier[4]=0x12; identifier[7]=0x34;
    CHECK(!mpsVpd83IdentifierTrusted(0,1,2,identifier.data(),8)); // missing EUI company prefix
    identifier.fill(0xff); identifier[4]=0x12;
    CHECK(!mpsVpd83IdentifierTrusted(0,1,2,identifier.data(),8));
    const char model[]="ATA PCCOOLER SSD T20";
    for (uint8_t type: {uint8_t(1),uint8_t(8)}) {
        CHECK(!mpsVpd83IdentifierTrusted(0,2,type,
            reinterpret_cast<const uint8_t *>(model),std::strlen(model)));
        CHECK(!mpsVpd83IdentifierTrusted(0,1,type,
            reinterpret_cast<const uint8_t *>(model),std::strlen(model)));
        CHECK(!mpsVpd83IdentifierTrusted(0,1,type,naa5,sizeof(naa5)));
    }
}

static void replyHeaders() {
    std::array<uint8_t,64> facts{};
    facts[2]=16; facts[3]=3;
    CHECK(mpsReplyHeaderValid(facts.data(),64,3,64));
    CHECK(!mpsReplyHeaderValid(facts.data(),63,3,64));
    CHECK(!mpsReplyHeaderValid(facts.data(),64,2,64));
    facts[2]=8; // a zero-padded fixed-size host buffer is not a complete Facts reply
    CHECK(!mpsReplyHeaderValid(facts.data(),64,3,64));
    facts[2]=17;
    CHECK(!mpsReplyHeaderValid(facts.data(),64,3,64));
    facts[2]=0;
    CHECK(!mpsReplyHeaderValid(facts.data(),64,3,64));
    CHECK(!mpsReplyHeaderValid(nullptr,64,3,64));
    CHECK(!mpsReplyHeaderValid(facts.data(),3,3,64));
    CHECK(!mpsReplyHeaderValid(facts.data(),64,3,0));
    std::array<uint8_t,20> init{};
    init[0]=4; init[2]=5; init[3]=2;
    CHECK(mpsReplyHeaderValid(init.data(),20,2,20));
    init[2]=4;
    CHECK(!mpsReplyHeaderValid(init.data(),20,2,20));
    init[2]=6;
    CHECK(!mpsReplyHeaderValid(init.data(),20,2,20));
    // Exhaust all 16-bit versions: SAS2 accepts exactly 0x0200, not 2.5/2.6.
    for (unsigned version=0;version<=UINT16_MAX;++version)
        CHECK(mpsFactsVersionValid(uint16_t(version))==(version==0x0200));
}

int main() {
    golden(); layouts(); rejection(); bindings(); warpDrive(); identities(); replyHeaders();
    std::printf("SAS2 MPI golden bytes, independent decoder, binding and reply checks: %lu assertions PASS (host only)\n",checks);
}
