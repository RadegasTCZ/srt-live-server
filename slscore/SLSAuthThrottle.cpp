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
                                   size_t max_keys, size_t max_pending)
    : m_pending(max_pending, SLS_AUTH_PENDING_TTL_MS)
{
    m_max_fail    = max_fail;
    m_window_ms   = window_ms;
    m_block_ms    = block_ms;
    m_max_keys    = max_keys;
    m_overflow.fails            = 0;
    m_overflow.window_start_ms  = 0;
    m_overflow.blocked_until_ms = 0;
    m_keys_pruned_ms = 0;
}

CSLSAuthThrottle::entry *CSLSAuthThrottle::find_locked(const std::string &key, bool insert, int64_t now_ms)
{
    std::map<std::string, entry>::iterator it = m_keys.find(key);
    if (it != m_keys.end()) {
        return &it->second;
    }
    if (m_keys.size() >= m_max_keys && now_ms - m_keys_pruned_ms >= 50) {
        // drop the pairs with nothing left to remember: not blocked, and
        // their window is over (at most every 50 ms: a full table is O(n))
        m_keys_pruned_ms = now_ms;
        for (it = m_keys.begin(); it != m_keys.end();) {
            if (it->second.blocked_until_ms <= now_ms && now_ms - it->second.window_start_ms >= m_window_ms) {
                m_keys.erase(it++);
            } else {
                ++it;
            }
        }
    }
    if (m_keys.size() >= m_max_keys) {
        return &m_overflow;
    }
    if (!insert) {
        return NULL;
    }
    entry e;
    e.fails            = 0;
    e.window_start_ms  = now_ms;
    e.blocked_until_ms = 0;
    return &(m_keys[key] = e);
}

bool CSLSAuthThrottle::blocked(const char *ip, const std::string &user, int64_t now_ms, int *left_ms)
{
    std::string key = sls_ip_key(ip) + "\n" + user;   // a user name never holds '\n'
    CSLSLock lock(&m_mutex);
    entry *e = find_locked(key, false, now_ms);
    if (NULL == e || e->blocked_until_ms <= now_ms) {
        return false;
    }
    if (left_ms) {
        *left_ms = (int)(e->blocked_until_ms - now_ms);
    }
    return true;
}

void CSLSAuthThrottle::note_handshake(int ns, const char *ip, const std::string &user, int64_t now_ms)
{
    pend p;
    p.ip   = ip;
    p.key  = sls_ip_key(ip) + "\n" + user;
    p.user = user;
    m_pending.put(ns, p, now_ms);
}

void CSLSAuthThrottle::handshake_done(int ns)
{
    m_pending.take(ns, NULL);
}

bool CSLSAuthThrottle::bad_secret(int ns, int64_t now_ms, std::string *ip, std::string *user)
{
    pend p;
    if (!m_pending.take(ns, &p)) {
        return false;   // not a handshake the callback noted (or its note was dropped)
    }
    CSLSLock lock(&m_mutex);
    entry *e = find_locked(p.key, true, now_ms);
    if (e->blocked_until_ms > now_ms) {
        return false;   // already blocked (the callback refuses the later attempts)
    }
    if (now_ms - e->window_start_ms >= m_window_ms) {
        e->fails           = 0;
        e->window_start_ms = now_ms;
    }
    if (++e->fails < m_max_fail) {
        return false;
    }
    e->fails            = 0;
    e->window_start_ms  = now_ms;
    e->blocked_until_ms = now_ms + m_block_ms;
    if (ip)   *ip   = p.ip;
    if (user) *user = (e == &m_overflow) ? std::string("(untracked: table full)") : p.user;
    return true;
}

size_t CSLSAuthThrottle::tracked_keys() const
{
    CSLSLock lock(&m_mutex);
    return m_keys.size();
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
