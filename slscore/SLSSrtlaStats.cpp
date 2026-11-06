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


#include <map>

#include "SLSSrtlaStats.hpp"
#include "SLSLock.hpp"

namespace {
struct entry {
    const void *door;
    uint32_t    group;
    std::string json;
};
CSLSMutex &stats_mutex()
{
    static CSLSMutex m;
    return m;
}
std::map<std::string, entry> &stats_map()
{
    static std::map<std::string, entry> m;
    return m;
}
}

std::string sls_srtla_stats_key(int public_port, const std::string &ip, int port)
{
    return std::to_string(public_port) + "/" + ip + ":" + std::to_string(port);
}

void sls_srtla_stats_set(const std::string &key, const void *door, uint32_t group, const std::string &json)
{
    CSLSLock lock(&stats_mutex());
    entry &e = stats_map()[key];
    e.door  = door;
    e.group = group;
    e.json  = json;
}

void sls_srtla_stats_erase(const std::string &key, const void *door, uint32_t group)
{
    CSLSLock lock(&stats_mutex());
    std::map<std::string, entry>::iterator it = stats_map().find(key);
    if (it != stats_map().end() && it->second.door == door && it->second.group == group) {
        stats_map().erase(it);
    }
}

std::string sls_srtla_stats_get(const std::string &key)
{
    CSLSLock lock(&stats_mutex());
    std::map<std::string, entry>::const_iterator it = stats_map().find(key);
    return it == stats_map().end() ? std::string() : it->second.json;
}
