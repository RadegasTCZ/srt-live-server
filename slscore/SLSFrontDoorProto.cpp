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


#include <stddef.h>

#include "SLSFrontDoorProto.hpp"

sls_fd_kind_t sls_fd_classify(const uint8_t *p, int len)
{
    if (NULL == p || len < 2) {
        return SLS_FD_JUNK;
    }
    uint16_t type = (uint16_t)((p[0] << 8) | p[1]);

    if (SLS_SRTLA_TYPE_REG1 == type || SLS_SRTLA_TYPE_REG2 == type) {
        if (SLS_SRTLA_REG_LEN != len) {
            return SLS_FD_JUNK;
        }
        return SLS_SRTLA_TYPE_REG1 == type ? SLS_FD_SRTLA_REG1 : SLS_FD_SRTLA_REG2;
    }
    if (type >= SLS_SRTLA_TYPE_FIRST && type <= SLS_SRTLA_TYPE_LAST) {
        return SLS_FD_SRTLA_OTHER;   // keepalive, ACK, REG3 and replies are short
    }
    if (len < SLS_SRT_HDR_LEN) {
        return SLS_FD_JUNK;
    }
    if (SLS_SRT_TYPE_HANDSHAKE == type && len >= SLS_SRT_HS_MIN_LEN) {
        return SLS_FD_SRT_HANDSHAKE;
    }
    return SLS_FD_SRT_OTHER;
}

const char *sls_fd_kind_name(sls_fd_kind_t kind)
{
    switch (kind) {
    case SLS_FD_SRTLA_REG1:     return "srtla-reg1";
    case SLS_FD_SRTLA_REG2:     return "srtla-reg2";
    case SLS_FD_SRTLA_OTHER:    return "srtla-other";
    case SLS_FD_SRT_HANDSHAKE:  return "srt-handshake";
    case SLS_FD_SRT_OTHER:      return "srt-other";
    case SLS_FD_JUNK:           return "junk";
    }
    return "?";
}
