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

/*
 * mpi2_min.h - MPI2.0 system-interface subset for this controller driver.
 *
 * Values and structure layouts are transcribed from FreeBSD
 * sys/dev/mps/mpi/{mpi2.h,mpi2_ioc.h}. Only the registers, bits and messages needed to reach
 * IOC state READY and issue IOC Facts are included. All structures are
 * packed and their sizes are asserted below; do not rely on host ABI types
 * (the original U32 typedef becomes "unsigned long" outside FreeBSD).
 */

#ifndef MPS_MPI2_MIN_H
#define MPS_MPI2_MIN_H

#include <IOKit/IOTypes.h>
#include <stddef.h>

/* System interface register offsets (mpi2.h:226-255, :260-388) */
#define MPI2_DOORBELL_OFFSET                  0x00
#define MPI2_WRITE_SEQUENCE_OFFSET            0x04
#define MPI2_HOST_DIAGNOSTIC_OFFSET           0x08
#define MPI2_DIAG_RW_DATA_OFFSET              0x10
#define MPI2_DIAG_RW_ADDRESS_LOW_OFFSET       0x14
#define MPI2_DIAG_RW_ADDRESS_HIGH_OFFSET      0x18
#define MPI2_HOST_INTERRUPT_STATUS_OFFSET     0x30
#define MPI2_HOST_INTERRUPT_MASK_OFFSET       0x34
#define MPI2_DCR_DATA_OFFSET                  0x38
#define MPI2_DCR_ADDRESS_OFFSET               0x3C
#define MPI2_REPLY_FREE_HOST_INDEX_OFFSET     0x48
#define MPI2_REPLY_POST_HOST_INDEX_OFFSET     0x6C
#define MPI2_HCB_SIZE_OFFSET                  0x74
#define MPI2_HCB_ADDRESS_LOW_OFFSET           0x78
#define MPI2_HCB_ADDRESS_HIGH_OFFSET          0x7C
#define MPI2_SCRATCHPAD0_OFFSET               0xB0
#define MPI2_SCRATCHPAD1_OFFSET               0xB4
#define MPI2_SCRATCHPAD2_OFFSET               0xB8
#define MPI2_SCRATCHPAD3_OFFSET               0xBC
#define MPI2_REQUEST_DESCRIPTOR_POST_LOW_OFFSET  0xC0
#define MPI2_REQUEST_DESCRIPTOR_POST_HIGH_OFFSET 0xC4

/* Doorbell bits (mpi2.h:263-273) */
#define MPI2_DOORBELL_USED                    0x08000000U
#define MPI2_DOORBELL_WHO_INIT_MASK           0x07000000U
#define MPI2_DOORBELL_WHO_INIT_SHIFT          24
#define MPI2_DOORBELL_FAULT_CODE_MASK         0x0000FFFFU
#define MPI2_DOORBELL_DATA_MASK               0x0000FFFFU
#define MPI2_DOORBELL_FUNCTION_MASK           0xFF000000U
#define MPI2_DOORBELL_FUNCTION_SHIFT          24
#define MPI2_DOORBELL_ADD_DWORDS_MASK         0x00FF0000U
#define MPI2_DOORBELL_ADD_DWORDS_SHIFT        16

/* WriteSequence unlock keys, 7 writes (mpi2.h:279-286, mps.c:122) */
#define MPI2_WRSEQ_FLUSH_KEY_VALUE            0x00
#define MPI2_WRSEQ_1ST_KEY_VALUE              0x0F
#define MPI2_WRSEQ_2ND_KEY_VALUE              0x04
#define MPI2_WRSEQ_3RD_KEY_VALUE              0x0B
#define MPI2_WRSEQ_4TH_KEY_VALUE              0x02
#define MPI2_WRSEQ_5TH_KEY_VALUE              0x07
#define MPI2_WRSEQ_6TH_KEY_VALUE              0x0D

/* HostDiagnostic bits (mpi2.h:293-315); note the MPI2_DIAG_ prefix */
#define MPI2_DIAG_BOOT_DEVICE_SELECT_MASK     0x00001800U
#define MPI2_DIAG_HCB_MODE                    0x00000100U
#define MPI2_DIAG_DIAG_WRITE_ENABLE           0x00000080U
#define MPI2_DIAG_RESET_HISTORY               0x00000020U
#define MPI2_DIAG_DIAG_RW_ENABLE              0x00000010U
#define MPI2_DIAG_RESET_ADAPTER               0x00000004U
#define MPI2_DIAG_HOLD_IOC_RESET              0x00000002U
#define MPI2_DIAG_CLEAR_FLASH_BAD_SIG         0x00000400U
#define MPI2_HCB_SIZE_HCB_ENABLE              0x00000001U

/* HostInterruptStatus / Mask bits (mpi2.h:327-343) */
#define MPI2_HIS_SYS2IOC_DB_STATUS            0x80000000U
#define MPI2_HIS_RESET_IRQ_STATUS             0x40000000U
#define MPI2_HIS_REPLY_DESCRIPTOR_INTERRUPT   0x00000008U
#define MPI2_HIS_IOC2SYS_DB_STATUS            0x00000001U
#define MPI2_HIM_REPLY_INT_MASK               0x00000008U
#define MPI2_HIM_IOC2SYS_DB_MASK              0x00000001U

