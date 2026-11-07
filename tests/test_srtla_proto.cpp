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
#include <string>
#include <vector>
#include "sls_test.hpp"
#include "SLSSrtlaProto.hpp"

static std::vector<uint8_t> reg(int type, uint8_t fill, int len = SLS_SRTLA_REG_LEN)
{
    std::vector<uint8_t> b(len, fill);
    b[0] = (uint8_t)(type >> 8);
    b[1] = (uint8_t)type;
    return b;
}

SLS_TEST(srtla_type)
{
    uint8_t p[2] = {0x92, 0x01};
    SLS_CHECK_EQ(sls_srtla_type(p, 2), 0x9201);
    SLS_CHECK_EQ(sls_srtla_type(p, 1), -1);
    SLS_CHECK_EQ(sls_srtla_type(NULL, 2), -1);
}

SLS_TEST(srtla_parse_reg)
{
    uint8_t id[SLS_SRTLA_ID_LEN];
    std::vector<uint8_t> r1 = reg(SLS_SRTLA_TYPE_REG1, 0xAB);
    SLS_CHECK(sls_srtla_parse_reg(&r1[0], (int)r1.size(), SLS_SRTLA_TYPE_REG1, id));
    SLS_CHECK_EQ(id[0], 0xAB);
    SLS_CHECK_EQ(id[SLS_SRTLA_ID_LEN - 1], 0xAB);
    // the wrong type asked, a wrong length, or not a REG type at all
    SLS_CHECK(!sls_srtla_parse_reg(&r1[0], (int)r1.size(), SLS_SRTLA_TYPE_REG2, id));
    SLS_CHECK(!sls_srtla_parse_reg(&r1[0], (int)r1.size() - 1, SLS_SRTLA_TYPE_REG1, id));
    std::vector<uint8_t> big = reg(SLS_SRTLA_TYPE_REG2, 0, SLS_SRTLA_REG_LEN + 1);
    SLS_CHECK(!sls_srtla_parse_reg(&big[0], (int)big.size(), SLS_SRTLA_TYPE_REG2, id));
    std::vector<uint8_t> r3 = reg(SLS_SRTLA_TYPE_REG3, 0);
    SLS_CHECK(!sls_srtla_parse_reg(&r3[0], (int)r3.size(), SLS_SRTLA_TYPE_REG3, id));
    SLS_CHECK(!sls_srtla_parse_reg(NULL, SLS_SRTLA_REG_LEN, SLS_SRTLA_TYPE_REG1, id));
}

SLS_TEST(srtla_build_reg_roundtrip)
{
    uint8_t id[SLS_SRTLA_ID_LEN];
    for (int i = 0; i < SLS_SRTLA_ID_LEN; i++) id[i] = (uint8_t)i;
    uint8_t out[SLS_SRTLA_REG_LEN + 4];
    memset(out, 0xEE, sizeof(out));
    SLS_CHECK_EQ(sls_srtla_build_reg(out, sizeof(out), SLS_SRTLA_TYPE_REG2, id), SLS_SRTLA_REG_LEN);
    SLS_CHECK_EQ(out[0], 0x92);
    SLS_CHECK_EQ(out[1], 0x01);
    SLS_CHECK_EQ(out[2], 0);
    SLS_CHECK_EQ(out[257], 255);
    SLS_CHECK_EQ(out[258], 0xEE);   // nothing written past the packet
    uint8_t back[SLS_SRTLA_ID_LEN];
    SLS_CHECK(sls_srtla_parse_reg(out, SLS_SRTLA_REG_LEN, SLS_SRTLA_TYPE_REG2, back));
    SLS_CHECK(0 == memcmp(id, back, SLS_SRTLA_ID_LEN));
    SLS_CHECK_EQ(sls_srtla_build_reg(out, SLS_SRTLA_REG_LEN - 1, SLS_SRTLA_TYPE_REG2, id), 0);
    SLS_CHECK_EQ(sls_srtla_build_reg(out, sizeof(out), SLS_SRTLA_TYPE_REG3, id), 0);
}

