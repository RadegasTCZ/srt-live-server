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


#ifndef _SLSSockTable_INCLUDE_
#define _SLSSockTable_INCLUDE_

#include <map>
#include <stdint.h>

#include "SLSLock.hpp"

/**
 * What the listen callback noted for a handshake, kept per new socket until
 * the outcome takes it (the accept, or a rejection libsrt logs). Bounded by
 * count and by age, and it never refuses (R43): a full table drops its
 * OLDEST record to make room, and a record older than ttl_ms is dropped.
 * The SRT handshake completes in milliseconds after the callback (a caller
 * gives up after its connect timeout, 3 s by default), so a record that
 * old belongs to a handshake that will not complete. A session whose
 * record was dropped is refused at accept (fail safe), so a spray of
 * incomplete handshakes costs the newest legitimate client nothing; how fast
 * one address can add records is bounded by the handshake rate limit.
 *
 * Thread-safe (libsrt's receive thread puts, the workers take); the mutex
 * covers map operations only. Every operation is O(log n).
 */
template <class V>
class CSLSSockTable
{
public:
    CSLSSockTable(size_t max_records, int ttl_ms) : m_max(max_records), m_ttl_ms(ttl_ms), m_seq(0) {}

    // Store v for ns (replacing an older record of the same socket id).
    // Returns true if the table was full and its oldest record was dropped.
    bool put(int ns, const V &v, int64_t now_ms)
    {
        CSLSLock lock(&m_mutex);
        expire_locked(now_ms);
        bool evicted = false;
        typename std::map<int, rec>::iterator it = m_recs.find(ns);
        if (it != m_recs.end()) {
            m_order.erase(it->second.seq);
            m_recs.erase(it);
        } else if (m_max > 0 && m_recs.size() >= m_max) {
            std::map<uint64_t, int>::iterator o = m_order.begin();
            m_recs.erase(o->second);
            m_order.erase(o);
            evicted = true;
        }
        rec r;
        r.v    = v;
        r.t_ms = now_ms;
        r.seq  = m_seq++;
        m_recs[ns]     = r;
        m_order[r.seq] = ns;
        return evicted;
    }
    // The record of ns, removed; false if there is none (never noted, or dropped).
    bool take(int ns, V *v)
    {
        CSLSLock lock(&m_mutex);
        typename std::map<int, rec>::iterator it = m_recs.find(ns);
        if (it == m_recs.end()) {
            return false;
        }
        if (v) *v = it->second.v;
        m_order.erase(it->second.seq);
        m_recs.erase(it);
        return true;
    }
    size_t size() const
    {
        CSLSLock lock(&m_mutex);
        return m_recs.size();
    }
    int ttl_ms() const { return m_ttl_ms; }

private:
    struct rec {
        V        v;
        int64_t  t_ms;
        uint64_t seq;
    };
    size_t                   m_max;
    int                      m_ttl_ms;
    uint64_t                 m_seq;
    mutable CSLSMutex        m_mutex;
    std::map<int, rec>       m_recs;
    std::map<uint64_t, int>  m_order;   // put order = age order (the clock only moves on)

    void expire_locked(int64_t now_ms)
    {
        while (!m_order.empty()) {
            std::map<uint64_t, int>::iterator o = m_order.begin();
            typename std::map<int, rec>::iterator r = m_recs.find(o->second);
            if (now_ms - r->second.t_ms < m_ttl_ms) {
                break;
            }
            m_recs.erase(r);
            m_order.erase(o);
        }
    }
};

#endif