/* IOC states are in Doorbell bits [31:28] (mpi2.h:208-214) */
#define MPI2_IOC_STATE_RESET                  0x00000000U
#define MPI2_IOC_STATE_READY                  0x10000000U
#define MPI2_IOC_STATE_OPERATIONAL            0x20000000U
#define MPI2_IOC_STATE_FAULT                  0x40000000U
#define MPI2_IOC_STATE_MASK                   0xF0000000U
#define MPI2_IOC_STATE_SHIFT                  28

/* MPI function codes (mpi2.h:663-699) */
#define MPI2_FUNCTION_SCSI_IO_REQUEST         0x00
#define MPI2_FUNCTION_SCSI_TASK_MGMT          0x01
#define MPI2_FUNCTION_IOC_INIT                0x02
#define MPI2_FUNCTION_IOC_FACTS               0x03
#define MPI2_FUNCTION_CONFIG                  0x04
#define MPI2_FUNCTION_PORT_ENABLE             0x06
#define MPI2_FUNCTION_EVENT_NOTIFICATION      0x07
#define MPI2_FUNCTION_IOC_MESSAGE_UNIT_RESET  0x40
#define MPI2_FUNCTION_HANDSHAKE               0x42

/* WhoInit (mpi2_ioc.h:258-263) */
#define MPI2_WHOINIT_NOT_READY                0x00
#define MPI2_WHOINIT_ROM_BIOS                 0x02
#define MPI2_WHOINIT_PCI_PEER                 0x03
#define MPI2_WHOINIT_HOST_DRIVER              0x04
#define MPI2_WHOINIT_MANUFACTURER             0x05

/* IOCStatus (mpi2.h:708-724) */
#define MPI2_IOCSTATUS_MASK                   0x7FFF
#define MPI2_IOCSTATUS_SUCCESS                0x0000

/* MPI and header versions (mpi2.h:177, :194-200) */
#define MPI2_VERSION                          0x0200
#define MPI2_HEADER_VERSION                   0x1200

/* IOC Init HostPageSize is a power-of-two exponent (mpsvar.h:66) */
#define MPS_HOST_PAGE_SIZE_4K                 12

/* Default reply layout (mpi2.h:843-858) */
#define MPI2_DEFAULT_REPLY_MSG_LENGTH_OFF     0x02
#define MPI2_DEFAULT_REPLY_FUNCTION_OFF       0x03
#define MPI2_DEFAULT_REPLY_IOCSTATUS_OFF      0x0E

#pragma pack(push, 1)

/* MPI2_IOC_FACTS_REQUEST: 0x0C bytes (mpi2_ioc.h:320-332) */
typedef struct {
    UInt16  Reserved1;      /* 0x00 */
    UInt8   ChainOffset;    /* 0x02 */
    UInt8   Function;       /* 0x03 */
    UInt16  Reserved2;      /* 0x04 */
    UInt8   Reserved3;      /* 0x06 */
    UInt8   MsgFlags;       /* 0x07 */
    UInt8   VP_ID;          /* 0x08 */
    UInt8   VF_ID;          /* 0x09 */
    UInt16  Reserved4;      /* 0x0A */
} MPSIOCFactsRequest;

/* MPI2_IOC_FACTS_REPLY: 0x40 bytes (mpi2_ioc.h:335-378) */
typedef struct {
    UInt16  MsgVersion;                     /* 0x00 */
    UInt8   MsgLength;                      /* 0x02 */
    UInt8   Function;                       /* 0x03 */
    UInt16  HeaderVersion;                  /* 0x04 */
    UInt8   IOCNumber;                      /* 0x06 */
    UInt8   MsgFlags;                       /* 0x07 */
    UInt8   VP_ID;                          /* 0x08 */
    UInt8   VF_ID;                          /* 0x09 */
    UInt16  Reserved1;                      /* 0x0A */
    UInt16  IOCExceptions;                  /* 0x0C */
    UInt16  IOCStatus;                      /* 0x0E */
    UInt32  IOCLogInfo;                     /* 0x10 */
    UInt8   MaxChainDepth;                  /* 0x14 */
    UInt8   WhoInit;                        /* 0x15 */
    UInt8   NumberOfPorts;                  /* 0x16 */
    UInt8   MaxMSIxVectors;                 /* 0x17 */
    UInt16  RequestCredit;                  /* 0x18 */
    UInt16  ProductID;                      /* 0x1A */
    UInt32  IOCCapabilities;                /* 0x1C */
    UInt32  FWVersion;                      /* 0x20 (MPI2_VERSION_UNION.Word) */
    UInt16  IOCRequestFrameSize;            /* 0x24 */
    UInt16  Reserved3;         /* 0x26 */
    UInt16  MaxInitiators;                  /* 0x28 */
    UInt16  MaxTargets;                     /* 0x2A */
    UInt16  MaxSasExpanders;                /* 0x2C */
    UInt16  MaxEnclosures;                  /* 0x2E */
    UInt16  ProtocolFlags;                  /* 0x30 */
    UInt16  HighPriorityCredit;             /* 0x32 */
    UInt16  MaxReplyDescriptorPostQueueDepth; /* 0x34 */
    UInt8   ReplyFrameSize;                 /* 0x36 */
    UInt8   MaxVolumes;                     /* 0x37 */
    UInt16  MaxDevHandle;                   /* 0x38 */
    UInt16  MaxPersistentEntries;           /* 0x3A */
    UInt16  MinDevHandle;                   /* 0x3C */
    UInt16  Reserved4;                      /* 0x3E */
} MPSIOCFactsReply;

