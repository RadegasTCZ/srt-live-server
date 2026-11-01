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
#include <arpa/inet.h>
#include <netinet/in.h>

#include "SLSConnLimiter.hpp"
#include "SLSLog.hpp"

std::string sls_ip_key(const char *ip)
{
    if (NULL == ip) {
        return std::string();
    }
    struct in6_addr a6;
    if (1 != inet_pton(AF_INET6, ip, &a6)) {
        return std::string(ip);           // IPv4 (or not an address): as is
    }
    char buf[INET6_ADDRSTRLEN + 4];
    if (IN6_IS_ADDR_V4MAPPED(&a6)) {
        struct in_addr a4;
        memcpy(&a4, &a6.s6_addr[12], 4);
        if (NULL == inet_ntop(AF_INET, &a4, buf, sizeof(buf))) {
            return std::string(ip);
        }
        return std::string(buf);
    }
    memset(&a6.s6_addr[8], 0, 8);         // the /64 prefix
    if (NULL == inet_ntop(AF_INET6, &a6, buf, sizeof(buf))) {
        return std::string(ip);
    }
    return std::string(buf) + "/64";
}

CSLSConnLimiter::CSLSConnLimiter(int max_total, int max_per_ip, int reserved_publishers)
{
    m_max_total  = max_total;
    m_max_per_ip = max_per_ip;
    m_reserved   = (max_total > 0 && reserved_publishers > 0 && reserved_publishers < max_total) ? reserved_publishers : 0;
    m_total      = 0;
    m_shared     = 0;
    m_hs_rate    = 0;
    m_hs_max_buckets   = 65536;
    m_hs_overflow.tokens  = 0;
    m_hs_overflow.last_ms = 0;
    m_hs_last_prune_ms = 0;
}

void CSLSConnLimiter::set_handshake_rate(int per_sec, size_t max_buckets)
{
    m_hs_rate        = per_sec;
    m_hs_max_buckets = max_buckets > 0 ? max_buckets : 1;
    m_hs_overflow.tokens  = overflow_rate();
    m_hs_overflow.last_ms = 0;
}

// The untracked addresses' shared budget: 4 x the per-IP rate, or the
// connection cap per second if that is more (so a full map never holds
// admissions below what the server could take anyway).
int CSLSConnLimiter::overflow_rate() const
{
    int r = 4 * m_hs_rate;
    return m_max_total > r ? m_max_total : r;
}

size_t CSLSConnLimiter::tracked_addresses() const
{
    CSLSLock lock(&m_mutex);
    return m_hs_buckets.size();
}

// Drops every bucket that has refilled by now: a full bucket carries no
// state, so this loses nothing.
void CSLSConnLimiter::prune_refilled_locked(int64_t now_ms)
{
    m_hs_last_prune_ms = now_ms;
    for (std::map<std::string, hs_bucket>::iterator p = m_hs_buckets.begin(); p != m_hs_buckets.end();) {
        const hs_bucket &b = p->second;
        if (b.tokens + (now_ms - b.last_ms) * (double)m_hs_rate / 1000.0 >= m_hs_rate) {
            m_hs_buckets.erase(p++);
        } else {
            ++p;
        }
    }
}

CSLSConnLimiter::~CSLSConnLimiter()
{
}

int CSLSConnLimiter::verdict_locked(const std::string &ip, int kind, const std::string &stream, int max_stream) const
{
    if (m_max_total > 0 && m_total >= m_max_total) {
        return SLS_CONN_TOTAL;
    }
    if (SLS_SLOT_PUBLISHER != kind && m_max_total > 0 && m_shared >= m_max_total - m_reserved) {
        return SLS_SLOT_PLAYER == kind ? SLS_CONN_PLAYERS : SLS_CONN_UNVERIFIED;
    }
    if (m_max_per_ip > 0) {
        std::map<std::string, int>::const_iterator it = m_per_ip.find(ip);
        if (it != m_per_ip.end() && it->second >= m_max_per_ip) {
            return SLS_CONN_PER_IP;
        }
    }
    if (SLS_SLOT_PLAYER == kind && max_stream > 0) {
        std::map<std::string, int>::const_iterator it = m_per_stream.find(stream);
        if (it != m_per_stream.end() && it->second >= max_stream) {
            return SLS_CONN_STREAM;
        }
    }
    return SLS_CONN_OK;
}

int CSLSConnLimiter::check(const char *ip, int kind, const std::string &stream, int max_stream) const
{
    std::string key = sls_ip_key(ip);
    CSLSLock lock(&m_mutex);
    return verdict_locked(key, kind, stream, max_stream);
}

