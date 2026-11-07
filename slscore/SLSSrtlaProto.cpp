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


#include <string.h>

#include "SLSSrtlaProto.hpp"

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

int sls_srtla_type(const uint8_t *p, int len)
{
    if (NULL == p || len < 2) {
        return -1;
    }
    return rd16(p);
}

bool sls_srt_is_data(const uint8_t *p, int len)
{
    return NULL != p && len >= SLS_SRT_HDR_LEN && 0 == (p[0] & 0x80);
}

uint32_t sls_srt_data_seq(const uint8_t *p)
{
    return rd32(p) & SLS_SRT_SEQ_MASK;
}

int32_t sls_srt_seq_diff(uint32_t a, uint32_t b)
{
    // b - a modulo 2^31, mapped into (-2^30, 2^30]
    uint32_t d = (b - a) & SLS_SRT_SEQ_MASK;
    if (d > 0x40000000u) {
        return (int32_t)(d - 0x80000000u);   // d - 2^31, negative; no signed overflow
    }
    return (int32_t)d;
}

uint32_t sls_srt_seq_add(uint32_t a, int32_t n)
{
    return (a + (uint32_t)n) & SLS_SRT_SEQ_MASK;
}

bool sls_srtla_parse_reg(const uint8_t *p, int len, int type, uint8_t *id_out)
{
    if (SLS_SRTLA_TYPE_REG1 != type && SLS_SRTLA_TYPE_REG2 != type) {
        return false;
    }
    if (NULL == p || SLS_SRTLA_REG_LEN != len || rd16(p) != type) {
        return false;
    }
    if (id_out) {
        memcpy(id_out, p + 2, SLS_SRTLA_ID_LEN);
    }
    return true;
}

int sls_srtla_build_reg(uint8_t *out, int cap, int type, const uint8_t *id)
{
    if (SLS_SRTLA_TYPE_REG1 != type && SLS_SRTLA_TYPE_REG2 != type) {
        return 0;
    }
    if (NULL == out || NULL == id || cap < SLS_SRTLA_REG_LEN) {
        return 0;
    }
    wr16(out, (uint16_t)type);
    memcpy(out + 2, id, SLS_SRTLA_ID_LEN);
    return SLS_SRTLA_REG_LEN;
}

int sls_srtla_build_short(uint8_t *out, int cap, int type)
{
    switch (type) {
    case SLS_SRTLA_TYPE_REG3:
    case SLS_SRTLA_TYPE_REG_ERR:
    case SLS_SRTLA_TYPE_REG_NGP:
    case SLS_SRTLA_TYPE_REG_NAK:
        break;
    default:
        return 0;
    }
    if (NULL == out || cap < SLS_SRTLA_SHORT_LEN) {
        return 0;
    }
    wr16(out, (uint16_t)type);
    return SLS_SRTLA_SHORT_LEN;
}

int sls_srtla_build_ack(uint8_t *out, int cap, const uint32_t *seqs)
{
    if (NULL == out || NULL == seqs || cap < SLS_SRTLA_ACK_LEN) {
        return 0;
    }
    wr32(out, (uint32_t)SLS_SRTLA_TYPE_ACK << 16);
    for (int i = 0; i < SLS_SRTLA_ACK_SEQS; i++) {
        wr32(out + 4 + 4 * i, seqs[i]);
    }
    return SLS_SRTLA_ACK_LEN;
}

bool sls_srtla_parse_ack(const uint8_t *p, int len, uint32_t *seqs_out)
{
    if (NULL == p || SLS_SRTLA_ACK_LEN != len || rd32(p) != ((uint32_t)SLS_SRTLA_TYPE_ACK << 16)) {
        return false;
    }
    if (seqs_out) {
        for (int i = 0; i < SLS_SRTLA_ACK_SEQS; i++) {
            seqs_out[i] = rd32(p + 4 + 4 * i);
        }
    }
    return true;
}

// HSv5 handshake: 16-byte SRT header, then the 48-byte CIF
//   +16 version, +20 encryption(16) | extension flags(16), +24 initial seq,
//   +28 MTU, +32 flow window, +36 handshake type, +40 socket id,
//   +44 SYN cookie, +48 peer IP (16 bytes); extensions from +64:
//   type(16) | length in 32-bit words(16), then the content.
// HSREQ/HSRSP content: SRT version, SRT flags, rcv TSBPD delay(16) | snd TSBPD delay(16).
#define HS_OFF_VERSION   16
#define HS_OFF_TYPE      36
#define HS_OFF_EXT       64
#define HS_VERSION_5     5
#define HS_CONCLUSION    0xFFFFFFFFu
#define HS_EXT_MAX       16      // walk at most this many extensions

