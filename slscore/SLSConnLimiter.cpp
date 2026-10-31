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


#include "SLSConnLimiter.hpp"
#include "SLSLog.hpp"

CSLSConnLimiter::CSLSConnLimiter(int max_total, int max_per_ip)
{
    m_max_total  = max_total;
    m_max_per_ip = max_per_ip;
    m_total      = 0;
}

CSLSConnLimiter::~CSLSConnLimiter()
{
}

bool CSLSConnLimiter::try_acquire(const char *ip, int *why)
{
    std::string key = ip ? ip : "";
    CSLSLock lock(&m_mutex);
    if (m_max_total > 0 && m_total >= m_max_total) {
        if (why) *why = SLS_CONN_TOTAL;
        return false;
    }
    std::map<std::string, int>::iterator it = m_per_ip.find(key);
    int n = (it == m_per_ip.end()) ? 0 : it->second;
    if (m_max_per_ip > 0 && n >= m_max_per_ip) {
        if (why) *why = SLS_CONN_PER_IP;
        return false;
    }
    m_per_ip[key] = n + 1;
    m_total++;
    if (why) *why = SLS_CONN_OK;
    return true;
}

void CSLSConnLimiter::release(const char *ip)
{
    std::string key = ip ? ip : "";
    bool held = false;
    {
        CSLSLock lock(&m_mutex);
        std::map<std::string, int>::iterator it = m_per_ip.find(key);
        if (it != m_per_ip.end() && it->second > 0 && m_total > 0) {
            held = true;
            m_total--;
            if (--it->second == 0) {
                m_per_ip.erase(it);
            }
        }
    }
    if (!held) {
        // a release without a matching acquire is a bookkeeping bug
        sls_log(SLS_LOG_ERROR, "[%p]CSLSConnLimiter::release, no slot held for ip='%s'.", this, key.c_str());
    }
}

int CSLSConnLimiter::total() const
{
    CSLSLock lock(&m_mutex);
    return m_total;
}
