
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


#ifndef _SLSRelay_INCLUDE_
#define _SLSRelay_INCLUDE_

#include <list>

#include "SLSRole.hpp"
#include "SLSMapPublisher.hpp"

/**
 * sls_conf_relay_t
 */

SLS_CONF_DYNAMIC_DECLARE_BEGIN(relay)
char             type[32];
char             mode[32];
char             upstreams[1024];
int              reconnect_interval;
int              idle_streams_timeout;
int              max_pull_streams;
SLS_CONF_DYNAMIC_DECLARE_END


/**
 * relay cmd declare
 */

SLS_CONF_CMD_DYNAMIC_DECLARE_BEGIN(relay)
SLS_SET_CONF(relay, string, type,                 "pull, push",  1, 31),
SLS_SET_CONF(relay, string, mode,                 "relay mode.", 1, 31),
SLS_SET_CONF(relay, string, upstreams,            "upstreams",   1, 1023),
SLS_SET_CONF(relay, int,    reconnect_interval,   "reconnect interval, unit s", 1, 3600),
SLS_SET_CONF(relay, int,    idle_streams_timeout, "idle streams timeout, unit s", -1, 3600),
SLS_SET_CONF(relay, int,    max_pull_streams,     "pull: max concurrent pulled streams for this app (default 16)", 1, 1024),

SLS_CONF_CMD_DYNAMIC_DECLARE_END

#define SLS_DEFAULT_MAX_PULL_STREAMS 16
inline void sls_conf_relay_defaults(sls_conf_relay_t *c)
{
    c->max_pull_streams = SLS_DEFAULT_MAX_PULL_STREAMS;
}

// Startup check of a relay block; SLS_OK, or SLS_ERROR with *err set
// (e.g. no upstreams: hash mode would divide by zero).
int sls_relay_conf_check(sls_conf_relay_t *cr, std::string *err);


enum SLS_PULL_MODE {
	SLS_PM_LOOP  = 0,
	SLS_PM_HASH  = 1,
	SLS_PM_ALL   = 2,
};

/**
 * CSLSRelay
 */
class CSLSRelay: public CSLSRole
{
public :
	CSLSRelay();
    virtual ~CSLSRelay();

    virtual int uninit();

    void  set_map_publisher(CSLSMapPublisher *publisher);
    // takes a reference on the manager, dropped in uninit or the destructor
    void  set_relay_manager(void *relay_manager);
    // the relay has ended: stop counting it as live (its manager, under the
    // manager's mutex); idempotent, and done by release otherwise
    void  leave_live();
    void *get_relay_manager();
    char *get_url();

    int           open(const char *url);
    virtual int   close();
    virtual int   get_peer_info(char *peer_name, int &peer_port);
    virtual int   get_stat_base(char *stat_base) ;
protected:
    char         m_url[URL_MAX_LEN];
    char         m_upstream[URL_MAX_LEN];
    char         m_server_ip[IP_MAX_LEN];
    int          m_server_port;

    CSLSMapPublisher  *m_map_publisher;
    void              *m_relay_manager;
    bool               m_live_counted;

    int          parse_url(char* url, char * ip, size_t ip_size, int& port, char * streamid, size_t streamid_size);
    void         release_relay_manager();

};


#endif
