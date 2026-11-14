/*-
 *  Copyright 2000-2020 Broadcom Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
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
 * Broadcom Inc. (LSI) MPT-Fusion Host Adapter FreeBSD
 */

/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2006-2015 LSI Corp.
 * Copyright (c) 2007-2015 LSI Corp.
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
#ifndef MPS_MPI2_MGMT_H
#define MPS_MPI2_MGMT_H
#include <stdint.h>
#include <stddef.h>

/* SAS2 wire subset from sys/dev/mps/mpi/{mpi2.h,mpi2_ioc.h,mpi2_sas.h,
 * mpi2_tool.h}. The 0x14 firmware prefix is followed by this transaction
 * context, then a legacy MPI Simple64. No caller DMA pointer is accepted. */
enum {
    kMPSFirmwareContextOffset = 0x14,
    kMPSFirmwareSgeOffset = 0x24,
    kMPSManagementSimpleBytes = 12
};
#pragma pack(push, 1)
struct MPSFirmwareTransactionContext {
    uint8_t Reserved1, ContextSize, DetailsLength, Flags;
    uint32_t Reserved2, ImageOffset, ImageSize;
};
struct MPSManagementSimple64 {
    uint32_t FlagsLength;
    uint64_t Address;
};
#pragma pack(pop)
static_assert(sizeof(MPSFirmwareTransactionContext) == 16, "firmware context size");
static_assert(offsetof(MPSFirmwareTransactionContext, ImageOffset) == 8, "image offset position");
static_assert(offsetof(MPSFirmwareTransactionContext, ImageSize) == 12, "image size position");
static_assert(sizeof(MPSManagementSimple64) == 12, "legacy MPI Simple64 size");
static_assert(offsetof(MPSManagementSimple64, Address) == 4, "legacy MPI address position");

static inline void mpsManagementPut32(uint8_t *p, uint32_t n)
{ for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(n >> (i * 8)); }

/* Nonterminal is the SMP outgoing buffer: EOB but no LAST or EOL. The final
 * incoming element terminates the complete list. Validate before touching it. */
static inline bool mpsManagementSimple64(uint8_t *p, uint64_t address,
    uint32_t bytes, bool write, bool terminal)
{
    if (!p || bytes > 0x00ffffffU || (bytes && bytes - 1 > UINT64_MAX - address)) return false;
    uint8_t flags = uint8_t(0x12U | 0x40U | (write ? 4U : 0U) | (terminal ? 0x81U : 0U));
    mpsManagementPut32(p, bytes | (uint32_t(flags) << 24));
    for (unsigned i = 0; i < 8; ++i) p[i + 4] = uint8_t(address >> (i * 8));
    return true;
}
static inline void mpsManagementFirmwareContext(uint8_t *p, uint32_t offset, uint32_t bytes)
{
    for (unsigned i = 0; i < 16; ++i) p[i] = 0;
    p[2] = 12; // ContextSize=0; DetailsLength includes reserved word and image range.
    mpsManagementPut32(p + 8, offset);
    mpsManagementPut32(p + 12, bytes);
}
#ifndef MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH
#define MPI2_FUNCTION_RAID_SCSI_IO_PASSTHROUGH (0x16U)
#endif
#ifndef MPI2_FUNCTION_PORT_FACTS
#define MPI2_FUNCTION_PORT_FACTS (0x05U)
#endif
#ifndef MPI2_FUNCTION_FW_DOWNLOAD
#define MPI2_FUNCTION_FW_DOWNLOAD (0x09U)
#endif
#ifndef MPI2_FUNCTION_FW_UPLOAD
#define MPI2_FUNCTION_FW_UPLOAD (0x12U)
#endif
#ifndef MPI2_FUNCTION_RAID_ACTION
#define MPI2_FUNCTION_RAID_ACTION (0x15U)
#endif
#ifndef MPI2_FUNCTION_TOOLBOX
#define MPI2_FUNCTION_TOOLBOX (0x17U)
#endif
#ifndef MPI2_FUNCTION_SCSI_ENCLOSURE_PROCESSOR
#define MPI2_FUNCTION_SCSI_ENCLOSURE_PROCESSOR (0x18U)
#endif
#ifndef MPI2_FUNCTION_SMP_PASSTHROUGH
#define MPI2_FUNCTION_SMP_PASSTHROUGH (0x1AU)
#endif
#ifndef MPI2_FUNCTION_DIAG_BUFFER_POST
#define MPI2_FUNCTION_DIAG_BUFFER_POST (0x1DU)
#endif
#ifndef MPI2_FUNCTION_DIAG_RELEASE
#define MPI2_FUNCTION_DIAG_RELEASE (0x1EU)
#endif
#ifndef MPI2_IOCSTATUS_DIAGNOSTIC_RELEASED
#define MPI2_IOCSTATUS_DIAGNOSTIC_RELEASED (0x00A0U)
#endif
#ifndef MPI2_RAID_ACTION_SYSTEM_SHUTDOWN_INITIATED
#define MPI2_RAID_ACTION_SYSTEM_SHUTDOWN_INITIATED (0x20U)
#endif
#ifndef MPI2_IOCFACTS_CAPABILITY_DIAG_TRACE_BUFFER
#define MPI2_IOCFACTS_CAPABILITY_DIAG_TRACE_BUFFER (0x00000008U)
#endif
#ifndef MPI2_IOCFACTS_CAPABILITY_SNAPSHOT_BUFFER
#define MPI2_IOCFACTS_CAPABILITY_SNAPSHOT_BUFFER (0x00000010U)
#endif
#ifndef MPI2_IOCFACTS_CAPABILITY_EXTENDED_BUFFER
#define MPI2_IOCFACTS_CAPABILITY_EXTENDED_BUFFER (0x00000020U)
#endif
#ifndef MPI2_TOOLBOX_DIAGNOSTIC_CLI_TOOL
#define MPI2_TOOLBOX_DIAGNOSTIC_CLI_TOOL (0x06U)
#endif

#endif
