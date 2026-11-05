/**
 * The MIT License (MIT)
 *
 * Copyright (c) 2019-2020 Edward.Wu
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


#ifndef _SLSFrontDoorProto_INCLUDE_
#define _SLSFrontDoorProto_INCLUDE_

#include <stdint.h>

/**
 * Wire facts the front door needs to tell plain SRT from SRTLA (srtla2) on
 * one UDP port. All fields are big-endian. SRTLA types use the SRT control
 * bit with control types SRT does not define, so the first packet from a
 * source decides what it is.
 */

#define SLS_SRTLA_ID_LEN     256
#define SLS_SRTLA_REG_LEN    258     // 2-byte type + id (REG1, REG2)
#define SLS_SRT_HDR_LEN      16      // minimum SRT packet
#define SLS_SRT_HS_MIN_LEN   64      // 16-byte header + 48-byte handshake CIF
#define SLS_UDP_MTU          1500

#define SLS_SRT_TYPE_HANDSHAKE     0x8000
#define SLS_SRTLA_TYPE_KEEPALIVE   0x9000
#define SLS_SRTLA_TYPE_ACK         0x9100
#define SLS_SRTLA_TYPE_REG1        0x9200
#define SLS_SRTLA_TYPE_REG2        0x9201
#define SLS_SRTLA_TYPE_REG3        0x9202
#define SLS_SRTLA_TYPE_REG_ERR     0x9210
#define SLS_SRTLA_TYPE_REG_NGP     0x9211
#define SLS_SRTLA_TYPE_REG_NAK     0x9212
#define SLS_SRTLA_TYPE_FIRST       0x9000   // the SRTLA range, inclusive
#define SLS_SRTLA_TYPE_LAST        0x92FF

enum sls_fd_kind_t {
    SLS_FD_SRTLA_REG1,      // a new group (exactly SLS_SRTLA_REG_LEN bytes)
    SLS_FD_SRTLA_REG2,      // a link joining a group (exactly SLS_SRTLA_REG_LEN bytes)
    SLS_FD_SRTLA_OTHER,     // any other packet with a type in the SRTLA range
    SLS_FD_SRT_HANDSHAKE,   // SRT control type 0x8000, at least SLS_SRT_HS_MIN_LEN bytes
    SLS_FD_SRT_OTHER,       // any other packet of at least SLS_SRT_HDR_LEN bytes
    SLS_FD_JUNK,            // too short, or REG1/REG2 of the wrong length
};

/**
 * Classifies one datagram from its first bytes. Pure; never reads past len
 * (NULL or len < 2 is JUNK).
 */
sls_fd_kind_t sls_fd_classify(const uint8_t *p, int len);

const char *sls_fd_kind_name(sls_fd_kind_t kind);

#endif
