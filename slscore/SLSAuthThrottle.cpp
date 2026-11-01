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


#include "SLSAuthThrottle.hpp"
#include "SLSConnLimiter.hpp"   // sls_ip_key

CSLSAuthThrottle::CSLSAuthThrottle(int max_fail, int window_ms, int block_ms,
                                   size_t max_addrs, size_t max_pending)
    : m_pending(max_pending, SLS_AUTH_PENDING_TTL_MS)
{
    m_max_fail  = max_fail;
    m_window_ms = window_ms;
    m_block_ms  = block_ms;
    m_max_addrs = max_addrs;
    m_pruned_ms = 0;
}

bool CSLSAuthThrottle::blocked(const char *ip, int64_t now_ms, int *left_ms)
{
    std::string addr = sls_ip_key(ip);
    CSLSLock lock(&m_mutex);
    std::map<std::string, entry>::const_iterator it = m_addrs.find(addr);
    if (it == m_addrs.end() || it->second.blocked_until_ms <= now_ms) {
        return false;   // untracked addresses are never blocked
    }
    if (left_ms) {
        *left_ms = (int)(it->second.blocked_until_ms - now_ms);
    }
    return true;
}

void CSLSAuthThrottle::note_handshake(int ns, const char *ip, int64_t now_ms)
{
    pend p;
    p.ip   = ip;
    p.addr = sls_ip_key(ip);
    m_pending.put(ns, p, now_ms);
}

void CSLSAuthThrottle::handshake_done(int ns)
{
    m_pending.take(ns, NULL);
}

bool CSLSAuthThrottle::bad_secret(int ns, int64_t now_ms, std::string *ip)
{
    pend p;
    if (!m_pending.take(ns, &p)) {
        return false;   // not a handshake the callback noted (or its note was dropped)
    }
    CSLSLock lock(&m_mutex);
    std::map<std::string, entry>::iterator it = m_addrs.find(p.addr);
    if (it == m_addrs.end()) {
        if (m_addrs.size() >= m_max_addrs && now_ms - m_pruned_ms >= 50) {
            // only entries with nothing left to remember (window and block
            // both over); at most every 50 ms, a full table is O(n)
            m_pruned_ms = now_ms;
            for (it = m_addrs.begin(); it != m_addrs.end();) {
                if (expired(it->second, now_ms)) {
                    m_addrs.erase(it++);
                } else {
                    ++it;
                }
            }
        }
        if (m_addrs.size() >= m_max_addrs) {
            return false;   // full of live entries: this address stays untracked
        }
        entry e;
        e.fails            = 0;
        e.window_start_ms  = now_ms;
        e.blocked_until_ms = 0;
        it = m_addrs.insert(std::make_pair(p.addr, e)).first;
    }
    entry &e = it->second;
    if (e.blocked_until_ms > now_ms) {
        return false;   // already blocked (the callback refuses the later attempts)
    }
    if (now_ms - e.window_start_ms >= m_window_ms) {
        e.fails           = 0;
        e.window_start_ms = now_ms;
    }
    if (++e.fails < m_max_fail) {
        return false;
    }
    e.fails            = 0;
    e.window_start_ms  = now_ms;
    e.blocked_until_ms = now_ms + m_block_ms;
    if (ip) *ip = p.ip;
    return true;
}

size_t CSLSAuthThrottle::tracked() const
{
    CSLSLock lock(&m_mutex);
    return m_addrs.size();
}

size_t CSLSAuthThrottle::pending() const
{
    return m_pending.size();
}

CSLSAuthThrottle &sls_auth_throttle()
{
    static CSLSAuthThrottle t;   // C++11: initialised once, thread-safe
    return t;
}
