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



#include "SLSUserDb.hpp"

void CSLSUserDb::replace(std::map<std::string, sls_user_t> &staging)
{
    CSLSLock lock(&m_lock, 1); // write lock
    for (std::map<std::string, sls_user_t>::iterator it = staging.begin(); it != staging.end(); ++it) {
        sls_user_t &u = it->second;
        std::map<std::string, sls_user_t>::const_iterator old = m_users.find(it->first);
        bool had = (old != m_users.end());
        u.publish_gen = (had && old->second.publish_pass == u.publish_pass) ? old->second.publish_gen : m_next_gen++;
        u.play_gen    = (had && old->second.play_pass    == u.play_pass)    ? old->second.play_gen    : m_next_gen++;
    }
    m_users.swap(staging);
    m_epoch++;
}

bool CSLSUserDb::lookup(const std::string &name, bool publish, std::string *pass,
                        uint64_t *gen, uint64_t *epoch) const
{
    CSLSLock lock(&m_lock, 0); // read lock
    std::map<std::string, sls_user_t>::const_iterator it = m_users.find(name);
    if (it == m_users.end()) {
        return false;
    }
    if (pass)  *pass  = publish ? it->second.publish_pass : it->second.play_pass;
    if (gen)   *gen   = publish ? it->second.publish_gen  : it->second.play_gen;
    if (epoch) *epoch = m_epoch.load();
    return true;
}

bool CSLSUserDb::contains(const std::string &name) const
{
    CSLSLock lock(&m_lock, 0); // read lock
    return m_users.count(name) > 0;
}

size_t CSLSUserDb::size() const
{
    CSLSLock lock(&m_lock, 0); // read lock
    return m_users.size();
}

int CSLSUserDb::check(const std::string &name, bool publish, uint64_t gen, uint64_t *epoch) const
{
    CSLSLock lock(&m_lock, 0); // read lock
    if (epoch) *epoch = m_epoch.load();
    std::map<std::string, sls_user_t>::const_iterator it = m_users.find(name);
    if (it == m_users.end()) {
        return SLS_AUTH_USER_REMOVED;
    }
    uint64_t now = publish ? it->second.publish_gen : it->second.play_gen;
    return now == gen ? SLS_AUTH_VALID : SLS_AUTH_PASS_CHANGED;
}

const char *CSLSUserDb::state_str(int state)
{
    switch (state) {
    case SLS_AUTH_USER_REMOVED: return "user removed";
    case SLS_AUTH_PASS_CHANGED: return "passphrase changed";
    default:                    return "valid";
    }
}
