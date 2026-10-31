
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


#include <errno.h>
#include <string.h>


#include "SLSRoleList.hpp"
#include "SLSLog.hpp"
#include "SLSLock.hpp"

/**
 * CSLSRoleList class implementation
 */

CSLSRoleList::CSLSRoleList()
{
}
CSLSRoleList::~CSLSRoleList()
{
}

int CSLSRoleList::push(CSLSRole * role)
{
	if (role) {
	    queued_role q;
	    q.role      = role;
	    q.queued_ms = sls_gettime_ms();
	    CSLSLock lock(&m_mutex);
	    m_list_role.push_back(q);
	}
	return 0;
}

CSLSRole * CSLSRoleList::pop()
{
	CSLSLock lock(&m_mutex);
	CSLSRole * role = NULL;
    if (!m_list_role.empty()) {
        role = m_list_role.front().role;
        m_list_role.pop_front();
    }
	return role;
}

void CSLSRoleList::erase()
{
    CSLSLock lock(&m_mutex);
    sls_log(SLS_LOG_TRACE, "[%p]CSLSRoleList::erase, list.count=%zu", this, m_list_role.size());
    for (std::list<queued_role>::iterator it = m_list_role.begin(); it != m_list_role.end();)
    {
        CSLSRole * role = it->role;
        if (role) {
        	role->uninit();
            delete role;
        }
        it ++;
    }
    m_list_role.clear();
}

int CSLSRoleList::size()
{
    CSLSLock lock(&m_mutex);
	return m_list_role.size();
}

int CSLSRoleList::expire(int64_t now_ms, int max_age_ms)
{
    std::list<queued_role> expired;
    {
        CSLSLock lock(&m_mutex);
        for (std::list<queued_role>::iterator it = m_list_role.begin(); it != m_list_role.end();) {
            CSLSRole *role = it->role;
            int timeout = (role && role->get_admission_timeout() > 0) ? role->get_admission_timeout() : max_age_ms;
            if (role && role->holds_conn_slot() && timeout > 0 && now_ms - it->queued_ms >= timeout) {
                expired.push_back(*it);
                it = m_list_role.erase(it);
            } else {
                ++it;
            }
        }
    }
    // no worker can reach these any more; tear them down outside the lock
    int n = 0;
    for (std::list<queued_role>::iterator it = expired.begin(); it != expired.end(); ++it) {
        CSLSRole *role = it->role;
        char peer_ip[IP_MAX_LEN] = {0};
        int  peer_port = 0;
        role->get_peer_info(peer_ip, peer_port);
        sls_log(SLS_LOG_INFO, "[%p]CSLSRoleList::expire, expired queued %s=%p peer=%s:%d after %lld ms (admission_timeout=%d ms), closing it.",
                this, role->get_role_name(), role, peer_ip, peer_port, (long long)(now_ms - it->queued_ms),
                role->get_admission_timeout() > 0 ? role->get_admission_timeout() : max_age_ms);
        role->uninit();
        delete role;
        n++;
    }
    return n;
}




