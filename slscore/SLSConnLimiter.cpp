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

CSLSConnLimiter::CSLSConnLimiter(int max_total, int max_per_ip, int reserved_publishers)
{
    m_max_total  = max_total;
    m_max_per_ip = max_per_ip;
    m_reserved   = (max_total > 0 && reserved_publishers > 0 && reserved_publishers < max_total) ? reserved_publishers : 0;
    m_total      = 0;
    m_shared     = 0;
    m_hs_rate    = 0;
    m_hs_last_prune_ms = 0;
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
    std::string key = ip ? ip : "";
    CSLSLock lock(&m_mutex);
    return verdict_locked(key, kind, stream, max_stream);
}

bool CSLSConnLimiter::try_acquire(const char *ip, int kind, const std::string &stream, int max_stream, int *why)
{
    std::string key = ip ? ip : "";
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
    std::string key = ip ? ip : "";
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

// A bucket idle for a second is full again and carries no state: such
// buckets are pruned once the map is large (at most once a second). Past
// SLS_HS_BUCKETS_MAX addresses (after a prune) a new address is let through
// untracked: the other caps still hold and the map cannot grow without
// bound. A handshake has to answer libsrt's cookie, so every address here is
// a real, reachable one.
#define SLS_HS_BUCKETS_PRUNE 4096
#define SLS_HS_BUCKETS_MAX   65536
bool CSLSConnLimiter::handshake_allowed(const char *ip, int64_t now_ms)
{
    if (m_hs_rate <= 0) {
        return true;
    }
    std::string key = ip ? ip : "";
    CSLSLock lock(&m_mutex);
    std::map<std::string, hs_bucket>::iterator it = m_hs_buckets.find(key);
    if (it == m_hs_buckets.end()) {
        if (m_hs_buckets.size() >= SLS_HS_BUCKETS_PRUNE && now_ms - m_hs_last_prune_ms >= 1000) {
            m_hs_last_prune_ms = now_ms;
            for (std::map<std::string, hs_bucket>::iterator p = m_hs_buckets.begin(); p != m_hs_buckets.end();) {
                if (now_ms - p->second.last_ms >= 1000) {
                    m_hs_buckets.erase(p++);
                } else {
                    ++p;
                }
            }
        }
        if (m_hs_buckets.size() >= SLS_HS_BUCKETS_MAX) {
            return true;
        }
        hs_bucket b;
        b.tokens  = m_hs_rate - 1;
        b.last_ms = now_ms;
        m_hs_buckets[key] = b;
        return true;
    }
    hs_bucket &b = it->second;
    if (now_ms > b.last_ms) {
        b.tokens += (now_ms - b.last_ms) * (double)m_hs_rate / 1000.0;
        if (b.tokens > m_hs_rate) {
            b.tokens = m_hs_rate;
        }
        b.last_ms = now_ms;
    }
    if (b.tokens < 1.0) {
        return false;
    }
    b.tokens -= 1.0;
    return true;
}

int CSLSConnLimiter::total() const
{
    CSLSLock lock(&m_mutex);
    return m_total;
}
