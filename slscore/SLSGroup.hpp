
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


#ifndef _SLSGroup_INCLUDE_
#define _SLSGroup_INCLUDE_

#include <map>
#include <atomic>

#include "SLSEpollThread.hpp"
#include "SLSRoleList.hpp"
#include "SLSRole.hpp"
#include "SLSMapRelay.hpp"
#include "HttpClient.hpp"

/**
 * CSLSGroup , group of players, publishers and listener
 */
class CSLSGroup : public CSLSEpollThread
{
public :
	CSLSGroup();
    virtual ~CSLSGroup();

    int  start();
    int  stop();
    // shutdown: adopt no more queued roles; CSLSManager::stop erases them
    void stop_intake() { m_intake_stopped = true; }

    void set_role_list(CSLSRoleList *list_role);
    void set_worker_connections(int n);
    void set_worker_number(int n);

    virtual int     handler();

    void set_stat_post_interval(int interval);
    void set_http_stat_post(CHttpClient *p);

    void get_stat_info(std::string &info);

protected:
    virtual void    clear();

private:
    CSLSRoleList                  *m_list_role;
    // on_close requests of roles that are already gone: fire-and-forget, polled by
    // this worker thread only (so no lock), each dropped once answered or past its deadline
    struct CloseWait {
        CHttpClient *client;
        int64_t      deadline_ms;
    };
    std::list<CloseWait>           m_list_wait_close;
    std::map<int, CSLSRole *>      m_map_role;
    std::list<CSLSRelayManager *>  m_list_reconnect_relay_manager;


    void  idle_check();
    void  check_reconnect_relay();
    void  check_invalid_sock();
    void  check_new_role();
    void  check_wait_close();
    void  adopt_close_client(CSLSRole *role);
    // a role leaves this worker: let a relay's manager decide on reconnect
    void  relay_ended(CSLSRole *role);

    int           m_worker_connections;
    int           m_worker_number;
    int64_t       m_cur_time_microsec;
    std::atomic<bool> m_intake_stopped;

    int64_t       m_stat_post_last_tm_ms;
    int           m_stat_post_interval;
    CSLSMutex     m_mutex_stat;
    std::string   m_stat_info;

};


#endif
