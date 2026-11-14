#ifndef MPS_IDENTITY_H
#define MPS_IDENTITY_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* A model, port address or arbitrary nonzero VPD string is not a disk serial.
 * Be conservative on broken bridges: rejecting publication is safer than
 * reopening an old nub onto a replacement with a generic identity. */
static inline bool mpsSerialIsTrusted(const uint8_t *p,size_t bytes)
{
    if (!p || bytes>128) return false;
    while(bytes && (*p==' ' || *p==0)) {++p;--bytes;}
    while(bytes && (p[bytes-1]==' ' || p[bytes-1]==0)) --bytes;
    if(bytes<4) return false;
    char lower[129]; bool alnum=false,distinct=false;
    for(size_t i=0;i<bytes;++i) {
        if(p[i]<0x21 || p[i]>0x7e) return false;
        alnum|=(p[i]>='0'&&p[i]<='9')||(p[i]>='A'&&p[i]<='Z')||(p[i]>='a'&&p[i]<='z');
        distinct|=p[i]!=p[0];
        lower[i]=(p[i]>='A'&&p[i]<='Z')?(char)(p[i]+('a'-'A')):(char)p[i];
    }
    lower[bytes]=0;
    if(!alnum || !distinct) return false;
    const char *generic[]={"unknown","none","null","n/a","default","serial",
        "serialnumber","notavailable","notspecified","0123456789","1234567890"};
    for(size_t i=0;i<sizeof(generic)/sizeof(generic[0]);++i)
        if(strcmp(lower,generic[i])==0) return false;
    return true;
}

/* Only LU-associated binary EUI/NAA device identifiers qualify. Textual
 * T10 vendor IDs and SCSI name strings can merely repeat vendor/model text.
 * NAA 5/6 identify globally registered devices; locally assigned/port-like
 * forms and the IOC's 0x44332211... SATA placeholder are not stable proof. */
static inline bool mpsVpd83IdentifierTrusted(uint8_t association,uint8_t codeSet,
    uint8_t type,const uint8_t *p,size_t bytes)
{
    if(association || codeSet!=1 || !p || !bytes) return false;
    bool distinct=false,nonzero=false,notff=false;
    for(size_t i=0;i<bytes;++i) {
        distinct|=p[i]!=p[0]; nonzero|=p[i]!=0; notff|=p[i]!=0xff;
    }
    if(!distinct||!nonzero||!notff) return false;
    if(type==3) return (bytes==8 && (p[0]>>4)==5) ||
                       (bytes==16 && (p[0]>>4)==6);
    if(type==2 && (bytes==8 || bytes==12 || bytes==16))
        return (p[0]||p[1]||p[2]) &&
               !(p[0]==0xff&&p[1]==0xff&&p[2]==0xff);
    return false;
}
#endif
