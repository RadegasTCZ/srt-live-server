
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


#ifndef _SLSListener_INCLUDE_
#define _SLSListener_INCLUDE_

#include <map>
#include <string>

#include <sys/socket.h>
#include <srt/srt.h>

#include "SLSRole.hpp"
#include "SLSRoleList.hpp"
#include "SLSPublisher.hpp"
#include "conf.hpp"
#include "SLSRecycleArray.hpp"
#include "SLSMapPublisher.hpp"
#include "SLSMapRelay.hpp"
#include "SLSLock.hpp"
#include "SLSStreamId.hpp"

/**
 * server conf
 */
SLS_CONF_DYNAMIC_DECLARE_BEGIN(server)
char             domain_player[URL_MAX_LEN];
char             domain_publisher[URL_MAX_LEN];
int              listen;
int              backlog;
int              latency;
int              idle_streams_timeout;//unit s; -1: unlimited
char             on_event_url[URL_MAX_LEN];
char             srt_passphrase[STR_MAX_LEN];
int              srt_pbkeylen;
char             user_file[URL_MAX_LEN];
int              admission_timeout;//unit ms
int              on_event_timeout;//unit ms
SLS_CONF_DYNAMIC_DECLARE_END

/**
 * sls_conf_server_t
 */
SLS_CONF_CMD_DYNAMIC_DECLARE_BEGIN(server)
SLS_SET_CONF(server, string, domain_player,        "play domain", 1,    URL_MAX_LEN-1),
SLS_SET_CONF(server, string, domain_publisher,     "", 1,    URL_MAX_LEN-1),
SLS_SET_CONF(server, int,    listen,               "listen port", 1024, 10000),
SLS_SET_CONF(server, int,    backlog,              "how many sockets may be allowed to wait until they are accepted", 1,    1024),
SLS_SET_CONF(server, int,    latency,              "latency.", 1, 300),
SLS_SET_CONF(server, int,    idle_streams_timeout, "players idle timeout when no publisher" , -1, 86400),
SLS_SET_CONF(server, string, on_event_url,         "on connect/close http url", 1,    URL_MAX_LEN-1),
SLS_SET_CONF(server, string, srt_passphrase,       "listener-wide SRT passphrase; empty = no encryption", 0,  STR_MAX_LEN-1),
SLS_SET_CONF(server, int,    srt_pbkeylen,         "SRT AES key length: 0=libsrt default, 16/24/32 = AES-128/192/256", 0, 32),
SLS_SET_CONF(server, string, user_file,            "path to per-user passphrase file (grant/revoke via edit + reload)", 0, URL_MAX_LEN-1),
SLS_SET_CONF(server, int,    admission_timeout,    "ms an accepted connection may wait for a worker before it is closed (default 5000)", 100, 60000),
SLS_SET_CONF(server, int,    on_event_timeout,     "ms the on_event_url hook may take to answer on_connect before the role is refused (default 2000)", 100, 30000),
SLS_CONF_CMD_DYNAMIC_DECLARE_END

#define SLS_DEFAULT_ADMISSION_TIMEOUT 5000
#define SLS_DEFAULT_ON_EVENT_TIMEOUT  2000
// connections accepted at most by the listener's one-off drain (on_added_to_epoll)
#define SLS_LISTENER_DRAIN_MAX 1024
#define SLS_DEFAULT_IDLE_STREAMS_TIMEOUT 10   // seconds; 0 would drop a role the moment it is idle
inline void sls_conf_server_defaults(sls_conf_server_t *c)
{
    c->idle_streams_timeout = SLS_DEFAULT_IDLE_STREAMS_TIMEOUT;
    c->admission_timeout = SLS_DEFAULT_ADMISSION_TIMEOUT;
    c->on_event_timeout  = SLS_DEFAULT_ON_EVENT_TIMEOUT;
}

/**
 * One user loaded from user_file (extensible record, not a fixed tuple).
 * Empty publish_pass  => publishers may push that stream in plaintext.
 * Empty play_pass     => players may pull that stream in plaintext.
 */
struct sls_user_t {
    std::string name;
    std::string publish_pass;  // "" = plaintext allowed
    std::string play_pass;
    // reserved for web UI project: ip allow/deny, endpoints
};


/**
 * SLSListener
 */
class CSLSListener : public CSLSRole
{
public :
	CSLSListener();
    ~CSLSListener();

    int         init();
    int         uninit();


    virtual int start();
    virtual int stop();

    virtual int  handler();
    // The worker's epoll holds the socket now: accept what queued before (see the .cpp)
    virtual int  on_added_to_epoll();
    // SIGHUP: re-resolve the on_event_url host (failure keeps the previous address)
    void reload_hook();

    void set_role_list(CSLSRoleList *list_role);
    void set_map_publisher(CSLSMapPublisher * publisher);
    void set_map_puller(CSLSMapRelay *map_puller);
    void set_map_pusher(CSLSMapRelay *map_puller);
    // shared admission limiter (owned by CSLSManager); NULL = no caps
    void set_admission_limiter(CSLSConnLimiter *limiter);


private:
    CSLSRoleList      * m_list_role;
    CSLSMapPublisher  * m_map_publisher;
    CSLSMapRelay      * m_map_puller;
    CSLSMapRelay      * m_map_pusher;

    CSLSMutex           m_mutex;

    int                 m_idle_streams_timeout_role;
    int                 m_admission_timeout;   // ms, from the server block
    CSLSConnLimiter   * m_admission_limiter;
    char                m_http_url_role[URL_MAX_LEN];
    int                 m_on_event_timeout;    // ms
    // on_event_url's server, resolved at start and on reload (never on a hot path);
    // roles copy it under m_hook_mutex when the listener creates them
    CSLSMutex           m_hook_mutex;
    struct sockaddr_storage m_hook_addr;
    socklen_t           m_hook_addr_len;

    // resolve m_http_url_role's host into m_hook_addr; on failure the old address is kept
    int  resolve_hook();
    void attach_hook(CSLSRole *role);

    CSLSRWLock                             m_users_lock;
    std::map<std::string, sls_user_t>      m_users;

    int  init_conf_app();
    // one INFO line per second at most for accept-time refusals
    void log_admission_refusal(const char *peer_name, int peer_port, int why);
    // Role of a parsed streamid by host/app (never by app name alone); on
    // PUBLISHER/PLAYER *app_key_out is the publisher-side "host/app" key.
    sls_sid_role_t classify_sid(const sls_sid_t &sid, std::string *app_key_out);
    // Parse the whole file into a staging map; SLS_OK only if every line is
    // valid and there is at least one user, then swap it into m_users. On any
    // error m_users is left untouched and *err (if given) names file:line.
    int  load_users(const char *path, std::string *err);

    static int listen_callback(void *opaq, SRTSOCKET ns, int hsversion,
                               const struct sockaddr *peeraddr, const char *streamid);

public:
    // Re-read the user_file configured for this listener and swap it into
    // m_users under a write lock. Safe to call from the main thread while
    // listen_callback is firing on SRT's internal thread. Returns SLS_ERROR
    // (and logs ERROR "reload failed") if the file is rejected; the previous
    // users stay in force.
    int reload_users();
};


#endif