/* MPI2_IOC_INIT_REQUEST: 0x48 bytes (mpi2_ioc.h:226-255) */
typedef struct {
    UInt8   WhoInit;                        /* 0x00 */
    UInt8   Reserved1;                      /* 0x01 */
    UInt8   ChainOffset;                    /* 0x02 */
    UInt8   Function;                       /* 0x03 */
    UInt16  Reserved2;                      /* 0x04 */
    UInt8   Reserved3;                      /* 0x06 */
    UInt8   MsgFlags;                       /* 0x07 */
    UInt8   VP_ID;                          /* 0x08 */
    UInt8   VF_ID;                          /* 0x09 */
    UInt16  Reserved4;                      /* 0x0A */
    UInt16  MsgVersion;                     /* 0x0C */
    UInt16  HeaderVersion;                  /* 0x0E */
    UInt32  Reserved5;                      /* 0x10 */
    UInt16  Reserved6;             /* 0x14 */
    UInt8   Reserved7;                   /* 0x16 */
    UInt8   HostMSIxVectors;                /* 0x17 */
    UInt16  Reserved8;                      /* 0x18 */
    UInt16  SystemRequestFrameSize;         /* 0x1A */
    UInt16  ReplyDescriptorPostQueueDepth;  /* 0x1C */
    UInt16  ReplyFreeQueueDepth;            /* 0x1E */
    UInt32  SenseBufferAddressHigh;         /* 0x20 */
    UInt32  SystemReplyAddressHigh;         /* 0x24 */
    UInt64  SystemRequestFrameBaseAddress;  /* 0x28 */
    UInt64  ReplyDescriptorPostQueueAddress;/* 0x30 */
    UInt64  ReplyFreeQueueAddress;          /* 0x38 */
    UInt64  TimeStamp;                      /* 0x40 */
} MPSIOCInitRequest;

/* MPI2_IOC_INIT_REPLY: 0x14 bytes (mpi2_ioc.h:296-313) */
typedef struct {
    UInt8   WhoInit;        /* 0x00 */
    UInt8   Reserved1;      /* 0x01 */
    UInt8   MsgLength;      /* 0x02 */
    UInt8   Function;       /* 0x03 */
    UInt16  Reserved2;      /* 0x04 */
    UInt8   Reserved3;      /* 0x06 */
    UInt8   MsgFlags;       /* 0x07 */
    UInt8   VP_ID;          /* 0x08 */
    UInt8   VF_ID;          /* 0x09 */
    UInt16  Reserved4;      /* 0x0A */
    UInt16  Reserved5;      /* 0x0C */
    UInt16  IOCStatus;      /* 0x0E */
    UInt32  IOCLogInfo;     /* 0x10 */
} MPSIOCInitReply;

#pragma pack(pop)

static_assert(sizeof(MPSIOCFactsRequest) == 0x0C, "IOC Facts request layout");
static_assert(sizeof(MPSIOCFactsReply) == 0x40, "IOC Facts reply layout");
static_assert(offsetof(MPSIOCFactsReply, IOCStatus) == 0x0E, "IOCStatus offset");
static_assert(offsetof(MPSIOCFactsReply, IOCCapabilities) == 0x1C, "IOCCapabilities offset");
static_assert(sizeof(MPSIOCInitRequest) == 0x48, "IOC Init request layout");
static_assert(sizeof(MPSIOCInitReply) == 0x14, "IOC Init reply layout");
static_assert(offsetof(MPSIOCInitRequest, SystemRequestFrameBaseAddress) == 0x28,
              "request frame base offset");
static_assert(offsetof(MPSIOCInitRequest, ReplyDescriptorPostQueueAddress) == 0x30,
              "reply post queue offset");
static_assert(offsetof(MPSIOCInitRequest, ReplyFreeQueueAddress) == 0x38,
              "reply free queue offset");

/* ---------------------------------------------------------------------- */
/* config page request/response over the request/reply queues.       */

/* Reply descriptor flags (mpi2.h:546-557) */
#define MPI2_RPY_DESCRIPT_FLAGS_TYPE_MASK      0x0F
#define MPI2_RPY_DESCRIPT_FLAGS_ADDRESS_REPLY  0x01
#define MPI2_RPY_DESCRIPT_FLAGS_UNUSED         0x0F

/* Request descriptor default type (mpi2.h:421) */
#define MPI2_REQ_DESCRIPT_FLAGS_DEFAULT_TYPE   0x08

/* Config action / page type (mpi2_cnfg.h:336-343, :506-513) */
#define MPI2_CONFIG_ACTION_PAGE_HEADER         0x00
#define MPI2_CONFIG_ACTION_PAGE_READ_CURRENT   0x01
#define MPI2_CONFIG_PAGETYPE_MANUFACTURING     0x09
#define MPI2_MANUFACTURING0_PAGEVERSION        0x00

/* Simple SGE flags (mpi2.h:1068-1078, :1084, :1103) */
#define MPI2_SGE_FLAGS_LAST_ELEMENT            0x80
#define MPI2_SGE_FLAGS_END_OF_BUFFER           0x40
#define MPI2_SGE_FLAGS_SIMPLE_ELEMENT          0x10
#define MPI2_SGE_FLAGS_64_BIT_ADDRESSING       0x02
#define MPI2_SGE_FLAGS_SHIFT                   24
#define MPI2_SGE_LENGTH_MASK                   0x00FFFFFF

#pragma pack(push, 1)