bool CSLSConnLimiter::try_acquire(const char *ip, int kind, const std::string &stream, int max_stream, int *why)
{
    std::string key = sls_ip_key(ip);
    CSLSLock lock(&m_mutex);
    int v = verdict_locked(key, kind, stream, max_stream);
    if (why) *why = v;
    if (SLS_CONN_OK != v) {
        return false;
    }
    m_per_ip[key]++;
    m_total++;
    if (SLS_SLOT_PUBLISHER != kind) {
        m_shared++;
    }
    if (SLS_SLOT_PLAYER == kind) {
        m_per_stream[stream]++;
    }
    return true;
}

void CSLSConnLimiter::promote()
{
    CSLSLock lock(&m_mutex);
    if (m_shared > 0) {
        m_shared--;
    }
}

void CSLSConnLimiter::release(const char *ip, int kind, const std::string &stream)
{
    std::string key = sls_ip_key(ip);
    bool held = false;
    {
        CSLSLock lock(&m_mutex);
        std::map<std::string, int>::iterator it = m_per_ip.find(key);
        std::map<std::string, int>::iterator st = m_per_stream.end();
        if (SLS_SLOT_PLAYER == kind) {
            st = m_per_stream.find(stream);
        }
        if (it != m_per_ip.end() && it->second > 0 && m_total > 0 &&
            (SLS_SLOT_PUBLISHER == kind || m_shared > 0) &&
            (SLS_SLOT_PLAYER != kind || (st != m_per_stream.end() && st->second > 0))) {
            held = true;
            m_total--;
            if (--it->second == 0) {
                m_per_ip.erase(it);
            }
            if (SLS_SLOT_PUBLISHER != kind) {
                m_shared--;
            }
            if (SLS_SLOT_PLAYER == kind && --st->second == 0) {
                m_per_stream.erase(st);
            }
        }
    }
    if (!held) {
        // a release without a matching acquire is a bookkeeping bug
        sls_log(SLS_LOG_ERROR, "[%p]CSLSConnLimiter::release, no slot held for ip='%s'.", this, key.c_str());
    }
}

// Refill b (capacity and rate: rate per second) up to now_ms, then take one
// token if there is one.
bool CSLSConnLimiter::take_token(hs_bucket &b, int rate, int64_t now_ms)
{
    if (now_ms > b.last_ms) {
        b.tokens += (now_ms - b.last_ms) * (double)rate / 1000.0;
        if (b.tokens > rate) {
            b.tokens = rate;
        }
        b.last_ms = now_ms;
    }
    if (b.tokens < 1.0) {
        return false;
    }
    b.tokens -= 1.0;
    return true;
}

// A refilled bucket carries no state: refilled buckets are pruned once the
// map is large (at most once a second), and whenever a new address finds it
// full (at most every SLS_HS_FULL_PRUNE_MS, which bounds the cost of the walk
// under a spray). Only if the map is still full is the new address left
// untracked: it draws from one overflow bucket shared by all untracked
// addresses (overflow_rate()), so a spray from more active addresses than
// the map holds keeps the map and the handshake rate bounded, never
// unlimited; newcomers then share that budget until buckets refill.
#define SLS_HS_BUCKETS_PRUNE 4096
#define SLS_HS_FULL_PRUNE_MS 50
bool CSLSConnLimiter::handshake_allowed(const char *ip, int64_t now_ms)
{
    if (m_hs_rate <= 0) {
        return true;
    }
    std::string key = sls_ip_key(ip);
    CSLSLock lock(&m_mutex);
    std::map<std::string, hs_bucket>::iterator it = m_hs_buckets.find(key);
    if (it == m_hs_buckets.end()) {
        size_t prune_at = m_hs_max_buckets < SLS_HS_BUCKETS_PRUNE ? m_hs_max_buckets : SLS_HS_BUCKETS_PRUNE;
        if (m_hs_buckets.size() >= prune_at && now_ms - m_hs_last_prune_ms >= 1000) {
            prune_refilled_locked(now_ms);
        }
        if (m_hs_buckets.size() >= m_hs_max_buckets && now_ms - m_hs_last_prune_ms >= SLS_HS_FULL_PRUNE_MS) {
            prune_refilled_locked(now_ms);
        }
        if (m_hs_buckets.size() >= m_hs_max_buckets) {
            return take_token(m_hs_overflow, overflow_rate(), now_ms);
        }
        hs_bucket b;
        b.tokens  = m_hs_rate - 1;
        b.last_ms = now_ms;
        m_hs_buckets[key] = b;
        return true;
    }
    return take_token(it->second, m_hs_rate, now_ms);
}

int CSLSConnLimiter::total() const
{
    CSLSLock lock(&m_mutex);
    return m_total;
}
