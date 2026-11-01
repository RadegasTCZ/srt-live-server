
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


#include "SLSGroup.hpp"
#include "SLSLog.hpp"
#include "SLSListener.hpp"

#define POLLING_TIME 1 /// Time in milliseconds between interrupt check

/**
 * CSLSGroup class implementation
 */

CSLSGroup::CSLSGroup()
{
    m_list_role          = NULL;
    m_worker_connections = 100;
    m_worker_number      = 0;
    m_intake_stopped     = false;

    m_stat_post_last_tm_ms = sls_gettime_ms();
    m_stat_post_interval   = 5;//5s default
}

CSLSGroup::~CSLSGroup()
{
}

int CSLSGroup::start()
{
    sls_log(SLS_LOG_INFO, "[%p]CSLSGroup::start, worker_number=%d.", this, m_worker_number);
	//do something here
    return CSLSEpollThread::start();

}

int CSLSGroup::stop()
{
	int ret = 0;
    sls_log(SLS_LOG_INFO, "[%p]CSLSGroup::stop, worker_number=%d.", this, m_worker_number);
	ret = CSLSEpollThread::stop();
    // With a thread, its last act was clear() and this is a no-op. In
    // single-thread mode (worker_threads 0) there was no thread: the roles,
    // the listeners among them, are released here, on the thread that
    // polled them.
    clear();

    // the worker has stopped: give the last on_close requests a moment to leave
    int64_t drain_until = sls_gettime_ms() + 300;
    while (!m_list_wait_close.empty() && sls_gettime_ms() < drain_until) {
        check_wait_close();
        if (!m_list_wait_close.empty()) {
            msleep(5);
        }
    }
    for (std::list<CloseWait>::iterator it = m_list_wait_close.begin(); it != m_list_wait_close.end(); it++) {
        it->client->close();
        delete it->client;
    }
    m_list_wait_close.clear();
    sls_log(SLS_LOG_INFO, "[%p]CSLSGroup::stop, m_list_wait_close.clear, worker_number=%d.", this, m_worker_number);
	return ret;
}

void CSLSGroup::check_new_role() {

    // first, check rolelist
    if (NULL == m_list_role || m_intake_stopped)
        return ;
    if (m_map_role.size() >= m_worker_connections)
        return;

    CSLSRole * role = m_list_role->pop();
    if (NULL == role)
        return ;

    int fd = role->get_fd();
    if (fd == 0) {
        //invalid role
        relay_ended(role);
        delete role;
        return ;
    }

    // A users reload revoked it while it was queued (S4): this worker is the
    // first owner, so it closes it before announcing anything (no on_connect,
    // so no on_close is owed).
    if (role->auth_revoked()) {
        role->uninit();
        delete role;
        return ;
    }

    //add to epoll
    if (0 == role->add_to_epoll(m_eid) && 0 == role->on_added_to_epoll()) {
        m_map_role[fd] = role;
        // Announce the role only now that a worker owns it, so on_connect and
        // on_close always pair: a queued role that expires (or is erased at
        // shutdown) sent neither, and an announced one always gets its on_close
        // (even when the hook refused it or timed out). The admission gate is
        // unchanged: until the hook answers 200, check_http_passed holds the
        // role's reads/writes, and a failure closes it. No-op for roles without
        // on_event_url (listener, pullers, pushers).
        role->on_connect();
        if (!role->hook_pending()) {
            role->session_start();     // no hook: admitted once a worker owns it
        }
        sls_log(SLS_LOG_DEBUG, "[%p]CSLSGroup::check_new_role, worker_number=%d, %s=%p, add_to_epoll fd=%d, role_map.size=%zu.",
                this, m_worker_number, role->get_role_name(), role, fd, m_map_role.size());
    } else {
        sls_log(SLS_LOG_INFO, "[%p]CSLSGroup::check_new_role, worker_number=%d, %s=%p, add_to_epoll or on_added_to_epoll failed, fd=%d.",
                this, m_worker_number, role->get_role_name(), role, fd);
        relay_ended(role);
        drop_role(role);
    }
}