/* MPI2_DEFAULT_REQUEST_DESCRIPTOR: 8 bytes (mpi2.h:404-413) */
typedef struct {
    UInt8   RequestFlags;               /* 0x00 */
    UInt8   MSIxIndex;                  /* 0x01 */
    UInt16  SMID;                       /* 0x02 */
    UInt16  LMID;                       /* 0x04 */
    UInt16  DescriptorTypeDependent;    /* 0x06 */
} MPSDefaultRequestDescriptor;

/* MPI2_CONFIG_PAGE_HEADER: 4 bytes (mpi2_cnfg.h:288-295) */
typedef struct {
    UInt8   PageVersion;    /* 0x00 */
    UInt8   PageLength;     /* 0x01 */
    UInt8   PageNumber;     /* 0x02 */
    UInt8   PageType;       /* 0x03 */
} MPSConfigPageHeader;

/* MPI2_CONFIG_REQUEST: 0x28 bytes (mpi2_cnfg.h:483-503) */
typedef struct {
    UInt8   Action;                 /* 0x00 */
    UInt8   SGLFlags;               /* 0x01 */
    UInt8   ChainOffset;            /* 0x02 */
    UInt8   Function;               /* 0x03 */
    UInt16  ExtPageLength;          /* 0x04 */
    UInt8   ExtPageType;            /* 0x06 */
    UInt8   MsgFlags;               /* 0x07 */
    UInt8   VP_ID;                  /* 0x08 */
    UInt8   VF_ID;                  /* 0x09 */
    UInt16  Reserved1;              /* 0x0A */
    UInt8   Reserved2;              /* 0x0C */
    UInt8   ProxyVF_ID;             /* 0x0D */
    UInt16  Reserved4;              /* 0x0E */
    UInt32  Reserved3;              /* 0x10 */
    MPSConfigPageHeader Header;     /* 0x14 */
    UInt32  PageAddress;            /* 0x18 */
    UInt32  SgeFlagsLength;         /* 0x1C */
    UInt64  SgeAddress;             /* 0x20 */
} MPSConfigRequest;

/* MPI2_CONFIG_REPLY: 0x18 bytes (mpi2_cnfg.h:518-535) */
typedef struct {
    UInt8   Action;                 /* 0x00 */
    UInt8   SGLFlags;               /* 0x01 */
    UInt8   MsgLength;              /* 0x02 */
    UInt8   Function;               /* 0x03 */
    UInt16  ExtPageLength;          /* 0x04 */
    UInt8   ExtPageType;            /* 0x06 */
    UInt8   MsgFlags;               /* 0x07 */
    UInt8   VP_ID;                  /* 0x08 */
    UInt8   VF_ID;                  /* 0x09 */
    UInt16  Reserved1;              /* 0x0A */
    UInt16  Reserved2;              /* 0x0C */
    UInt16  IOCStatus;              /* 0x0E */
    UInt32  IOCLogInfo;             /* 0x10 */
    MPSConfigPageHeader Header;     /* 0x14 */
} MPSConfigReply;

/* MPI2_CONFIG_PAGE_MAN_0: 0x4C bytes (mpi2_cnfg.h:615-625) */
typedef struct {
    MPSConfigPageHeader Header;             /* 0x00 */
    UInt8   ChipName[16];                   /* 0x04 */
    UInt8   ChipRevision[8];                /* 0x14 */
    UInt8   BoardName[16];                  /* 0x1C */
    UInt8   BoardAssembly[16];              /* 0x2C */
    UInt8   BoardTracerNumber[16];          /* 0x3C */
} MPSConfigPageMan0;

#pragma pack(pop)

static_assert(sizeof(MPSDefaultRequestDescriptor) == 0x08, "request descriptor layout");
static_assert(sizeof(MPSConfigPageHeader) == 0x04, "config page header layout");
static_assert(sizeof(MPSConfigRequest) == 0x28, "config request layout");
static_assert(sizeof(MPSConfigReply) == 0x18, "config reply layout");
static_assert(sizeof(MPSConfigPageMan0) == 0x4C, "manufacturing page 0 layout");
static_assert(offsetof(MPSConfigRequest, SgeFlagsLength) == 0x1C, "config SGE offset");
static_assert(offsetof(MPSConfigPageMan0, ChipName) == 0x04, "ChipName offset");

/* ---------------------------------------------------------------------- */
/* EventNotification and PortEnable.                                 */

/* Event numbers (mpi2_ioc.h:581-614) */
#define MPI2_EVENT_STATE_CHANGE                    0x0002
#define MPI2_EVENT_SAS_DEVICE_STATUS_CHANGE        0x000F
#define MPI2_EVENT_SAS_DISCOVERY                   0x0016
#define MPI2_EVENT_SAS_BROADCAST_PRIMITIVE         0x0017
#define MPI2_EVENT_SAS_TOPOLOGY_CHANGE_LIST        0x001C
#define MPI2_EVENT_LOG_ENTRY_ADDED                 0x0021

/* SAS topology PHY reason codes (FreeBSD mpi2_ioc.h). */
#define MPI2_EVENT_SAS_TOPO_RC_MASK                0x0F
#define MPI2_EVENT_SAS_TOPO_RC_TARG_ADDED          0x01
#define MPI2_EVENT_SAS_TOPO_RC_TARG_NOT_RESPONDING 0x02
#define MPI2_EVENT_SAS_TOPO_RC_PHY_CHANGED         0x03
#define MPI2_EVENT_SAS_TOPO_RC_NO_CHANGE           0x04
#define MPI2_EVENT_SAS_TOPO_RC_DELAY_NOT_RESPONDING 0x05
#define MPI2_EVENT_SAS_TOPO_PHYSTATUS_VACANT       0x80

