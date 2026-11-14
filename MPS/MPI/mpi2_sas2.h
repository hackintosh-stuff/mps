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

#ifndef MPS_MPI2_SAS2_H
#define MPS_MPI2_SAS2_H
#define MPI2_CONFIG_PAGETYPE_RAID_VOLUME (0x08)
#define MPI2_RAID_VOLUME_PGAD_FORM_GET_NEXT_HANDLE (0x00000000)
#define MPI2_RAID_VOLUME_PGAD_FORM_HANDLE (0x10000000)
#define MPI2_RAID_VOL_STATE_ONLINE (0x03)
#define MPI2_RAID_VOL_STATE_DEGRADED (0x04)
#define MPI2_RAID_VOL_STATE_OPTIMAL (0x05)
#define MPI2_RAIDVOL0_STATUS_FLAG_VOLUME_INACTIVE (0x00000004)
#define MPI2_IOCFACTS_CAPABILITY_EEDP (0x00000040)
#define MPI2_IOCFACTS_CAPABILITY_TLR (0x00000800)
#define MPI2_SCSIIO_CONTROL_TLR_ON (0x00000040)
#define MPI2_SCSIIO_CONTROL_TLR_MASK (0x000000C0)
#define MPI2_SCSIIO_EEDPFLAGS_CHECK_REMOVE_OP (0x0003)
#define MPI2_SCSIIO_EEDPFLAGS_INSERT_OP (0x0004)
#define MPI2_SCSIIO_EEDPFLAGS_INC_PRI_REFTAG (0x8000)
#define MPI2_SCSIIO_EEDPFLAGS_CHECK_REFTAG (0x0400)
#define MPI2_SCSIIO_EEDPFLAGS_CHECK_GUARD (0x0100)
#define MPI2_IOCSTATUS_EEDP_GUARD_ERROR (0x004D)
#define MPI2_IOCSTATUS_EEDP_REF_TAG_ERROR (0x004E)
#define MPI2_IOCSTATUS_EEDP_APP_TAG_ERROR (0x004F)
#define MPI2_FUNCTION_DIAG_BUFFER_POST (0x1D) /* Diagnostic Buffer Post */
#define MPI2_IOCSTATUS_DIAGNOSTIC_RELEASED (0x00A0)
#endif
