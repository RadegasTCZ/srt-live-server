
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
SLS_CONF_CMD_DYNAMIC_DECLARE_END

/**
 * Per-stream credentials loaded from user_file.
 * Empty publish_pass  => publishers may push that stream in plaintext.
 * Empty play_pass     => players may pull that stream in plaintext.
 */
struct sls_user_cred_t {
    std::string publish_pass;
    std::string play_pass;
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

    void set_role_list(CSLSRoleList *list_role);
    void set_map_publisher(CSLSMapPublisher * publisher);
    void set_map_puller(CSLSMapRelay *map_puller);
    void set_map_pusher(CSLSMapRelay *map_puller);
    void set_record_hls_path_prefix(char *path);

    virtual std::string get_stat_info();

private:
    CSLSRoleList      * m_list_role;
    CSLSMapPublisher  * m_map_publisher;
    CSLSMapRelay      * m_map_puller;
    CSLSMapRelay      * m_map_pusher;

    CSLSMutex           m_mutex;

    int                 m_idle_streams_timeout_role;
    std::string 		m_stat_info;
    char                m_http_url_role[URL_MAX_LEN];
    char                m_record_hls_path_prefix[URL_MAX_LEN];

    CSLSRWLock                             m_users_lock;
    std::map<std::string, sls_user_cred_t> m_users;

    int  init_conf_app();
    int  load_users(const char *path);

    static int listen_callback(void *opaq, SRTSOCKET ns, int hsversion,
                               const struct sockaddr *peeraddr, const char *streamid);

public:
    // Re-read the user_file configured for this listener and swap it into
    // m_users under a write lock. Safe to call from the main thread while
    // listen_callback is firing on SRT's internal thread.
    int reload_users();
};


#endif