int CSLSGroup::handler()
{
	int ret = 0;
	int i;
    int read_len  = MAX_SOCK_COUNT;
    int write_len = MAX_SOCK_COUNT;

    int handler_count = 0;

    //check epoll event
    ret = srt_epoll_wait(m_eid, m_read_socks, &read_len, m_write_socks, &write_len, POLLING_TIME, 0, 0, 0, 0);
    if (ret < 0) {
        //sls_log(SLS_LOG_TRACE, "[%p]CSLSGroup::handle, worker_number=%d, srt_epoll_wait, no epoll event, ret=%d.",
        //        this, m_worker_number, ret);
    	ret = srt_getlasterror(NULL);
        if (ret == SRT_ETIMEOUT)//6003
            ret = SLSERROR(EAGAIN);
        else
            ret = CSLSSrt::libsrt_neterrno();

    	idle_check();
        return handler_count;
    }

    sls_log(SLS_LOG_TRACE, "[%p]CSLSGroup::handle, worker_number=%d, writable sock count=%d, readable sock count=%d.",
        this, m_worker_number, write_len, read_len);

    // A socket in the epoll without a role is a bug (a role leaves the epoll
    // before its socket is closed), but if one is ever left there the epoll
    // reports it on every wait: those lines go through the peer-line budget
    // (1/s, the rest at DEBUG), so a leak shows without flooding the log.
    for (i = 0; i < write_len; i ++) {
        std::map<int, CSLSRole *>::iterator it = m_map_role.find(m_write_socks[i]);
        if (it == m_map_role.end()) {
            sls_peer_log(SLS_LOG_WARNING, "[%p]CSLSGroup::handle, worker_number=%d, no role map writable sock=%d,why?",
                    this, m_worker_number, m_write_socks[i]);
            continue;
        }

        CSLSRole* role = it->second;
        if (!role) {
            sls_peer_log(SLS_LOG_WARNING, "[%p]CSLSGroup::handle, worker_number=%d, role is null, writable sock=%d,why?",
                    this, m_worker_number, m_write_socks[i]);
            continue;
        }

        ret = role->handler();
        if (ret < 0) {
        	//handle exception
            sls_log(SLS_LOG_TRACE, "[%p]CSLSGroup::handle, worker_number=%d, write sock=%d is invalid, %s=%p, write_len=%d, role_map.size=%zu.",
                    this, m_worker_number, m_write_socks[i], role->get_role_name(), role, write_len, m_map_role.size());
        	role->set_end_reason_from_sock();   // before the close: the socket says why
        	role->invalid_srt();
        } else {
            handler_count += ret;
        }
    }

    for (i = 0; i < read_len; i ++) {
        std::map<int, CSLSRole *>::iterator it = m_map_role.find(m_read_socks[i]);
        if (it == m_map_role.end()) {
            sls_peer_log(SLS_LOG_WARNING, "[%p]CSLSGroup::handle, worker_number=%d, no role map readable sock=%d,why?",
                    this, m_worker_number, m_read_socks[i]);
            continue;
        }

        CSLSRole* role = it->second;
        if (!role) {
            sls_peer_log(SLS_LOG_WARNING, "[%p]CSLSGroup::handle, worker_number=%d, role is null, readable sock=%d,why?",
                     this, m_worker_number, m_read_socks[i]);
            continue;
        }

        ret = role->handler();
        if (ret < 0) {
        	//handle exception
            sls_log(SLS_LOG_TRACE, "[%p]CSLSGroup::handle, worker_number=%d, readable sock=%d is invalid, %s=%p, readable len=%d, role_map.size=%zu.",
                      this, m_worker_number, m_read_socks[i], role->get_role_name(), role, read_len, m_map_role.size());
        	role->set_end_reason_from_sock();   // before the close: the socket says why
        	role->invalid_srt();
        } else {
            handler_count += ret;
        }
    }

	idle_check();
    if (0 == handler_count) {
    	//release cpu
        msleep(POLLING_TIME);
    }
	return handler_count;
}

