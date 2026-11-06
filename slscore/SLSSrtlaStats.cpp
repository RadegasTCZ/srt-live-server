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
CSLSMutex &stats_mutex()
{
    static CSLSMutex m;
    return m;
}
std::map<std::string, std::string> &stats_map()
{
    static std::map<std::string, std::string> m;
    return m;
}
}

void sls_srtla_stats_set(const std::string &key, const std::string &json)
{
    CSLSLock lock(&stats_mutex());
    stats_map()[key] = json;
}

void sls_srtla_stats_erase(const std::string &key)
{
    CSLSLock lock(&stats_mutex());
    stats_map().erase(key);
}

std::string sls_srtla_stats_get(const std::string &key)
{
    CSLSLock lock(&stats_mutex());
    std::map<std::string, std::string>::const_iterator it = stats_map().find(key);
    return it == stats_map().end() ? std::string() : it->second;
}
