
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


#ifndef _SLSManager_INCLUDE_
#define _SLSManager_INCLUDE_

#include <list>

#include "SLSRole.hpp"
#include "SLSRoleList.hpp"
#include "SLSGroup.hpp"
#include "SLSListener.hpp"
#include "conf.hpp"
#include "SLSMapData.hpp"
#include "SLSMapRelay.hpp"

/**
 * pid file used when the srt block has no 'pid_file' directive
 */
#define SLS_DEFAULT_PID_FILE "/run/sls/sls.pid"

/**
 * srt conf declare
 */
SLS_CONF_DYNAMIC_DECLARE_BEGIN(srt)
char             log_file[URL_MAX_LEN];
char             log_level[URL_MAX_LEN];
int              worker_threads;
int              worker_connections;
char             stat_post_url[URL_MAX_LEN];
int              stat_post_interval;
char             user[SHORT_STR_MAX_LEN];
char             group[SHORT_STR_MAX_LEN];
char             pid_file[URL_MAX_LEN];
int              max_connections_per_ip;
SLS_CONF_DYNAMIC_DECLARE_END


/**
 * srt cmd declare
 */
SLS_CONF_CMD_DYNAMIC_DECLARE_BEGIN(srt)
SLS_SET_CONF(srt, string, log_file,                    "save log file name.", 1, URL_MAX_LEN-1),
SLS_SET_CONF(srt, string, log_level,                   "log level", 1, URL_MAX_LEN-1),
SLS_SET_CONF(srt, int,    worker_threads,              "count of worker thread, if 0, only main thread.", 0, 100),
SLS_SET_CONF(srt, int,    worker_connections,          "max roles per worker; worker_connections x max(1, worker_threads) caps accepted connections", 1, 1024),
SLS_SET_CONF(srt, string, stat_post_url,               "statistic info post url", 1, URL_MAX_LEN-1),
SLS_SET_CONF(srt, int,    stat_post_interval,          "interval of statistic info post.", 1, 60),
SLS_SET_CONF(srt, string, user,                        "drop privileges to this user after bind", 1, SHORT_STR_MAX_LEN-1),
SLS_SET_CONF(srt, string, group,                       "drop privileges to this group after bind (defaults to user's primary group)", 1, SHORT_STR_MAX_LEN-1),
SLS_SET_CONF(srt, string, pid_file,                    "pid file path (default /run/sls/sls.pid); the directory must exist and be writable", 1, URL_MAX_LEN-1),
SLS_SET_CONF(srt, int,    max_connections_per_ip,      "max concurrent accepted connections from one IP (default 16); 0 = unlimited", 0, 10000),
SLS_CONF_CMD_DYNAMIC_DECLARE_END

#define SLS_DEFAULT_MAX_CONNECTIONS_PER_IP 16
#define SLS_DEFAULT_STAT_POST_INTERVAL 5   // seconds
inline void sls_conf_srt_defaults(sls_conf_srt_t *c)
{
    c->stat_post_interval = SLS_DEFAULT_STAT_POST_INTERVAL;
    c->max_connections_per_ip = SLS_DEFAULT_MAX_CONNECTIONS_PER_IP;
}


/**
 * CSLSManager , manage players, publishers and listener
 */
class CSLSManager
{
public :
	CSLSManager();
	virtual ~CSLSManager();


    int  start_listeners();   // bind + listen, no threads (call before dropping privileges)
    int  start_workers();     // groups and their threads (call after dropping privileges)
    int  stop();
    int  reload_users();
    // close queued roles past their listener's admission_timeout (main thread)
    int  expire_queued_roles(int64_t now_ms);
    // main thread: re-resolve relay upstreams marked stale (the workers never
    // resolve names: relay connects only read the cached addresses)
    void refresh_relay_upstreams();
    int  single_thread_handler();
    int  check_invalid();
    bool is_single_thread();
    // a worker dropped a listener after startup: sls no longer listens there
    bool listener_lost();

    void       get_stat_info(std::string &info);
    static int stat_client_callback(void *p, HTTP_CALLBACK_TYPE type, void *v, void* context);

private:
    std::list<CSLSListener *>     m_servers;
    int                           m_server_count;
    CSLSMapData                  *m_map_data;
    CSLSMapPublisher             *m_map_publisher;
    CSLSMapRelay                 *m_map_puller;
    CSLSMapRelay                 *m_map_pusher;

    std::list<CSLSGroup    *>     m_workers;
    int                           m_worker_threads;

    CSLSRoleList * m_list_role;
    CSLSGroup    * m_single_group;

    // admission slots for every listener of this manager; freed in stop()
    // after the last role that could hold a slot is gone
    CSLSConnLimiter * m_conn_limiter;
};


#endif