SLS_TEST(srtla_build_short)
{
    uint8_t out[3] = {0xEE, 0xEE, 0xEE};
    SLS_CHECK_EQ(sls_srtla_build_short(out, 3, SLS_SRTLA_TYPE_REG3), 2);
    SLS_CHECK_EQ(out[0], 0x92);
    SLS_CHECK_EQ(out[1], 0x02);
    SLS_CHECK_EQ(out[2], 0xEE);
    SLS_CHECK_EQ(sls_srtla_build_short(out, 2, SLS_SRTLA_TYPE_REG_ERR), 2);
    SLS_CHECK_EQ(out[1], 0x10);
    SLS_CHECK_EQ(sls_srtla_build_short(out, 2, SLS_SRTLA_TYPE_REG_NGP), 2);
    SLS_CHECK_EQ(out[1], 0x11);
    SLS_CHECK_EQ(sls_srtla_build_short(out, 2, SLS_SRTLA_TYPE_REG_NAK), 2);
    SLS_CHECK_EQ(out[1], 0x12);
    SLS_CHECK_EQ(sls_srtla_build_short(out, 1, SLS_SRTLA_TYPE_REG3), 0);
    SLS_CHECK_EQ(sls_srtla_build_short(out, 2, SLS_SRTLA_TYPE_REG1), 0);
    SLS_CHECK_EQ(sls_srtla_build_short(out, 2, SLS_SRTLA_TYPE_KEEPALIVE), 0);
}

SLS_TEST(srtla_ack_layout)
{
    uint32_t seqs[SLS_SRTLA_ACK_SEQS];
    for (int i = 0; i < SLS_SRTLA_ACK_SEQS; i++) seqs[i] = 0x01020300u + i;
    uint8_t out[SLS_SRTLA_ACK_LEN + 1];
    out[SLS_SRTLA_ACK_LEN] = 0xEE;
    SLS_CHECK_EQ(SLS_SRTLA_ACK_LEN, 44);
    SLS_CHECK_EQ(sls_srtla_build_ack(out, sizeof(out), seqs), 44);
    SLS_CHECK_EQ(out[0], 0x91);
    SLS_CHECK_EQ(out[1], 0x00);
    SLS_CHECK_EQ(out[2], 0x00);
    SLS_CHECK_EQ(out[3], 0x00);
    SLS_CHECK_EQ(out[4], 0x01);
    SLS_CHECK_EQ(out[7], 0x00);
    SLS_CHECK_EQ(out[43], 0x09);
    SLS_CHECK_EQ(out[44], 0xEE);
    uint32_t back[SLS_SRTLA_ACK_SEQS];
    SLS_CHECK(sls_srtla_parse_ack(out, 44, back));
    SLS_CHECK_EQ(back[9], 0x01020309u);
    SLS_CHECK(!sls_srtla_parse_ack(out, 43, back));
    out[2] = 1;
    SLS_CHECK(!sls_srtla_parse_ack(out, 44, back));
    SLS_CHECK_EQ(sls_srtla_build_ack(out, 43, seqs), 0);
}

SLS_TEST(srt_data_seq)
{
    uint8_t d[16] = {0x7F, 0xFF, 0xFF, 0xFE};
    SLS_CHECK(sls_srt_is_data(d, 16));
    SLS_CHECK(!sls_srt_is_data(d, 15));
    SLS_CHECK_EQ(sls_srt_data_seq(d), 0x7FFFFFFEu);
    d[0] = 0x80;
    SLS_CHECK(!sls_srt_is_data(d, 16));   // control bit
}

SLS_TEST(srt_seq_wrap)
{
    SLS_CHECK_EQ(sls_srt_seq_diff(5, 7), 2);
    SLS_CHECK_EQ(sls_srt_seq_diff(7, 5), -2);
    SLS_CHECK_EQ(sls_srt_seq_diff(0x7FFFFFFFu, 0), 1);
    SLS_CHECK_EQ(sls_srt_seq_diff(0, 0x7FFFFFFFu), -1);
    SLS_CHECK_EQ(sls_srt_seq_diff(0x7FFFFFF0u, 0x10), 0x20);
    SLS_CHECK_EQ(sls_srt_seq_diff(0, 0x40000000u), 0x40000000);    // the top of the range
    SLS_CHECK_EQ(sls_srt_seq_diff(0, 0x40000001u), -0x3FFFFFFF);
    SLS_CHECK_EQ(sls_srt_seq_add(0x7FFFFFFFu, 1), 0u);
    SLS_CHECK_EQ(sls_srt_seq_add(0, -1), 0x7FFFFFFFu);
    SLS_CHECK_EQ(sls_srt_seq_add(10, 5), 15u);
}

static void put32(std::vector<uint8_t> &b, int off, uint32_t v)
{
    b[off] = (uint8_t)(v >> 24); b[off + 1] = (uint8_t)(v >> 16); b[off + 2] = (uint8_t)(v >> 8); b[off + 3] = (uint8_t)v;
}

