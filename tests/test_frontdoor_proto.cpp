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
#include <vector>
#include "sls_test.hpp"
#include "SLSFrontDoorProto.hpp"

static sls_fd_kind_t classify(uint8_t b0, uint8_t b1, int len)
{
    std::vector<uint8_t> buf(len > 0 ? len : 1, 0);
    if (len >= 1) buf[0] = b0;
    if (len >= 2) buf[1] = b1;
    return sls_fd_classify(&buf[0], len);
}

SLS_TEST(proto_reg1)
{
    SLS_CHECK_EQ(classify(0x92, 0x00, 258), SLS_FD_SRTLA_REG1);
    SLS_CHECK_EQ(classify(0x92, 0x00, 257), SLS_FD_JUNK);
    SLS_CHECK_EQ(classify(0x92, 0x00, 259), SLS_FD_JUNK);
}

SLS_TEST(proto_reg2)
{
    SLS_CHECK_EQ(classify(0x92, 0x01, 258), SLS_FD_SRTLA_REG2);
    SLS_CHECK_EQ(classify(0x92, 0x01, 2), SLS_FD_JUNK);
}

SLS_TEST(proto_srtla_other)
{
    SLS_CHECK_EQ(classify(0x90, 0x00, 4), SLS_FD_SRTLA_OTHER);    // KEEPALIVE + 2 bytes
    SLS_CHECK_EQ(classify(0x91, 0x00, 42), SLS_FD_SRTLA_OTHER);   // ACK + 40 bytes
    SLS_CHECK_EQ(classify(0x92, 0x02, 2), SLS_FD_SRTLA_OTHER);    // REG3
    SLS_CHECK_EQ(classify(0x92, 0xFF, 2), SLS_FD_SRTLA_OTHER);    // top of the range
    SLS_CHECK_EQ(classify(0x93, 0x00, 16), SLS_FD_SRT_OTHER);     // just above it
}

SLS_TEST(proto_srt_handshake)
{
    SLS_CHECK_EQ(classify(0x80, 0x00, 64), SLS_FD_SRT_HANDSHAKE);
    SLS_CHECK_EQ(classify(0x80, 0x00, 63), SLS_FD_SRT_OTHER);
    SLS_CHECK_EQ(classify(0x80, 0x00, 1500), SLS_FD_SRT_HANDSHAKE);
}

SLS_TEST(proto_srt_other)
{
    SLS_CHECK_EQ(classify(0x80, 0x02, 16), SLS_FD_SRT_OTHER);
    std::vector<uint8_t> data(1316, 0xAB);
    data[0] = 0x00; data[1] = 0x00; data[2] = 0x00; data[3] = 0x01;
    SLS_CHECK_EQ(sls_fd_classify(&data[0], (int)data.size()), SLS_FD_SRT_OTHER);
}

SLS_TEST(proto_junk)
{
    uint8_t b[16] = {0x80, 0x00};
    SLS_CHECK_EQ(sls_fd_classify(b, 0), SLS_FD_JUNK);
    SLS_CHECK_EQ(sls_fd_classify(b, 1), SLS_FD_JUNK);
    SLS_CHECK_EQ(sls_fd_classify(b, 15), SLS_FD_JUNK);
    SLS_CHECK_EQ(sls_fd_classify(NULL, 0), SLS_FD_JUNK);
    SLS_CHECK_EQ(sls_fd_classify(NULL, 64), SLS_FD_JUNK);
    SLS_CHECK_EQ(sls_fd_classify(b, -1), SLS_FD_JUNK);
}
