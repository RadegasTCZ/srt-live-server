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


#ifndef _SLSConnLimiter_INCLUDE_
#define _SLSConnLimiter_INCLUDE_

#include <map>
#include <string>
#include <stdint.h>

#include "SLSLock.hpp"

/**
 * Admission slots for listener-accepted connections (publishers and players).
 * One limiter per CSLSManager, shared by all its listeners. Outbound relay
 * connections (pullers, pushers) never take a slot.
 *
 * A slot is taken at accept (try_acquire) and handed to the role that owns the
 * socket (CSLSRole::set_conn_limiter); the role gives it back exactly once, in
 * CSLSRole::uninit (which the destructor also runs).
 *
 * Caps, all checked together under one mutex:
 * - max_total: every accepted connection;
 * - the reserve: the shared slots (players, and publishers whose claim is
 *   not verified yet) are at most max_total - reserved, so viewers (or
 *   anyone who merely names a publisher stream) can never lock verified
 *   publishers out;
 * - max_per_ip: concurrent connections from one address;
 * - per stream (max_stream, given by the caller): players of one stream.
 * check() gives try_acquire's verdict without taking anything: the listen
 * callback uses it to refuse before the handshake completes.
 */
enum SLS_CONN_REFUSAL {
    SLS_CONN_OK         = 0,
    SLS_CONN_TOTAL      = 1,   // max_total reached
    SLS_CONN_PER_IP     = 2,   // max_per_ip reached for this address
    SLS_CONN_PLAYERS    = 3,   // a player: the shared slots (max_total - reserved) are in use
    SLS_CONN_STREAM     = 4,   // the stream has max_stream players
    SLS_CONN_UNVERIFIED = 5,   // an unverified publisher: the shared slots are in use
};

// What a slot is for. Only a VERIFIED publisher may use the reserve: one
// whose passphrase libsrt checked (user_file, srt_passphrase) or one the
// on_event_url hook admitted, or any publisher on a server without auth.
enum SLS_SLOT_KIND {
    SLS_SLOT_PLAYER    = 0,
    SLS_SLOT_PUBLISHER = 1,   // verified: may use the reserve
    SLS_SLOT_PENDING   = 2,   // a publisher the hook has not admitted yet: a shared slot
};

// The key one address counts under, for the per-IP cap and the handshake
// rate alike: IPv4 as is, an IPv4-mapped IPv6 address (::ffff:a.b.c.d) as
// that IPv4 address, any other IPv6 address as its /64 prefix
// ("2001:db8:1:2::/64": one host usually holds a whole /64). Anything that
// does not parse is used verbatim.
std::string sls_ip_key(const char *ip);

class CSLSConnLimiter
{
public:
    // 0 = unlimited for max_total and max_per_ip; reserved_publishers < max_total
    CSLSConnLimiter(int max_total, int max_per_ip, int reserved_publishers = 0);
    ~CSLSConnLimiter();

    // Thread-safe; on refusal *why (if given) says which cap was hit. kind is
    // an SLS_SLOT_KIND; stream is a player's stream key ("" otherwise),
    // max_stream its cap (0 = none). release() must get the same ip, kind
    // (after a promote: SLS_SLOT_PUBLISHER) and stream.
    bool try_acquire(const char *ip, int kind, const std::string &stream, int max_stream, int *why = NULL);
    void release(const char *ip, int kind, const std::string &stream);
    // a PENDING slot becomes a PUBLISHER slot (the hook admitted it): it
    // leaves the shared slots, which another connection may then take
    void promote();
    // try_acquire's verdict (an SLS_CONN_REFUSAL), taking nothing
    int  check(const char *ip, int kind, const std::string &stream, int max_stream) const;
    int  total() const;
    int  max_total() const  { return m_max_total; }
    int  max_per_ip() const { return m_max_per_ip; }
    int  reserved_publishers() const { return m_reserved; }
    // what players and unverified publishers may hold at most; 0 = unlimited
    int  max_players() const { return m_max_total > 0 ? m_max_total - m_reserved : 0; }

    // Handshakes one IP may start per second (a token bucket of that size,
    // refilled at that rate); 0 = no limit. Set before the listeners start.
    // Past max_buckets tracked addresses, the untracked ones share ONE
    // overflow bucket of max(4 x per_sec, max_total) a second (max_buckets:
    // for the unit test).
    void set_handshake_rate(int per_sec, size_t max_buckets = 65536);
    int  handshake_rate() const { return m_hs_rate; }
    // One handshake from ip at now_ms: true if within the rate (and counted).
    // Never blocks; called on libsrt's receive thread (listen callback).
    bool handshake_allowed(const char *ip, int64_t now_ms);
    int  overflow_rate() const;
    size_t tracked_addresses() const;

private:
    int                         m_max_total;
    int                         m_max_per_ip;
    int                         m_reserved;
    int                         m_total;
    int                         m_shared;       // PLAYER + PENDING slots held
    std::map<std::string, int>  m_per_ip;
    std::map<std::string, int>  m_per_stream;   // players per stream key
    mutable CSLSMutex           m_mutex;

    struct hs_bucket {
        double  tokens;
        int64_t last_ms;
    };
    int                              m_hs_rate;
    size_t                           m_hs_max_buckets;
    hs_bucket                        m_hs_overflow;   // shared by the untracked addresses
    std::map<std::string, hs_bucket> m_hs_buckets;
    int64_t                          m_hs_last_prune_ms;

    void prune_refilled_locked(int64_t now_ms);
    static bool take_token(hs_bucket &b, int rate, int64_t now_ms);
    int  verdict_locked(const std::string &ip, int kind, const std::string &stream, int max_stream) const;
};

#endif
