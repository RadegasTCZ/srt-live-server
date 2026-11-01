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


#ifndef _SLSAuthThrottle_INCLUDE_
#define _SLSAuthThrottle_INCLUDE_

#include <map>
#include <string>
#include <stdint.h>

#include "SLSLock.hpp"
#include "SLSSockTable.hpp"

#define SLS_AUTH_FAIL_MAX        5        // wrong passphrases from one address ...
#define SLS_AUTH_FAIL_WINDOW_MS  60000    // ... within this long ...
#define SLS_AUTH_BLOCK_MS        60000    // ... refuse that address this long
#define SLS_AUTH_MAX_ADDRS       16384    // addresses tracked
#define SLS_AUTH_MAX_PENDING     4096     // passphrase handshakes between callback and outcome
#define SLS_AUTH_PENDING_TTL_MS  5000     // as the listener's records (R43)

/**
 * Wrong-passphrase throttle (S8, final design R45). libsrt checks the
 * passphrase after the listen callback and tells the listener nothing when
 * it fails: no accept, no event, and srt_getrejectreason() on the dead
 * socket says UNKNOWN. The one trace is libsrt's own log line
 *   "@<socket>:newConnection: connection rejected due to: ... ERROR:BADSECRET"
 * which CSLSSrt's log handler turns into bad_secret(<socket>). So the
 * callback notes (note_handshake) which address each passphrase handshake
 * comes from, and SLS_AUTH_FAIL_MAX failures from one address within
 * SLS_AUTH_FAIL_WINDOW_MS block that address for SLS_AUTH_BLOCK_MS: the
 * callback refuses it (blocked()) before any key exchange, whatever user or
 * stream it names, the right passphrase included.
 *
 * Keyed ONLY on the address (sls_ip_key: IPv4, an IPv4-mapped address as
 * IPv4, IPv6 per /64), across all users and streams: rotating user or stream
 * names resets nothing. One entry per address. An entry is dropped only once
 * its window and its block are both over. A table full of live entries
 * leaves a new address untracked: never blocked, its failures uncounted.
 * There is no shared block and no per-user block, so one source can never
 * lock out another (a real streamer included); the handshake rate limit
 * (max_handshakes_per_ip_per_sec) is the backstop. The pending notes are a
 * CSLSSockTable (R43): full, it drops the oldest note, and a note lives
 * SLS_AUTH_PENDING_TTL_MS at most.
 *
 * Thread-safe; called on libsrt's receive thread (callback, log handler)
 * and the workers (handshake_done). The mutex covers map operations only.
 */
class CSLSAuthThrottle
{
public:
    CSLSAuthThrottle(int max_fail = SLS_AUTH_FAIL_MAX, int window_ms = SLS_AUTH_FAIL_WINDOW_MS,
                     int block_ms = SLS_AUTH_BLOCK_MS, size_t max_addrs = SLS_AUTH_MAX_ADDRS,
                     size_t max_pending = SLS_AUTH_MAX_PENDING);

    // Is ip blocked at now_ms? *left_ms (if given): how much longer.
    bool   blocked(const char *ip, int64_t now_ms, int *left_ms = NULL);
    // A handshake on socket ns from ip on which libsrt will check a
    // passphrase. Never refuses (see above).
    void   note_handshake(int ns, const char *ip, int64_t now_ms);
    // The socket was accepted: its passphrase was right.
    void   handshake_done(int ns);
    // libsrt rejected socket ns for a wrong passphrase. true if this failure
    // started a block; then *ip (if given) says for which address.
    bool   bad_secret(int ns, int64_t now_ms, std::string *ip = NULL);
    size_t tracked() const;
    size_t pending() const;

private:
    struct entry {
        int     fails;
        int64_t window_start_ms;
        int64_t blocked_until_ms;
    };
    struct pend {
        std::string ip;      // as logged
        std::string addr;    // its sls_ip_key
    };
    int                          m_max_fail;
    int                          m_window_ms;
    int                          m_block_ms;
    size_t                       m_max_addrs;
    mutable CSLSMutex            m_mutex;
    std::map<std::string, entry> m_addrs;
    int64_t                      m_pruned_ms;
    CSLSSockTable<pend>          m_pending;   // its own lock

    bool expired(const entry &e, int64_t now_ms) const {
        return e.blocked_until_ms <= now_ms && now_ms - e.window_start_ms >= m_window_ms;
    }
};

// the process-wide throttle (one for every listener)
CSLSAuthThrottle &sls_auth_throttle();

#endif
