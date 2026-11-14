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

/*-
 * Copyright 2000-2020 Broadcom Inc. All rights reserved.
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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#ifndef MPS_MPI2_TARGET_H
#define MPS_MPI2_TARGET_H

/* MPI 2.0 layouts/constants from FreeBSD sys/dev/mps/mpi. */
#include "mpi2_min.h"

#define MPI2_FUNCTION_SAS_IO_UNIT_CONTROL          0x1B
#define MPI2_FUNCTION_SATA_PASSTHROUGH             0x1C
#define MPI2_IOCSTATUS_CONFIG_INVALID_PAGE         0x0022
#define MPI2_IOCFACTS_CAPABILITY_INTEGRATED_RAID    0x00001000
#define MPI2_CONFIG_EXTPAGETYPE_RAID_CONFIG        0x16
#define MPI2_SAS_DEVICE_PGAD_FORM_GET_NEXT_HANDLE   0x00000000
#define MPI2_RAID_PGAD_FORM_GET_NEXT_CONFIGNUM      0x00000000
#define MPI2_SAS_DEVICE0_ASTATUS_NO_ERRORS         0x00
/* Firmware reports this for a just-attached SATA disk; mpt3sas treats it as
 * usable (mpt3sas_scsih.c _scsih_get_sas_device_status) instead of rejecting
 * the device, and the disk serves I/O from that state. */
#define MPI2_SAS_DEVICE0_ASTATUS_SATA_NEEDS_INITIALIZATION 0x04
#define MPI2_SAS_DEVICE0_FLAGS_DEVICE_PRESENT      0x0001
#define MPI2_SAS_DEVICE0_FLAGS_SATA_FUA_SUPPORTED  0x0010
#define MPI2_SAS_DEVICE0_FLAGS_UNSUPPORTED_DEVICE  0x0100
#define MPI2_SAS_DEVICE0_FLAGS_UNAUTHORIZED_DEVICE 0x8000
#define MPI2_SAS_DEVICE_INFO_ATAPI_DEVICE          0x00002000
#define MPI2_EVENT_IR_VOLUME                      0x001E
#define MPI2_EVENT_IR_PHYSICAL_DISK                0x001F
#define MPI2_EVENT_IR_CONFIGURATION_CHANGE_LIST    0x0020
#define MPI2_SAS_OP_REMOVE_DEVICE                  0x0D
#define MPI2_SATA_PT_REQ_PT_FLAGS_PIO              0x0010
#define MPI2_SATA_PT_REQ_PT_FLAGS_READ             0x0001

#pragma pack(push, 1)
struct MPSSataPassthroughRequest {
    UInt16 DevHandle;
    UInt8 ChainOffset;
    UInt8 Function;
    UInt16 PassthroughFlags;
    UInt8 SGLFlags;
    UInt8 MsgFlags;
    UInt8 VP_ID;
    UInt8 VF_ID;
    UInt16 Reserved1;
    UInt32 Reserved2[3];
    UInt32 DataLength;
    UInt8 CommandFIS[20];
    UInt32 SgeFlagsLength;
    UInt64 SgeAddress;
};
struct MPSSataPassthroughReply {
    UInt16 DevHandle;
    UInt8 MsgLength;
    UInt8 Function;
    UInt16 PassthroughFlags;
    UInt8 SGLFlags;
    UInt8 MsgFlags;
    UInt8 VP_ID;
    UInt8 VF_ID;
    UInt16 Reserved1;
    UInt8 Reserved2;
    UInt8 SASStatus;
    UInt16 IOCStatus;
    UInt32 IOCLogInfo;
    UInt8 StatusFIS[20];
    UInt32 StatusControlRegisters;
    UInt32 TransferCount;
};
struct MPSSasIoUnitControlRequest {
    UInt8 Operation;
    UInt8 Reserved1;
    UInt8 ChainOffset;
    UInt8 Function;
    UInt16 DevHandle;
    UInt8 IOCParameter;
    UInt8 MsgFlags;
    UInt8 VP_ID;
    UInt8 VF_ID;
    UInt16 Reserved3;
    UInt16 Reserved4;
    UInt8 PhyNum;
    UInt8 PrimFlags;
    UInt32 Primitive;
    UInt8 LookupMethod;
    UInt8 Reserved5;
    UInt16 SlotNumber;
    UInt64 LookupAddress;
    UInt32 IOCParameterValue;
    UInt32 Reserved7;
    UInt32 Reserved8;
};
struct MPSSasIoUnitControlReply {
    UInt8 Operation;
    UInt8 Reserved1;
    UInt8 MsgLength;
    UInt8 Function;
    UInt16 DevHandle;
    UInt8 IOCParameter;
    UInt8 MsgFlags;
    UInt8 VP_ID;
    UInt8 VF_ID;
    UInt16 Reserved3;
    UInt16 Reserved4;
    UInt16 IOCStatus;
    UInt32 IOCLogInfo;
};
#pragma pack(pop)

static_assert(sizeof(MPSSataPassthroughRequest) == 0x3c, "SATA request layout");
static_assert(offsetof(MPSSataPassthroughRequest, SgeFlagsLength) == 0x30,
              "SATA MPI SGE offset");
static_assert(sizeof(MPSSataPassthroughReply) == 0x30, "SATA reply layout");
static_assert(sizeof(MPSSasIoUnitControlRequest) == 0x2C, "SAS control request");
static_assert(sizeof(MPSSasIoUnitControlReply) == 0x14, "SAS control reply");

#endif