// a v5 conclusion with a KMREQ-like extension first, then HSREQ
static std::vector<uint8_t> conclusion(int ext_cmd, uint16_t rcv, uint16_t snd)
{
    std::vector<uint8_t> b(64 + 4 + 8 + 4 + 12, 0);
    b[0] = 0x80;
    put32(b, 16, 5);
    put32(b, 36, 0xFFFFFFFFu);
    put32(b, 64, (3u << 16) | 2);        // type 3, 2 words
    put32(b, 76, ((uint32_t)ext_cmd << 16) | 3);
    put32(b, 80, 0x00010501);            // SRT version
    put32(b, 84, 0xBF);                  // flags
    put32(b, 88, ((uint32_t)rcv << 16) | snd);
    return b;
}

SLS_TEST(srt_hs_tsbpd)
{
    int cmd = 0, rcv = 0, snd = 0;
    std::vector<uint8_t> h = conclusion(SLS_SRT_HS_CMD_HSREQ, 2000, 120);
    SLS_CHECK(sls_srt_hs_tsbpd(&h[0], (int)h.size(), &cmd, &rcv, &snd));
    SLS_CHECK_EQ(cmd, SLS_SRT_HS_CMD_HSREQ);
    SLS_CHECK_EQ(rcv, 2000);
    SLS_CHECK_EQ(snd, 120);
    h = conclusion(SLS_SRT_HS_CMD_HSRSP, 2500, 0);
    SLS_CHECK(sls_srt_hs_tsbpd(&h[0], (int)h.size(), &cmd, &rcv, &snd));
    SLS_CHECK_EQ(cmd, SLS_SRT_HS_CMD_HSRSP);
    SLS_CHECK_EQ(rcv, 2500);
    // truncated in the middle of the HSREQ
    SLS_CHECK(!sls_srt_hs_tsbpd(&h[0], (int)h.size() - 1, &cmd, &rcv, &snd));
    // an induction (type 1) or v4 handshake has no extensions to read
    put32(h, 36, 1);
    SLS_CHECK(!sls_srt_hs_tsbpd(&h[0], (int)h.size(), &cmd, &rcv, &snd));
    h = conclusion(SLS_SRT_HS_CMD_HSREQ, 1, 1);
    put32(h, 16, 4);
    SLS_CHECK(!sls_srt_hs_tsbpd(&h[0], (int)h.size(), &cmd, &rcv, &snd));
    // no HSREQ/HSRSP at all, and a bare 64-byte handshake
    h = conclusion(7, 1, 1);
    SLS_CHECK(!sls_srt_hs_tsbpd(&h[0], (int)h.size(), &cmd, &rcv, &snd));
    h.resize(64);
    SLS_CHECK(!sls_srt_hs_tsbpd(&h[0], 64, &cmd, &rcv, &snd));
    // an extension claiming more words than the packet has
    h = conclusion(SLS_SRT_HS_CMD_HSREQ, 1, 1);
    put32(h, 64, (3u << 16) | 0xFFFF);
    SLS_CHECK(!sls_srt_hs_tsbpd(&h[0], (int)h.size(), &cmd, &rcv, &snd));
    SLS_CHECK(!sls_srt_hs_tsbpd(NULL, 100, &cmd, &rcv, &snd));
}

SLS_TEST(srt_hs_fields)
{
    std::vector<uint8_t> h = conclusion(SLS_SRT_HS_CMD_HSREQ, 1, 1);
    put32(h, 24, 0x80001234u);   // the top bit is not part of the sequence number
    uint32_t type = 0, isn = 0;
    SLS_CHECK(sls_srt_hs_fields(&h[0], (int)h.size(), &type, &isn));
    SLS_CHECK_EQ(type, SLS_SRT_HS_CONCLUSION);
    SLS_CHECK_EQ(isn, 0x1234u);
    SLS_CHECK(!sls_srt_hs_fields(&h[0], 63, &type, &isn));
    h[1] = 2;   // an ACK, not a handshake
    SLS_CHECK(!sls_srt_hs_fields(&h[0], (int)h.size(), &type, &isn));
}

