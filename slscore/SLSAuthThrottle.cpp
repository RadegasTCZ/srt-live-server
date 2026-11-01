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


#include <algorithm>

#include "SLSAuthThrottle.hpp"
#include "SLSConnLimiter.hpp"   // sls_ip_key

CSLSAuthThrottle::CSLSAuthThrottle(int max_fail, int window_ms, int block_ms,
                                   size_t max_keys, size_t max_pending, size_t max_per_addr)
    : m_pending(max_pending, SLS_AUTH_PENDING_TTL_MS)
{
    m_max_fail     = max_fail;
    m_window_ms    = window_ms;
    m_block_ms     = block_ms;
    m_max_keys     = max_keys > 0 ? max_keys : 1;
    m_max_per_addr = max_per_addr > 0 ? max_per_addr : 1;
}

// (Re)place e in the eviction order. A block that is over, or a window
// that is over, counts as nothing left to remember.
void CSLSAuthThrottle::set_ord_locked(const std::string &key, entry &e, int64_t now_ms)
{
    m_order.erase(e.ord);
    if (e.blocked_until_ms > now_ms) {
        e.ord.cls = 1; e.ord.a = e.blocked_until_ms; e.ord.b = 0;
    } else {
        e.ord.cls = 0;
        e.ord.a   = (now_ms - e.window_start_ms < m_window_ms) ? e.fails : 0;
        e.ord.b   = e.last_ms;
    }
    e.ord.key = key;
    m_order.insert(e.ord);
}

// key by value: callers pass keys that live in the containers it erases from
void CSLSAuthThrottle::remove_locked(std::string key)
{
    std::map<std::string, entry>::iterator it = m_keys.find(key);
    if (it == m_keys.end()) {
        return;
    }
    m_order.erase(it->second.ord);
    std::map<std::string, std::vector<std::string> >::iterator a = m_by_addr.find(it->second.addr);
    if (a != m_by_addr.end()) {
        a->second.erase(std::remove(a->second.begin(), a->second.end(), key), a->second.end());
        if (a->second.empty()) {
            m_by_addr.erase(a);
        }
    }
    m_keys.erase(it);
}

bool CSLSAuthThrottle::blocked(const char *ip, const std::string &user, int64_t now_ms, int *left_ms)
{
    std::string key = sls_ip_key(ip) + "\n" + user;   // a user name never holds '\n'
    CSLSLock lock(&m_mutex);
    std::map<std::string, entry>::iterator it = m_keys.find(key);
    if (it == m_keys.end()) {
        return false;   // untracked: never blocked
    }
    entry &e = it->second;
    if (e.blocked_until_ms <= now_ms) {
        if (1 == e.ord.cls) {
            set_ord_locked(key, e, now_ms);   // the block is over: first to go again
        }
        return false;
    }
    if (left_ms) {
        *left_ms = (int)(e.blocked_until_ms - now_ms);
    }
    return true;
}

void CSLSAuthThrottle::note_handshake(int ns, const char *ip, const std::string &user, int64_t now_ms)
{
    pend p;
    p.ip   = ip;
    p.addr = sls_ip_key(ip);
    p.key  = p.addr + "\n" + user;
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
    std::map<std::string, entry>::iterator it = m_keys.find(p.key);
    if (it == m_keys.end()) {
        // Make room, never by sharing: this address's least worth keeping
        // pair if it has its share, else the table's.
        std::vector<std::string> &mine = m_by_addr[p.addr];
        if (mine.size() >= m_max_per_addr) {
            std::string victim;
            const ord_t *best = NULL;
            for (size_t i = 0; i < mine.size(); i++) {
                const ord_t &o = m_keys[mine[i]].ord;
                if (NULL == best || o < *best) {
                    best = &o;
                    victim = mine[i];
                }
            }
            remove_locked(victim);
        } else if (m_keys.size() >= m_max_keys) {
            remove_locked(m_order.begin()->key);   // (copied: see remove_locked)
        }
        entry e;
        e.fails            = 0;
        e.window_start_ms  = now_ms;
        e.blocked_until_ms = 0;
        e.last_ms          = now_ms;
        e.addr             = p.addr;
        e.ord.cls = -1; e.ord.a = 0; e.ord.b = 0;   // not in m_order yet
        it = m_keys.insert(std::make_pair(p.key, e)).first;
        m_by_addr[p.addr].push_back(p.key);         // (remove_locked may have dropped the vector)
    }
    entry &e = it->second;
    if (e.blocked_until_ms > now_ms) {
        set_ord_locked(p.key, e, now_ms);
        return false;   // already blocked (the callback refuses the later attempts)
    }
    if (now_ms - e.window_start_ms >= m_window_ms) {
        e.fails           = 0;
        e.window_start_ms = now_ms;
    }
    e.last_ms = now_ms;
    bool started = false;
    if (++e.fails >= m_max_fail) {
        e.fails            = 0;
        e.window_start_ms  = now_ms;
        e.blocked_until_ms = now_ms + m_block_ms;
        started = true;
    }
    set_ord_locked(p.key, e, now_ms);
    if (started) {
        if (ip)   *ip   = p.ip;
        if (user) *user = p.user;
    }
    return started;
}

size_t CSLSAuthThrottle::tracked_keys() const
{
    CSLSLock lock(&m_mutex);
    return m_keys.size();
}

size_t CSLSAuthThrottle::tracked_keys_of(const char *ip) const
{
    std::string addr = sls_ip_key(ip);
    CSLSLock lock(&m_mutex);
    std::map<std::string, std::vector<std::string> >::const_iterator a = m_by_addr.find(addr);
    return a == m_by_addr.end() ? 0 : a->second.size();
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
