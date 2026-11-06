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


#ifndef _SLSSrtlaStats_INCLUDE_
#define _SLSSrtlaStats_INCLUDE_

#include <string>

#include <stdint.h>

/**
 * The SRTLA part of the stats: the front doors publish one JSON object per
 * group (its links and reorder counters), keyed by sls_srtla_stats_key():
 * the public port and the group's registering "ip:port", which is the
 * publisher role's remote address. The role's stats line includes it as
 * "srtla". Each entry has an owner (front door, group): only the owner may
 * erase it, so an older group from the same address cannot erase a newer
 * one's. Thread-safe (one mutex, no system calls under it); the front door
 * refreshes it every few seconds.
 */
std::string sls_srtla_stats_key(int public_port, const std::string &ip, int port);
void        sls_srtla_stats_set(const std::string &key, const void *door, uint32_t group, const std::string &json);
void        sls_srtla_stats_erase(const std::string &key, const void *door, uint32_t group);
std::string sls_srtla_stats_get(const std::string &key);   // "" if none

#endif
