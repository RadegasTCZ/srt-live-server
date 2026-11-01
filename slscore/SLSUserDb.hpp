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



#ifndef _SLSUserDb_INCLUDE_
#define _SLSUserDb_INCLUDE_

#include <map>
#include <string>
#include <atomic>
#include <stdint.h>

#include "SLSLock.hpp"

/**
 * One user loaded from user_file (extensible record, not a fixed tuple).
 * Empty publish_pass  => publishers may push that stream in plaintext.
 * Empty play_pass     => players may pull that stream in plaintext.
 * The generations change whenever a reload changes that direction's
 * passphrase (CSLSUserDb::replace); an admitted session keeps the one it
 * was admitted with.
 */
struct sls_user_t {
    std::string name;
    std::string publish_pass;  // "" = plaintext allowed
    std::string play_pass;
    uint64_t    publish_gen;
    uint64_t    play_gen;
    // reserved for web UI project: ip allow/deny, endpoints
    sls_user_t() : publish_gen(0), play_gen(0) {}
};

// CSLSUserDb::check results
enum SLS_AUTH_STATE {
    SLS_AUTH_VALID        = 0,
    SLS_AUTH_USER_REMOVED = 1,
    SLS_AUTH_PASS_CHANGED = 2,
};

/**
 * The users of one listener's user_file. Shared (std::shared_ptr) by the
 * listener and every role it admitted, so a role on any worker can re-check
 * its credentials after a reload, whatever happens to the listener.
 * Thread-safe: the listen callback (libsrt's thread) and the workers read,
 * the main thread replaces. No lock is ever held across a system call.
 */
class CSLSUserDb
{
public:
    CSLSUserDb() : m_epoch(1), m_next_gen(1) {}

    // Swap in a new set (staging is left with the old one). A passphrase
    // that did not change keeps its generation, a changed or new one gets a
    // fresh one. Then the epoch moves on: every role re-checks.
    void     replace(std::map<std::string, sls_user_t> &staging);
    // A direction's passphrase ("" = plaintext), its generation and the
    // current epoch, read together; false if there is no such user.
    bool     lookup(const std::string &name, bool publish, std::string *pass,
                    uint64_t *gen, uint64_t *epoch) const;
    bool     contains(const std::string &name) const;
    size_t   size() const;
    // Changes on every replace(); cheap, for a per-tick "anything new?" test.
    uint64_t epoch() const { return m_epoch.load(); }
    // Is an admission (name, direction, generation) still valid? *epoch (if
    // given) gets the epoch the answer belongs to.
    int      check(const std::string &name, bool publish, uint64_t gen, uint64_t *epoch) const;
    static const char *state_str(int state);

private:
    mutable CSLSRWLock                 m_lock;
    std::map<std::string, sls_user_t>  m_users;
    std::atomic<uint64_t>              m_epoch;      // written under the write lock
    uint64_t                           m_next_gen;   // under the write lock
};

#endif
