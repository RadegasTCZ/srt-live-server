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
#include <set>
#include <string>
#include <vector>
#include <stdint.h>

#include "SLSLock.hpp"
#include "SLSSockTable.hpp"

#define SLS_AUTH_FAIL_MAX        5        // wrong passphrases ...
#define SLS_AUTH_FAIL_WINDOW_MS  60000    // ... within this long ...
#define SLS_AUTH_BLOCK_MS        60000    // ... refuse that user from that address this long
#define SLS_AUTH_MAX_KEYS        16384    // (address, user) pairs tracked
#define SLS_AUTH_MAX_PER_ADDR    8        // of them for one address (IPv6: one /64)
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
 * Bounded, and nobody's failures can ever block anyone else (R44): only a
 * tracked pair is ever blocked, and an untracked one never is. To track a
 * new pair in a full table, the entry dropped is the one least worth
 * keeping: an unblocked one first (fewest failures, then the least recently
 * failed), the oldest block only when every entry is blocked. One address
 * holds at most SLS_AUTH_MAX_PER_ADDR pairs (past that, its own least
 * worth keeping one goes), so a source rotating user names cannot push other
 * sources' counts out. Failures that go uncounted this way are bounded by
 * the handshake rate limit (max_handshakes_per_ip_per_sec). The pending notes
 * are a CSLSSockTable (R43): full, it drops the oldest note, and a note
 * lives SLS_AUTH_PENDING_TTL_MS at most.
 *
 * Thread-safe; called on libsrt's receive thread (callback, log handler)
 * and the workers (handshake_done). The mutex covers map operations only;
 * every operation is O(log n) (plus a scan of one address's few pairs).
 */
class CSLSAuthThrottle
{
public:
    CSLSAuthThrottle(int max_fail = SLS_AUTH_FAIL_MAX, int window_ms = SLS_AUTH_FAIL_WINDOW_MS,
                     int block_ms = SLS_AUTH_BLOCK_MS, size_t max_keys = SLS_AUTH_MAX_KEYS,
                     size_t max_pending = SLS_AUTH_MAX_PENDING, size_t max_per_addr = SLS_AUTH_MAX_PER_ADDR);

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
    size_t tracked_keys_of(const char *ip) const;
    size_t pending() const;

private:
    // eviction order, least worth keeping first: (0, fails, last failure)
    // for an unblocked entry, (1, block end, 0) for a blocked one; the key
    // last makes it unique
    struct ord_t {
        int         cls;
        int64_t     a;
        int64_t     b;
        std::string key;
        bool operator<(const ord_t &o) const {
            if (cls != o.cls) return cls < o.cls;
            if (a != o.a)     return a < o.a;
            if (b != o.b)     return b < o.b;
            return key < o.key;
        }
    };
    struct entry {
        int         fails;
        int64_t     window_start_ms;
        int64_t     blocked_until_ms;
        int64_t     last_ms;
        std::string addr;     // its sls_ip_key
        ord_t       ord;      // as stored in m_order
    };
    struct pend {
        std::string ip;      // as logged
        std::string addr;
        std::string key;
        std::string user;
    };
    int                          m_max_fail;
    int                          m_window_ms;
    int                          m_block_ms;
    size_t                       m_max_keys;
    size_t                       m_max_per_addr;
    mutable CSLSMutex            m_mutex;
    std::map<std::string, entry> m_keys;
    std::set<ord_t>              m_order;
    std::map<std::string, std::vector<std::string> > m_by_addr;
    CSLSSockTable<pend>          m_pending;   // its own lock

    void set_ord_locked(const std::string &key, entry &e, int64_t now_ms);
    void remove_locked(std::string key);
};

// the process-wide throttle (one for every listener)
CSLSAuthThrottle &sls_auth_throttle();

#endif
