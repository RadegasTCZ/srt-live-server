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

#define SLS_AUTH_FAIL_MAX        5        // wrong passphrases ...
#define SLS_AUTH_FAIL_WINDOW_MS  60000    // ... within this long ...
#define SLS_AUTH_BLOCK_MS        60000    // ... refuse that user from that address this long
#define SLS_AUTH_MAX_KEYS        16384    // (address, user) pairs tracked
#define SLS_AUTH_MAX_PENDING     4096     // passphrase handshakes between callback and outcome
#define SLS_AUTH_PENDING_TTL_MS  5000     // as the listener's records (R43)
// the "user" of a listener-wide srt_passphrase (no user_file): one count per
// address, whatever stream name each attempt gives (R44); never a valid name
#define SLS_AUTH_ANY_USER        "*"

/**
 * Wrong-passphrase throttle (S8). libsrt checks the passphrase after the
 * listen callback and tells the listener nothing when it fails: no accept,
 * no event, and srt_getrejectreason() on the dead socket says UNKNOWN. The
 * one trace is libsrt's own log line
 *   "@<socket>:newConnection: connection rejected due to: ... ERROR:BADSECRET"
 * which CSLSSrt's log handler turns into bad_secret(<socket>). So the
 * callback notes (note_handshake) which address and user each passphrase
 * handshake is for, and SLS_AUTH_FAIL_MAX failures of one (address, user)
 * within SLS_AUTH_FAIL_WINDOW_MS block that pair for SLS_AUTH_BLOCK_MS: the
 * callback refuses it (blocked()) before any key exchange, even with the
 * right passphrase. Other addresses and other users are not affected.
 *
 * Addresses are keyed by sls_ip_key (v4-mapped as IPv4, IPv6 per /64).
 * Bounded: past SLS_AUTH_MAX_KEYS pairs (after dropping the ones with
 * nothing left to remember) the untracked pairs share ONE overflow entry, so
 * a spray of (address, user) pairs blocks new pairs rather than going
 * uncounted. The pending notes are a CSLSSockTable (R43): full, it drops
 * the oldest note (that handshake's failure, if any, goes uncounted), and a
 * note lives SLS_AUTH_PENDING_TTL_MS at most.
 *
 * Thread-safe; called on libsrt's receive thread (callback, log handler)
 * and the workers (handshake_done). The mutex covers map operations only.
 */
class CSLSAuthThrottle
{
public:
    CSLSAuthThrottle(int max_fail = SLS_AUTH_FAIL_MAX, int window_ms = SLS_AUTH_FAIL_WINDOW_MS,
                     int block_ms = SLS_AUTH_BLOCK_MS, size_t max_keys = SLS_AUTH_MAX_KEYS,
                     size_t max_pending = SLS_AUTH_MAX_PENDING);

    // Is (ip, user) blocked at now_ms? *left_ms (if given): how much longer.
    bool   blocked(const char *ip, const std::string &user, int64_t now_ms, int *left_ms = NULL);
    // A handshake on socket ns from ip for user, on which libsrt will check
    // a passphrase. Never refuses (see above).
    void   note_handshake(int ns, const char *ip, const std::string &user, int64_t now_ms);
    // The socket was accepted: its passphrase was right.
    void   handshake_done(int ns);
    // libsrt rejected socket ns for a wrong passphrase. true if this failure
    // started a block; then *ip and *user (if given) say for whom.
    bool   bad_secret(int ns, int64_t now_ms, std::string *ip = NULL, std::string *user = NULL);
    size_t tracked_keys() const;
    size_t pending() const;

private:
    struct entry {
        int     fails;
        int64_t window_start_ms;
        int64_t blocked_until_ms;
    };
    struct pend {
        std::string ip;      // as logged
        std::string key;
        std::string user;
    };
    int                          m_max_fail;
    int                          m_window_ms;
    int                          m_block_ms;
    size_t                       m_max_keys;
    mutable CSLSMutex            m_mutex;
    std::map<std::string, entry> m_keys;
    entry                        m_overflow;
    CSLSSockTable<pend>          m_pending;   // its own lock
    int64_t                      m_keys_pruned_ms;

    // the entry key counts under: its own, or the overflow one when the table is full
    entry *find_locked(const std::string &key, bool insert, int64_t now_ms);
};

// the process-wide throttle (one for every listener)
CSLSAuthThrottle &sls_auth_throttle();

#endif