SLS_TEST(srt_hs_reject)
{
    std::vector<uint8_t> h = conclusion(SLS_SRT_HS_CMD_HSRSP, 1, 1);
    int reason = -1;
    put32(h, 36, 1011);   // what libsrt answers to a plaintext caller of an encrypted listener
    SLS_CHECK(sls_srt_hs_reject(&h[0], (int)h.size(), &reason));
    SLS_CHECK_EQ(reason, 11);
    put32(h, 16, 4);      // any handshake version
    put32(h, 36, 1000 + 2000 + 7);   // an application-defined code
    SLS_CHECK(sls_srt_hs_reject(&h[0], 64, &reason));
    SLS_CHECK_EQ(reason, 2007);
    // the types of a connection that goes ahead
    const uint32_t ok[] = {0u, 1u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFDu, 999u};
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        put32(h, 36, ok[i]);
        SLS_CHECK(!sls_srt_hs_reject(&h[0], (int)h.size(), &reason));
    }
    put32(h, 36, 1011);
    SLS_CHECK(!sls_srt_hs_reject(&h[0], 63, &reason));
    h[1] = 2;   // an ACK
    SLS_CHECK(!sls_srt_hs_reject(&h[0], (int)h.size(), &reason));
    SLS_CHECK(!sls_srt_hs_reject(NULL, 64, &reason));
}

// a v5 conclusion with HSREQ, then an SID extension as libsrt sends it:
// the string in 32-bit words, each word's bytes reversed, NUL-padded
static std::vector<uint8_t> with_sid(const std::string &sid)
{
    std::vector<uint8_t> b = conclusion(SLS_SRT_HS_CMD_HSREQ, 1, 1);
    int words = (int)(sid.size() + 3) / 4;
    size_t at = b.size();
    b.resize(at + 4 + 4 * words, 0);
    put32(b, (int)at, ((uint32_t)SLS_SRT_HS_CMD_SID << 16) | (uint32_t)words);
    for (size_t i = 0; i < sid.size(); i++) {
        b[at + 4 + (i & ~(size_t)3) + 3 - (i & 3)] = (uint8_t)sid[i];
    }
    return b;
}

SLS_TEST(srt_hs_streamid)
{
    char out[64];
    std::vector<uint8_t> h = with_sid("uplive.sls.com/live/irltest");
    SLS_CHECK(sls_srt_hs_streamid(&h[0], (int)h.size(), out, sizeof(out)));
    SLS_CHECK(0 == strcmp(out, "uplive.sls.com/live/irltest"));
    h = with_sid("abcd");   // exactly one word, no padding
    SLS_CHECK(sls_srt_hs_streamid(&h[0], (int)h.size(), out, sizeof(out)));
    SLS_CHECK(0 == strcmp(out, "abcd"));
    // cut to the buffer; non-printable bytes are replaced
    h = with_sid("a\x01" "b\x7f" "cdefgh");
    SLS_CHECK(sls_srt_hs_streamid(&h[0], (int)h.size(), out, 5));
    SLS_CHECK(0 == strcmp(out, "a?b?"));
    // truncated SID, no SID, not a conclusion, v4, empty
    h = with_sid("uplive.x/live/s1");
    SLS_CHECK(!sls_srt_hs_streamid(&h[0], (int)h.size() - 1, out, sizeof(out)));
    SLS_CHECK(0 == strcmp(out, ""));
    std::vector<uint8_t> none = conclusion(SLS_SRT_HS_CMD_HSREQ, 1, 1);
    SLS_CHECK(!sls_srt_hs_streamid(&none[0], (int)none.size(), out, sizeof(out)));
    put32(h, 36, 1);
    SLS_CHECK(!sls_srt_hs_streamid(&h[0], (int)h.size(), out, sizeof(out)));
    h = with_sid("uplive.x/live/s1");
    put32(h, 16, 4);
    SLS_CHECK(!sls_srt_hs_streamid(&h[0], (int)h.size(), out, sizeof(out)));
    h = with_sid("");
    SLS_CHECK(!sls_srt_hs_streamid(&h[0], (int)h.size(), out, sizeof(out)));
    SLS_CHECK(!sls_srt_hs_streamid(NULL, 100, out, sizeof(out)));
    SLS_CHECK(!sls_srt_hs_streamid(&h[0], (int)h.size(), NULL, 10));
}

SLS_TEST(siphash24_vectors)
{
    // the reference vectors: key 00..0f, messages 00, 00 01, ... (SipHash paper, appendix)
    uint8_t key[16], msg[64];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
    for (int i = 0; i < 64; i++) msg[i] = (uint8_t)i;
    SLS_CHECK_EQ(sls_siphash24(key, msg, 0), 0x726fdb47dd0e0e31ull);
    SLS_CHECK_EQ(sls_siphash24(key, msg, 1), 0x74f839c593dc67fdull);
    SLS_CHECK_EQ(sls_siphash24(key, msg, 8), 0x93f5f5799a932462ull);
    SLS_CHECK_EQ(sls_siphash24(key, msg, 15), 0xa129ca6149be45e5ull);
    SLS_CHECK_EQ(sls_siphash24(key, msg, 63), 0x958a324ceb064572ull);
}
