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


#ifndef _SLSSrtlaProto_INCLUDE_
#define _SLSSrtlaProto_INCLUDE_

#include <stdint.h>
#include <stddef.h>

#include "SLSFrontDoorProto.hpp"

/**
 * srtla2 messages and the few SRT header fields the SRTLA receiver needs.
 * Pure functions: no state, no allocation, never read or write past the
 * lengths given. All fields are big-endian. Written from the protocol facts
 * in the 1.6.0 spec.
 */

#define SLS_SRTLA_ACK_SEQS      10                          // sequence numbers per SRTLA ACK
#define SLS_SRTLA_ACK_LEN       (4 + 4 * SLS_SRTLA_ACK_SEQS) // 4-byte header + the sequence numbers
#define SLS_SRTLA_SHORT_LEN     2                           // REG3, REG_ERR, REG_NGP, REG_NAK
#define SLS_SRTLA_ID_HALF       (SLS_SRTLA_ID_LEN / 2)      // the sender's half; the receiver fills the rest

#define SLS_SRT_TYPE_ACK        0x8002
#define SLS_SRT_TYPE_NAK        0x8003
#define SLS_SRT_TYPE_SHUTDOWN   0x8005
#define SLS_SRT_SEQ_MASK        0x7FFFFFFFu                 // 31-bit data sequence numbers

// SRT handshake extensions (HSv5 conclusion)
#define SLS_SRT_HS_CMD_HSREQ    1
#define SLS_SRT_HS_CMD_HSRSP    2

/** The 16-bit type of a packet (SRT control type with the control bit, or an SRTLA type); -1 if len < 2. */
int sls_srtla_type(const uint8_t *p, int len);

/** True for an SRT data packet: at least the SRT header, control bit clear. */
bool sls_srt_is_data(const uint8_t *p, int len);

/** The 31-bit sequence number of an SRT data packet (check sls_srt_is_data first). */
uint32_t sls_srt_data_seq(const uint8_t *p);

/**
 * Signed distance b - a between two 31-bit sequence numbers, in
 * (-2^30, 2^30]: positive when b is after a, taking wrap-around into account.
 */
int32_t sls_srt_seq_diff(uint32_t a, uint32_t b);

/** a + n in 31-bit sequence space (n may be negative). */
uint32_t sls_srt_seq_add(uint32_t a, int32_t n);

/**
 * REG1/REG2: copies the 256-byte id out of a packet of exactly
 * SLS_SRTLA_REG_LEN bytes with the given type. Returns false otherwise.
 */
bool sls_srtla_parse_reg(const uint8_t *p, int len, int type, uint8_t *id_out);

/**
 * Writes a REG1/REG2 packet (type + id) into out (cap bytes). Returns
 * SLS_SRTLA_REG_LEN, or 0 if cap is too small or type is not REG1/REG2.
 */
int sls_srtla_build_reg(uint8_t *out, int cap, int type, const uint8_t *id);

/**
 * Writes a 2-byte message (REG3, REG_ERR, REG_NGP, REG_NAK). Returns
 * SLS_SRTLA_SHORT_LEN, or 0 if cap is too small or type is not one of those.
 */
int sls_srtla_build_short(uint8_t *out, int cap, int type);

/**
 * Writes an SRTLA ACK: the 32-bit header 0x91000000 and SLS_SRTLA_ACK_SEQS
 * sequence numbers. Returns SLS_SRTLA_ACK_LEN, or 0 if cap is too small.
 */
int sls_srtla_build_ack(uint8_t *out, int cap, const uint32_t *seqs);

/**
 * Reads an SRTLA ACK back (for tests and the test sender). Returns false
 * unless the packet is exactly SLS_SRTLA_ACK_LEN bytes with the ACK header.
 */
bool sls_srtla_parse_ack(const uint8_t *p, int len, uint32_t *seqs_out);

/**
 * TSBPD delays from an SRT HSv5 conclusion handshake carrying an HSREQ or
 * HSRSP extension (plaintext). cmd_out gets SLS_SRT_HS_CMD_HSREQ or _HSRSP,
 * rcv_ms/snd_ms the receiver and sender TSBPD delays the packet declares.
 * Returns false for anything else (not a handshake, not v5 conclusion, no
 * such extension, or a truncated extension).
 */
bool sls_srt_hs_tsbpd(const uint8_t *p, int len, int *cmd_out, int *rcv_ms, int *snd_ms);

#endif