void CSLSGroup::idle_check()
{
	check_wait_close();
	check_reconnect_relay();
    check_invalid_sock();
    check_new_role();
}

// A gone role's on_close request is taken over here and polled (zero-timeout poll,
// never blocks) until the hook has answered or the request times out.
void CSLSGroup::adopt_close_client(CSLSRole *role)
{
    CHttpClient *client = role->take_close_client();
    if (NULL == client) {
        return;
    }
    CloseWait w;
    w.client      = client;
    w.deadline_ms = sls_gettime_ms() + SLS_ON_CLOSE_TIMEOUT_MS;
    m_list_wait_close.push_back(w);
    sls_log(SLS_LOG_DEBUG, "[%p]CSLSGroup::adopt_close_client, worker_number=%d, %s=%p, on_close pending, waiting=%zu.",
            this, m_worker_number, role->get_role_name(), role, m_list_wait_close.size());
}

void CSLSGroup::check_wait_close()
{
    if (m_list_wait_close.empty()) {
        return;
    }
    int64_t now = sls_gettime_ms();
    for (std::list<CloseWait>::iterator it = m_list_wait_close.begin(); it != m_list_wait_close.end();) {
        CHttpClient *client = it->client;
        client->handler();
        client->check_timeout(now);
        if (client->has_result() || now >= it->deadline_ms) {
            sls_log(SLS_LOG_DEBUG, "[%p]CSLSGroup::check_wait_close, worker_number=%d, on_close done, status=%d.",
                    this, m_worker_number, client->get_status());
            client->close();
            delete client;
            it = m_list_wait_close.erase(it);
        } else {
            it++;
        }
    }
}

void CSLSGroup::check_reconnect_relay()
{
	int64_t cur_time_ms = sls_gettime_ms();//m_cur_time_microsec;

	CSLSRelayManager * relay_manager = NULL;
    std::list<CSLSRelayManager * >::iterator it_erase;
    std::list<CSLSRelayManager * >::iterator it;
    for ( it = m_list_reconnect_relay_manager.begin(); it != m_list_reconnect_relay_manager.end();)
    {
    	CSLSRelayManager * relay_manager = *it;
        if (NULL == relay_manager)
        {
			sls_log(SLS_LOG_INFO, "[%p]CSLSGroup::check_reconnect_relay, worker_number=%d, remove invalid relay_manager.",
					this, m_worker_number);
    		it_erase = it;
    		it ++;
    		m_list_reconnect_relay_manager.erase(it_erase);
        	continue;
        }
        int ret = relay_manager->reconnect(cur_time_ms);
        if (SLS_OK != ret) {
        	it ++;
        	continue;
        }
		it_erase = it;
		it ++;
		m_list_reconnect_relay_manager.erase(it_erase);
		relay_manager->release();   // this entry's reference
    }
}

void CSLSGroup::relay_ended(CSLSRole *role)
{
	if (!role->is_reconnect()) {
		return;
	}
	CSLSRelay * relay = (CSLSRelay *)role;
	CSLSRelayManager * relay_manager = (CSLSRelayManager *)relay->get_relay_manager();
	if (NULL != relay_manager && relay_manager->relay_ended(relay, true)) {
		relay_manager->add_ref();   // held by the reconnect-list entry
		m_list_reconnect_relay_manager.push_back(relay_manager);
		sls_log(SLS_LOG_INFO, "[%p]CSLSGroup::relay_ended, worker_number=%d, %s=%p, need reconnect.",
			  this, m_worker_number, role->get_role_name(), role);
	}
}

