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

#include "SLSLock.hpp"

/**
 * Admission slots for listener-accepted connections (publishers and players).
 * One limiter per CSLSManager, shared by all its listeners. Outbound relay
 * connections (pullers, pushers) never take a slot.
 *
 * A slot is taken at accept (try_acquire) and handed to the role that owns the
 * socket (CSLSRole::set_conn_limiter); the role gives it back exactly once, in
 * CSLSRole::uninit (which the destructor also runs).
 */
enum SLS_CONN_REFUSAL {
    SLS_CONN_OK     = 0,
    SLS_CONN_TOTAL  = 1,   // max_total reached
    SLS_CONN_PER_IP = 2,   // max_per_ip reached for this address
};

class CSLSConnLimiter
{
public:
    CSLSConnLimiter(int max_total, int max_per_ip);   // 0 = unlimited for per-ip
    ~CSLSConnLimiter();

    // thread-safe; on refusal *why (if given) says which cap was hit
    bool try_acquire(const char *ip, int *why = NULL);
    void release(const char *ip);
    int  total() const;
    int  max_total() const  { return m_max_total; }
    int  max_per_ip() const { return m_max_per_ip; }

private:
    int                         m_max_total;
    int                         m_max_per_ip;
    int                         m_total;
    std::map<std::string, int>  m_per_ip;
    mutable CSLSMutex           m_mutex;
};

#endif