#define MPS_EVENT_MASK_WORDS                       4

#pragma pack(push, 1)

/* MPI2_PORT_ENABLE_REQUEST: 0x0C bytes (mpi2_ioc.h:492-505) */
typedef struct {
    UInt16  Reserved1;      /* 0x00 */
    UInt8   ChainOffset;    /* 0x02 */
    UInt8   Function;       /* 0x03 */
    UInt8   Reserved2;      /* 0x04 */
    UInt8   PortFlags;      /* 0x05 */
    UInt8   Reserved3;      /* 0x06 */
    UInt8   MsgFlags;       /* 0x07 */
    UInt8   VP_ID;          /* 0x08 */
    UInt8   VF_ID;          /* 0x09 */
    UInt16  Reserved4;      /* 0x0A */
} MPSPortEnableRequest;

/* MPI2_PORT_ENABLE_REPLY: 0x14 bytes (mpi2_ioc.h:508-524) */
typedef struct {
    UInt16  Reserved1;      /* 0x00 */
    UInt8   MsgLength;      /* 0x02 */
    UInt8   Function;       /* 0x03 */
    UInt8   Reserved2;      /* 0x04 */
    UInt8   PortFlags;      /* 0x05 */
    UInt8   Reserved3;      /* 0x06 */
    UInt8   MsgFlags;       /* 0x07 */
    UInt8   VP_ID;          /* 0x08 */
    UInt8   VF_ID;          /* 0x09 */
    UInt16  Reserved4;      /* 0x0A */
    UInt16  Reserved5;      /* 0x0C */
    UInt16  IOCStatus;      /* 0x0E */
    UInt32  IOCLogInfo;     /* 0x10 */
} MPSPortEnableReply;

/* MPI2_EVENT_NOTIFICATION_REQUEST: 0x2C bytes (mpi2_ioc.h:533-552) */
typedef struct {
    UInt16  Reserved1;      /* 0x00 */
    UInt8   ChainOffset;    /* 0x02 */
    UInt8   Function;       /* 0x03 */
    UInt16  Reserved2;      /* 0x04 */
    UInt8   Reserved3;      /* 0x06 */
    UInt8   MsgFlags;       /* 0x07 */
    UInt8   VP_ID;          /* 0x08 */
    UInt8   VF_ID;          /* 0x09 */
    UInt16  Reserved4;      /* 0x0A */
    UInt32  Reserved5;      /* 0x0C */
    UInt32  Reserved6;      /* 0x10 */
    UInt32  EventMasks[MPS_EVENT_MASK_WORDS];   /* 0x14 */
    UInt16  SASBroadcastPrimitiveMasks;         /* 0x24 */
    UInt16  Reserved7;            /* 0x26 */
    UInt32  Reserved8;      /* 0x28 */
} MPSEventNotifyRequest;

/* MPI2_EVENT_NOTIFICATION_REPLY header: 0x1C bytes (mpi2_ioc.h:555-574) */
typedef struct {
    UInt16  EventDataLength;    /* 0x00 */
    UInt8   MsgLength;          /* 0x02 */
    UInt8   Function;           /* 0x03 */
    UInt16  Reserved1;          /* 0x04 */
    UInt8   AckRequired;        /* 0x06 */
    UInt8   MsgFlags;           /* 0x07 */
    UInt8   VP_ID;              /* 0x08 */
    UInt8   VF_ID;              /* 0x09 */
    UInt16  Reserved2;          /* 0x0A */
    UInt16  Reserved3;          /* 0x0C */
    UInt16  IOCStatus;          /* 0x0E */
    UInt32  IOCLogInfo;         /* 0x10 */
    UInt16  Event;              /* 0x14 */
    UInt16  Reserved4;          /* 0x16 */
    UInt32  EventContext;       /* 0x18 */
} MPSEventNotifyReply;

/* MPI2_EVENT_DATA_SAS_DISCOVERY: 0x08 bytes */
typedef struct {
    UInt8   Flags;              /* 0x00 */
    UInt8   ReasonCode;         /* 0x01 */
    UInt8   PhysicalPort;       /* 0x02 */
    UInt8   Reserved1;          /* 0x03 */
    UInt32  DiscoveryStatus;    /* 0x04 */
} MPSEventDataSasDiscovery;

#pragma pack(pop)

static_assert(sizeof(MPSPortEnableRequest) == 0x0C, "PortEnable request layout");
static_assert(sizeof(MPSPortEnableReply) == 0x14, "PortEnable reply layout");
static_assert(sizeof(MPSEventNotifyRequest) == 0x2C, "EventNotify request layout");
static_assert(offsetof(MPSEventNotifyReply, Event) == 0x14, "event number offset");
static_assert(offsetof(MPSEventNotifyReply, EventContext) == 0x18, "event context offset");
static_assert(sizeof(MPSEventDataSasDiscovery) == 0x08, "SAS discovery data layout");

/* ---------------------------------------------------------------------- */
/* extended config pages (SAS IO Unit).                              */

/* Extended page type / ext page type (mpi2_cnfg.h:342, :348, :350) */
#define MPI2_CONFIG_PAGETYPE_EXTENDED              0x0F
#define MPI2_CONFIG_EXTPAGETYPE_SAS_IO_UNIT        0x10
#define MPI2_CONFIG_EXTPAGETYPE_SAS_DEVICE         0x12

