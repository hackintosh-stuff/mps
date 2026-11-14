/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2006-2015 LSI Corp.
 * Copyright (c) 2013-2015 Avago Technologies
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Avago Technologies (LSI) MPT-Fusion Host Adapter FreeBSD
 */

#ifndef MPS_DMA_SGL_H
#define MPS_DMA_SGL_H
#include <stdint.h>
#include <stddef.h>
// MPI 2.0 Simple64 and Chain64, FreeBSD mps/mpi/mpi2.h.
// Explicit little endian bytes; all outputs preflighted before modification.
struct MPSDmaSegment { uint64_t address; uint32_t length; };
struct MPSSglResult { uint32_t chainsUsed; uint8_t requestChainOffset; };
static inline void mpsSglStore32(uint8_t *p, uint32_t n)
{ for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(n>>(i*8)); }
static inline void mpsMpiSimple(uint8_t *p, uint64_t address, uint32_t length,
                                bool write, bool terminal=true)
{
    mpsSglStore32(p,length|((uint32_t)(0x12|(write?4:0)|(terminal?0xc1:0))<<24));
    mpsSglStore32(p+4,(uint32_t)address);
    mpsSglStore32(p+8,(uint32_t)(address>>32));
}
static inline void mpsMpiChain(uint8_t *p,uint64_t address,uint16_t length,uint8_t next)
{
    mpsSglStore32(p,(uint32_t)length|((uint32_t)next<<16)|0x32000000U);
    mpsSglStore32(p+4,(uint32_t)address);
    mpsSglStore32(p+8,(uint32_t)(address>>32));
}
// LAST terminates each nonfinal segment, EOB/EOL only the final data SGE.
// Both chain offsets count DWORDs. Frame lengths exclude unused tail bytes.
static inline bool mpsBuildMpiSgl(uint8_t *request,uint32_t requestBytes,
    uint32_t sglOffset,const MPSDmaSegment *segments,uint32_t segmentCount,
    uint32_t expectedBytes,uint8_t *chainStorage,uint64_t chainAddress,
    uint32_t chainFrameBytes,uint32_t chainDepth,uint32_t chainStorageBytes,
    bool write,MPSSglResult *result)
{
    const uint64_t maxAddress=UINT64_C(0x7fffffffffffffff);
    if(!request||!segments||!result||!segmentCount||!expectedBytes||
       requestBytes>4096||sglOffset>requestBytes||(sglOffset&3)||
       requestBytes-sglOffset<12) return false;
    uint64_t total=0;
    for(uint32_t i=0;i<segmentCount;++i) {
        if(!segments[i].length||segments[i].length>0x00ffffffU||
           segments[i].address>maxAddress||
           segments[i].length-1>maxAddress-segments[i].address) return false;
        total+=segments[i].length;
        if(total>expectedBytes) return false;
    }
    if(total!=expectedBytes) return false;
    const uint32_t inlineSlots=(requestBytes-sglOffset)/12;
    uint32_t chainCount=0,chainSlots=0,remaining=0;
    if(segmentCount>inlineSlots) {
        if(!chainStorage||!chainAddress||(chainAddress&3)||(chainFrameBytes&3)||
           chainFrameBytes<24||chainFrameBytes>1024||!chainDepth) return false;
        chainSlots=chainFrameBytes/12;
        if((chainSlots-1)*3>255||
           (sglOffset+(inlineSlots-1)*12)/4>255) return false;
        remaining=segmentCount-(inlineSlots-1);
        uint32_t n=remaining;
        while(n) {
            if(++chainCount>chainDepth||chainCount>chainStorageBytes/chainFrameBytes)
                return false;
            n=n<=chainSlots?0:n-(chainSlots-1);
        }
        uint64_t used=(uint64_t)chainCount*chainFrameBytes;
        if(chainAddress>maxAddress||used-1>maxAddress-chainAddress) return false;
    }
    result->chainsUsed=chainCount; result->requestChainOffset=0;
    uint32_t pos=0,inlineData=chainCount?inlineSlots-1:segmentCount;
    for(;pos<inlineData;++pos) {
        uint8_t *p=request+sglOffset+pos*12;
        mpsMpiSimple(p,segments[pos].address,segments[pos].length,write,
                     pos+1==segmentCount);
        if(chainCount&&pos+1==inlineData) p[3]|=0x80;
    }
    if(!chainCount) return true;
    uint8_t *link=request+sglOffset+inlineData*12;
    result->requestChainOffset=(uint8_t)((sglOffset+inlineData*12)/4);
    for(uint32_t c=0;c<chainCount;++c) {
        bool last=c+1==chainCount;
        uint32_t count=last?remaining:chainSlots-1;
        mpsMpiChain(link,chainAddress+(uint64_t)c*chainFrameBytes,
                    (uint16_t)((count+(last?0:1))*12),last?0:(uint8_t)(count*3));
        uint8_t *frame=chainStorage+c*chainFrameBytes;
        for(uint32_t i=0;i<count;++i,++pos) {
            uint8_t *p=frame+i*12;
            mpsMpiSimple(p,segments[pos].address,segments[pos].length,write,
                         pos+1==segmentCount);
            if(!last&&i+1==count) p[3]|=0x80;
        }
        remaining-=count; link=frame+count*12;
    }
    return true;
}
#endif