void CSLSGroup::check_invalid_sock()
{
	bool update_stat_info = false;
	std::string stat_info;      // this tick's snapshot, published whole after the loop
	int64_t cur_time_ms = sls_gettime_ms();
	int d = cur_time_ms - m_stat_post_last_tm_ms;
	if (d >= m_stat_post_interval*1000) {
		update_stat_info = true;
		m_stat_post_last_tm_ms = cur_time_ms ;
	}

    std::map<int, CSLSRole *>::iterator it;
    std::map<int, CSLSRole *>::iterator it_erase;
    for(it=m_map_role.begin(); it!=m_map_role.end();) {
        CSLSRole * role = it->second;
		it_erase = it;
		it ++;
        if (!role) {
			m_map_role.erase(it_erase);
            continue;
        }

        if (update_stat_info) {
            std::string role_info = role->get_stat_info();
            if (!role_info.empty()) {
                if (!stat_info.empty()) {
                    stat_info += ",";
                }
                stat_info += role_info;
            }
        }

        int state = role->get_state(cur_time_ms);
        if (SLS_RS_INVALID == state || SLS_RS_UNINIT == state)
		{
			sls_log(SLS_LOG_DEBUG, "[%p]CSLSGroup::check_invalid_sock, worker_number=%d, %s=%p, invalid sock=%d, state=%d, role_map.size=%zu.",
				  this, m_worker_number, role->get_role_name(), role, role->get_fd(), state, m_map_role.size());
			//check relay: reconnect, or retire its manager
			relay_ended(role);

			role->uninit();
			adopt_close_client(role);
			sls_log(SLS_LOG_DEBUG, "[%p]CSLSGroup::check_invalid_sock, worker_number=%d, %s=%p, delete.",
				  this, m_worker_number, role->get_role_name(), role);
			drop_role(role);
			m_map_role.erase(it_erase);
			continue;
		}
    }

    if (update_stat_info) {
        CSLSLock lock(&m_mutex_stat);
        m_stat_info.swap(stat_info);
    }
}

// Frees a role the worker gave up on. A listener is only closed and kept:
// the manager still holds its pointer (SIGHUP reloads it from the main
// thread), and it is marked lost, which makes the main loop stop sls.
void CSLSGroup::drop_role(CSLSRole *role)
{
    if (role->is_listener()) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSGroup::drop_role, worker_number=%d, listener=%p stopped listening; sls stops.",
                this, m_worker_number, role);
        ((CSLSListener *)role)->set_lost();   // first: the main loop stops reloading it
        role->uninit();
        m_list_lost_listeners.push_back(role);
        return;
    }
    delete role;
}

void CSLSGroup::clear()
{
    sls_log(SLS_LOG_INFO, "[%p]CSLSGroup::clear, worker_number=%d, role_map.size=%zu.",
            this, m_worker_number, m_map_role.size());
    std::map<int, CSLSRole *>::iterator it;
    for(it=m_map_role.begin(); it!=m_map_role.end(); it++) {
        CSLSRole * role = it->second;
        if (role) {
            sls_log(SLS_LOG_INFO, "[%p]CSLSGroup::clear, worker_number=%d, delete %s=%p.",
                    this, m_worker_number, role->get_role_name(), role);
            role->set_end_reason("shutdown");
            role->uninit();
            adopt_close_client(role);
            delete role;
        }
    }
    m_map_role.clear();

    for (std::list<CSLSRole *>::iterator it_l = m_list_lost_listeners.begin(); it_l != m_list_lost_listeners.end(); it_l++) {
        delete *it_l;
    }
    m_list_lost_listeners.clear();

    std::list<CSLSRelayManager *>::iterator it_rm;
    for (it_rm = m_list_reconnect_relay_manager.begin(); it_rm != m_list_reconnect_relay_manager.end(); it_rm++) {
        if (*it_rm) (*it_rm)->release();
    }
    m_list_reconnect_relay_manager.clear();
}

void CSLSGroup::set_role_list(CSLSRoleList *list_role)
{
    m_list_role = list_role;
}

void CSLSGroup::set_worker_number(int n)
{
    m_worker_number = n;
}

void CSLSGroup::set_worker_connections(int n)
{
    m_worker_connections = n;
}

void CSLSGroup::set_stat_post_interval(int interval)
{
    m_stat_post_interval = interval;
}

// append this group's role objects, comma-separated, to info
void CSLSGroup::get_stat_info(std::string &info)
{
    CSLSLock lock(&m_mutex_stat);
    if (m_stat_info.empty()) {
        return;
    }
    if (!info.empty()) {
        info += ",";
    }
    info += m_stat_info;
}