/* ControllerPhyDeviceInfo bits (mpi2_sas.h:107-114) */
#define MPI2_SAS_DEVICE_INFO_SSP_TARGET            0x00000400
#define MPI2_SAS_DEVICE_INFO_STP_TARGET            0x00000200
#define MPI2_SAS_DEVICE_INFO_SATA_DEVICE           0x00000080
#define MPI2_SAS_DEVICE_INFO_END_DEVICE            0x00000001
#define MPI2_SAS_DEVICE_INFO_DIRECT_ATTACH         0x00000800
/* SCSI Enclosure Processor (SEP/SES/vSES): mpi2_sas.h:103. Such a device is
 * an SSP end device but not a block disk; without this bit it is counted as
 * a standalone target whenever an expander is present. */
#define MPI2_SAS_DEVICE_INFO_SEP                   0x00004000
/* SAS Device Page0 Flags (mpi2_cnfg.h:2762-2763). Fast-path descriptors and
 * IoFlags are only valid when both bits are set. */

#pragma pack(push, 1)

/* MPI2_CONFIG_EXTENDED_PAGE_HEADER: 8 bytes (mpi2_cnfg.h:305-318) */
typedef struct {
    UInt8   PageVersion;    /* 0x00 */
    UInt8   Reserved1;      /* 0x01 */
    UInt8   PageNumber;     /* 0x02 */
    UInt8   PageType;       /* 0x03 */
    UInt16  ExtPageLength;  /* 0x04 */
    UInt8   ExtPageType;    /* 0x06 */
    UInt8   Reserved2;      /* 0x07 */
} MPSExtPageHeader;

/* MPI2_SAS_IO_UNIT0_PHY_DATA: 0x14 bytes (mpi2_cnfg.h:2134-2146) */
typedef struct {
    UInt8   Port;                       /* 0x00 */
    UInt8   PortFlags;                  /* 0x01 */
    UInt8   PhyFlags;                   /* 0x02 */
    UInt8   NegotiatedLinkRate;         /* 0x03 */
    UInt32  ControllerPhyDeviceInfo;    /* 0x04 */
    UInt16  AttachedDevHandle;          /* 0x08 */
    UInt16  ControllerDevHandle;        /* 0x0A */
    UInt32  DiscoveryStatus;            /* 0x0C */
    UInt32  Reserved;                   /* 0x10 */
} MPSSasIOUnit0PhyData;

#pragma pack(pop)

static_assert(sizeof(MPSExtPageHeader) == 0x08, "extended page header layout");
static_assert(offsetof(MPSExtPageHeader, PageNumber) == 0x02, "extended page number");
static_assert(offsetof(MPSExtPageHeader, PageType) == 0x03, "extended page type");
static_assert(offsetof(MPSExtPageHeader, ExtPageType) == 0x06, "extended page subtype");
static_assert(sizeof(MPSSasIOUnit0PhyData) == 0x14, "SAS IO Unit0 phy data layout");

/* SAS Device PageAddress format (mpi2_cnfg.h:396-401) */
#define MPI2_SAS_DEVICE_PGAD_FORM_HANDLE           0x20000000
#define MPI2_SAS_DEVICE_PGAD_HANDLE_MASK           0x0000FFFF

/* ---------------------------------------------------------------------- */
/* SCSI IO request over the request/reply queues.                    */

/* RequestFlags type for SCSI IO (mpi2.h:418) */
#define MPI2_REQ_DESCRIPT_FLAGS_SCSI_IO            0x00

/* ReplyFlags type for a successful SCSI IO without a reply frame (mpi2.h:547) */
#define MPI2_RPY_DESCRIPT_FLAGS_SCSI_IO_SUCCESS    0x00

/* Fast path SCSI IO (mpi2.h:423, :552; mpi2_init.h:353) */

/* Control data direction bits (mpi2_init.h:243-248) */
#define MPI2_SCSIIO_CONTROL_NODATATRANSFER         0x00000000
#define MPI2_SCSIIO_CONTROL_WRITE                  0x01000000
#define MPI2_SCSIIO_CONTROL_READ                   0x02000000

/* Offset of the embedded SGE inside a SCSI IO request frame, in dwords. */
#define MPS_SCSIIO_SGE_DWORDS                      24

#pragma pack(push, 1)

/* MPI2_SCSI_IO_REQUEST: 0x60 bytes (mpi2_init.h:125-160) */
typedef struct {
    UInt16  DevHandle;                      /* 0x00 */
    UInt8   ChainOffset;                    /* 0x02 */
    UInt8   Function;                       /* 0x03 */
    UInt16  Reserved1;                      /* 0x04 */
    UInt8   Reserved2;                      /* 0x06 */
    UInt8   MsgFlags;                       /* 0x07 */
    UInt8   VP_ID;                          /* 0x08 */
    UInt8   VF_ID;                          /* 0x09 */
    UInt16  Reserved3;                      /* 0x0A */
    UInt32  SenseBufferLowAddress;          /* 0x0C */
    UInt16  SGLFlags;                       /* 0x10 */
    UInt8   SenseBufferLength;              /* 0x12 */
    UInt8   Reserved4;                      /* 0x13 */
    UInt8   SGLOffset0;                     /* 0x14 */
    UInt8   SGLOffset1;                     /* 0x15 */
    UInt8   SGLOffset2;                     /* 0x16 */
    UInt8   SGLOffset3;                     /* 0x17 */
    UInt32  SkipCount;                      /* 0x18 */
    UInt32  DataLength;                     /* 0x1C */
    UInt32  BidirectionalDataLength;        /* 0x20 */
    UInt16  IoFlags;                        /* 0x24 */
    UInt16  EEDPFlags;                      /* 0x26 */
    UInt32  EEDPBlockSize;                  /* 0x28 */
    UInt32  SecondaryReferenceTag;          /* 0x2C */
    UInt16  SecondaryApplicationTag;        /* 0x30 */
    UInt16  ApplicationTagTranslationMask;  /* 0x32 */
    UInt8   LUN[8];                         /* 0x34 */
    UInt32  Control;                        /* 0x3C */
    UInt8   CDB[32];                        /* 0x40 */
} MPSScsiIoRequest;