bool sls_srt_hs_fields(const uint8_t *p, int len, uint32_t *hs_type, uint32_t *isn, uint32_t *sock_id)
{
    if (NULL == p || len < SLS_SRT_HS_MIN_LEN || rd16(p) != SLS_SRT_TYPE_HANDSHAKE) {
        return false;
    }
    if (hs_type) *hs_type = rd32(p + HS_OFF_TYPE);
    if (isn) *isn = rd32(p + 24) & SLS_SRT_SEQ_MASK;
    if (sock_id) *sock_id = rd32(p + 40);
    return true;
}

bool sls_srt_hs_tsbpd(const uint8_t *p, int len, int *cmd_out, int *rcv_ms, int *snd_ms)
{
    if (NULL == p || len < HS_OFF_EXT || rd16(p) != SLS_SRT_TYPE_HANDSHAKE) {
        return false;
    }
    if (rd32(p + HS_OFF_VERSION) != HS_VERSION_5 || rd32(p + HS_OFF_TYPE) != HS_CONCLUSION) {
        return false;
    }
    int off = HS_OFF_EXT;
    for (int n = 0; n < HS_EXT_MAX && off + 4 <= len; n++) {
        int type = rd16(p + off);
        int words = rd16(p + off + 2);
        int body = off + 4;
        if (words > (len - body) / 4) {
            return false;   // truncated
        }
        if ((SLS_SRT_HS_CMD_HSREQ == type || SLS_SRT_HS_CMD_HSRSP == type) && words >= 3) {
            uint32_t delays = rd32(p + body + 8);
            if (cmd_out) *cmd_out = type;
            if (rcv_ms) *rcv_ms = (int)(delays >> 16);
            if (snd_ms) *snd_ms = (int)(delays & 0xFFFF);
            return true;
        }
        off = body + 4 * words;
    }
    return false;
}

bool sls_srt_hs_reject(const uint8_t *p, int len, int *reason)
{
    if (NULL == p || len < SLS_SRT_HS_MIN_LEN || rd16(p) != SLS_SRT_TYPE_HANDSHAKE) {
        return false;
    }
    // the other types are small (0 wave-a-hand, 1 induction) or negative
    // (conclusion, agreement, done)
    uint32_t type = rd32(p + HS_OFF_TYPE);
    if (type < SLS_SRT_HS_REJECT_BASE || type >= 0x80000000u) {
        return false;
    }
    if (reason) *reason = (int)(type - SLS_SRT_HS_REJECT_BASE);
    return true;
}

bool sls_srt_hs_streamid(const uint8_t *p, int len, char *out, int cap)
{
    if (NULL == out || cap < 1) {
        return false;
    }
    out[0] = '\0';
    if (NULL == p || len < HS_OFF_EXT || rd16(p) != SLS_SRT_TYPE_HANDSHAKE) {
        return false;
    }
    if (rd32(p + HS_OFF_VERSION) != HS_VERSION_5 || rd32(p + HS_OFF_TYPE) != HS_CONCLUSION) {
        return false;
    }
    int off = HS_OFF_EXT;
    for (int n = 0; n < HS_EXT_MAX && off + 4 <= len; n++) {
        int type = rd16(p + off);
        int words = rd16(p + off + 2);
        int body = off + 4;
        if (words > (len - body) / 4) {
            return false;   // truncated
        }
        if (SLS_SRT_HS_CMD_SID == type) {
            int k = 0;
            for (int i = 0; i < 4 * words && k < cap - 1; i++) {
                uint8_t c = p[body + (i & ~3) + 3 - (i & 3)];   // reversed within each word
                if (0 == c) {
                    break;   // padding up to the word
                }
                out[k++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
            }
            out[k] = '\0';
            return k > 0;
        }
        off = body + 4 * words;
    }
    return false;
}

static inline uint64_t rotl64(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }
static inline uint64_t rd64le(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
#define SIPROUND do { \
    v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32); \
    v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0; \
    v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32); } while (0)

uint64_t sls_siphash24(const uint8_t key[16], const uint8_t *msg, size_t len)
{
    uint64_t k0 = rd64le(key), k1 = rd64le(key + 8);
    uint64_t v0 = k0 ^ 0x736f6d6570736575ull;
    uint64_t v1 = k1 ^ 0x646f72616e646f6dull;
    uint64_t v2 = k0 ^ 0x6c7967656e657261ull;
    uint64_t v3 = k1 ^ 0x7465646279746573ull;
    size_t full = len - (len % 8);
    for (size_t i = 0; i < full; i += 8) {
        uint64_t m = rd64le(msg + i);
        v3 ^= m; SIPROUND; SIPROUND; v0 ^= m;
    }
    uint64_t b = ((uint64_t)len) << 56;
    for (size_t i = 0; i < len % 8; i++) {
        b |= ((uint64_t)msg[full + i]) << (8 * i);
    }
    v3 ^= b; SIPROUND; SIPROUND; v0 ^= b;
    v2 ^= 0xff;
    SIPROUND; SIPROUND; SIPROUND; SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}