/* MPI2_SCSI_IO_REPLY: 0x30 bytes (mpi2_init.h:382-408) */
typedef struct {
    UInt16  DevHandle;                      /* 0x00 */
    UInt8   MsgLength;                      /* 0x02 */
    UInt8   Function;                       /* 0x03 */
    UInt16  Reserved1;                      /* 0x04 */
    UInt8   Reserved2;                      /* 0x06 */
    UInt8   MsgFlags;                       /* 0x07 */
    UInt8   VP_ID;                          /* 0x08 */
    UInt8   VF_ID;                          /* 0x09 */
    UInt16  Reserved3;                      /* 0x0A */
    UInt8   SCSIStatus;                     /* 0x0C */
    UInt8   SCSIState;                      /* 0x0D */
    UInt16  IOCStatus;                      /* 0x0E */
    UInt32  IOCLogInfo;                     /* 0x10 */
    UInt32  TransferCount;                  /* 0x14 */
    UInt32  SenseCount;                     /* 0x18 */
    UInt32  ResponseInfo;                   /* 0x1C */
    UInt16  TaskTag;                        /* 0x20 */
    UInt16  Reserved4;                      /* 0x22 */
    UInt32  BidirectionalTransferCount;     /* 0x24 */
    UInt32  Reserved5;                      /* 0x28 */
    UInt32  Reserved6;                      /* 0x2C */
} MPSScsiIoReply;

#pragma pack(pop)

static_assert(sizeof(MPSScsiIoRequest) == 0x60, "SCSI IO request layout");
static_assert(sizeof(MPSScsiIoReply) == 0x30, "SCSI IO reply layout");

/* MPI2 SCSI error reply is 0x30 bytes, including reserved tail. */
#define MPS_SCSIIO_REPLY_MIN_BYTES 0x30U
static_assert(offsetof(MPSScsiIoRequest, CDB) == 0x40, "SCSI IO CDB offset");
static_assert(offsetof(MPSScsiIoRequest, Control) == 0x3C, "SCSI IO control offset");

/* SAS Device Page 0 field offsets (mpi2_cnfg.h:2708-2732) */
#define MPS_SASDEV0_SLOT_OFF                       0x08
#define MPS_SASDEV0_ENCLOSURE_OFF                  0x0A
#define MPS_SASDEV0_SASADDRESS_OFF                 0x0C
#define MPS_SASDEV0_PARENT_OFF                     0x14
#define MPS_SASDEV0_PHYNUM_OFF                     0x16
#define MPS_SASDEV0_DEVHANDLE_OFF                  0x18
#define MPS_SASDEV0_DEVICEINFO_OFF                 0x1C

/* ---------------------------------------------------------------------- */
/* task management, SCSI status/state and fixed-format sense data.     */
/* Layouts and values transcribed from FreeBSD                             */
/* sys/dev/mps/mpi/{mpi2_init.h,mpi2.h} at the reference commit in         */
/* PLAN.md section 2.                                                      */

/* SAM status codes (mpi2_init.h:415-427) */
#define MPI2_SCSI_STATUS_GOOD                      0x00
#define MPI2_IOCSTATUS_SCSI_RECOVERED_ERROR         0x0040
#define MPI2_IOCSTATUS_SCSI_DATA_OVERRUN            0x0044
#define MPI2_IOCSTATUS_SCSI_DATA_UNDERRUN           0x0045
#define MPI2_SCSI_STATUS_CHECK_CONDITION           0x02
#define MPI2_SCSI_STATUS_BUSY                      0x08
#define MPI2_SCSI_STATUS_RESERVATION_CONFLICT      0x18
#define MPI2_SCSI_STATUS_TASK_SET_FULL             0x28
#define MPI2_SCSI_STATUS_TASK_ABORTED              0x40

/* SCSIState bits (mpi2_init.h:429-435) */
#define MPI2_SCSI_STATE_AUTOSENSE_VALID            0x01
#define MPI2_SCSI_STATE_AUTOSENSE_FAILED           0x02
#define MPI2_SCSI_STATE_NO_SCSI_STATUS             0x04
#define MPI2_SCSI_STATE_TERMINATED                 0x08
#define MPI2_SCSI_STATE_RESPONSE_INFO_VALID        0x10
#define MPI2_SCSI_RI_MASK_REASONCODE               0x000000FFU

/* TaskType values (mpi2_init.h:470-480) */
#define MPI2_SCSITASKMGMT_TASKTYPE_ABORT_TASK      0x01
#define MPI2_SCSITASKMGMT_TASKTYPE_TARGET_RESET    0x03
#define MPI2_SCSITASKMGMT_TASKTYPE_LUN_RESET       0x05
#define MPI2_SCSITASKMGMT_MSGFLAGS_LINK_RESET       0x00

/* TM ResponseCode values (mpi2_init.h:516-524) */
#define MPI2_SCSITASKMGMT_RSP_TM_COMPLETE          0x00
#define MPI2_SCSITASKMGMT_RSP_TM_SUCCEEDED         0x08

/* Fixed-format sense keys (SPC-4) */
#define MPS_SENSE_KEY_NO_SENSE                     0x00
#define MPS_SENSE_KEY_RECOVERED_ERROR              0x01
#define MPS_SENSE_KEY_NOT_READY                    0x02
#define MPS_SENSE_KEY_MEDIUM_ERROR                 0x03
#define MPS_SENSE_KEY_HARDWARE_ERROR               0x04
#define MPS_SENSE_KEY_ILLEGAL_REQUEST              0x05
#define MPS_SENSE_KEY_UNIT_ATTENTION               0x06
#define MPS_SENSE_KEY_DATA_PROTECT                 0x07
#define MPS_SENSE_KEY_BLANK_CHECK                  0x08
#define MPS_SENSE_KEY_ABORTED_COMMAND              0x0B

#pragma pack(push, 1)

/* MPI2_SCSI_TASK_MANAGE_REQUEST: 0x34 bytes (mpi2_init.h:449-468) */
typedef struct {
    UInt16  DevHandle;              /* 0x00 */
    UInt8   ChainOffset;            /* 0x02 */
    UInt8   Function;               /* 0x03 */
    UInt8   Reserved1;              /* 0x04 */
    UInt8   TaskType;               /* 0x05 */
    UInt8   Reserved2;              /* 0x06 */
    UInt8   MsgFlags;               /* 0x07 */
    UInt8   VP_ID;                  /* 0x08 */
    UInt8   VF_ID;                  /* 0x09 */
    UInt16  Reserved3;              /* 0x0A */
    UInt8   LUN[8];                 /* 0x0C */
    UInt32  Reserved4[7];           /* 0x14 */
    UInt16  TaskMID;                /* 0x30 */
    UInt16  Reserved5;              /* 0x32 */
} MPSScsiTaskMgmtRequest;

/* MPI2_SCSI_TASK_MANAGE_REPLY: 0x1C bytes (mpi2_init.h:495-514) */
typedef struct {
    UInt16  DevHandle;              /* 0x00 */
    UInt8   MsgLength;              /* 0x02 */
    UInt8   Function;               /* 0x03 */
    UInt8   ResponseCode;           /* 0x04 */
    UInt8   TaskType;               /* 0x05 */
    UInt8   Reserved1;              /* 0x06 */
    UInt8   MsgFlags;               /* 0x07 */
    UInt8   VP_ID;                  /* 0x08 */
    UInt8   VF_ID;                  /* 0x09 */
    UInt16  Reserved2;              /* 0x0A */
    UInt16  Reserved3;              /* 0x0C */
    UInt16  IOCStatus;              /* 0x0E */
    UInt32  IOCLogInfo;             /* 0x10 */
    UInt32  TerminationCount;       /* 0x14 */
    UInt32  ResponseInfo;           /* 0x18 */
} MPSScsiTaskMgmtReply;

#pragma pack(pop)

static_assert(sizeof(MPSScsiTaskMgmtRequest) == 0x34, "TM request layout");
static_assert(sizeof(MPSScsiTaskMgmtReply) == 0x1C, "TM reply layout");
static_assert(offsetof(MPSScsiTaskMgmtRequest, TaskMID) == 0x30,
              "TM TaskMID offset");
static_assert(offsetof(MPSScsiTaskMgmtReply, TerminationCount) == 0x14,
              "TM TerminationCount offset");

/* ---------------------------------------------------------------------- */
/* High-priority task-management descriptor, EventAck and event-mask constants. */

/* mpi2.h:420-421 */
#define MPI2_REQ_DESCRIPT_FLAGS_HIGH_PRIORITY      0x06
/* mpi2.h:671 */
#define MPI2_FUNCTION_EVENT_ACK                    0x08
/* mpi2_ioc.h:577-578: 1 in an EventMasks bit DISABLES that event */
#define MPI2_EVENT_NOTIFICATION_ACK_REQUIRED       0x01

#pragma pack(push, 1)

/* MPI2_EVENT_ACK_REQUEST: 0x14 bytes (mpi2_ioc.h:1378-1393) */
typedef struct {
    UInt16  Reserved1;              /* 0x00 */
    UInt8   ChainOffset;            /* 0x02 */
    UInt8   Function;               /* 0x03 */
    UInt16  Reserved2;              /* 0x04 */
    UInt8   Reserved3;              /* 0x06 */
    UInt8   MsgFlags;               /* 0x07 */
    UInt8   VP_ID;                  /* 0x08 */
    UInt8   VF_ID;                  /* 0x09 */
    UInt16  Reserved4;              /* 0x0A */
    UInt16  Event;                  /* 0x0C */
    UInt16  Reserved5;              /* 0x0E */
    UInt32  EventContext;           /* 0x10 */
} MPSEventAckRequest;

#pragma pack(pop)

static_assert(sizeof(MPSEventAckRequest) == 0x14, "EventAck request layout");
static_assert(offsetof(MPSEventAckRequest, Event) == 0x0C, "EventAck Event offset");
static_assert(offsetof(MPSEventAckRequest, EventContext) == 0x10,
              "EventAck EventContext offset");

#endif /* MPS_MPI2_MIN_H */

#define MPI2_IOCFACTS_CAPABILITY_EVENT_REPLAY 0x00002000U
#define MPI2_SGE_FLAGS_END_OF_LIST 0x01
#define MPI2_SGE_FLAGS_HOST_TO_IOC 0x04
