
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


#ifndef _SLSRole_INCLUDE_
#define _SLSRole_INCLUDE_

#include <map>
#include <memory>
#include <string>


#include "SLSRole.hpp"
#include "SLSSrt.hpp"
#include "SLSMapData.hpp"
#include "conf.hpp"
#include "SLSLock.hpp"
#include "common.hpp"
#include "HttpClient.hpp"
#include "SLSConnLimiter.hpp"
#include "SLSUserDb.hpp"


enum SLS_ROLE_STATE {
    SLS_RS_UNINIT = 0,
    SLS_RS_INITED = 1,
    SLS_RS_INVALID = 2,
};

const int DATA_BUFF_SIZE    = 100 * 1316;
const int UNLIMITED_TIMEOUT = -1;

class CSLSMapPublisher;
class CSLSMapRelay;
class CSLSRoleList;

/**
 * What a listener-accepted role needs to become visible to others
 * (CSLSRole::admit): its server block's maps, role queue and port. They
 * are the manager's and freed only after every worker has stopped, so a
 * role may keep them for its whole life (never a pointer to the listener,
 * which another worker may free first at shutdown).
 */
struct sls_admit_ctx_t {
    CSLSMapData      *map_data;
    CSLSMapPublisher *map_publisher;
    CSLSMapRelay     *map_puller;
    CSLSMapRelay     *map_pusher;
    CSLSRoleList     *role_list;
    int               port;
    sls_admit_ctx_t() : map_data(NULL), map_publisher(NULL), map_puller(NULL),
                        map_pusher(NULL), role_list(NULL), port(0) {}
};
/**
 * CSLSRole , the base of player, publisher and listener
 */
#define SLS_ON_CLOSE_TIMEOUT_MS 2000   // an on_close request that is still unanswered is dropped

class CSLSRole
{
public :
	CSLSRole();
	virtual ~CSLSRole();


    virtual int init();
    virtual int uninit();
    virtual int handler();

    int         open(char *url);
    int         close();

    int         get_fd();
    int         set_eid(int eid);
    bool        is_write(){return m_is_write;};

    int         set_srt(CSLSSrt *srt);
    int         invalid_srt();

    int         write(const char *buf, int size);

    int         add_to_epoll(int eid);
    // Called by the worker right after add_to_epoll() succeeded. Non-zero
    // return: the role is dropped as if the add had failed.
    virtual int on_added_to_epoll() { return 0; }
    // A listener is owned by the manager too (it holds the pointer for
    // reloads): a group must never free one before its clear().
    virtual bool is_listener() const { return false; }
    int         remove_from_epoll();
    int         get_state(int64_t cur_time_microsec = 0);
    int         get_sock_state();
    char      * get_role_name();

    void        set_conf(sls_conf_base_t *conf);
    void        set_map_data(char *map_key, CSLSMapData *map_data);

    void        set_idle_streams_timeout(int timeout);
    bool        check_idle_streams_duration(int64_t cur_time_ms = 0);

    char      * get_streamid();
    bool        is_reconnect();

    // who this role is, for the stats document (set once, by whoever creates the role)
    void        set_stat_identity(const char *stream, const char *remote_ip, int remote_port);
    // Session log (one INFO line each, never rate-limited): a session starts
    // when the role is ADMITTED (right after a worker adopts a role without a
    // hook, or when the on_connect hook answers 200) and ends in uninit().
    // A refused or never-adopted role logs neither. No-op for roles that are
    // not reported (listeners).
    void        session_start();
    void        session_end();
    void        set_end_reason(const char *why) { if (NULL == m_end_reason) m_end_reason = why; }
    // the end reason from the socket state (peer disconnected / error), if none is set yet
    void        set_end_reason_from_sock();
    static const char *end_reason_of_sock_state(int st);
    // The socket reached the connected state or carried data. A relay that
    // never did, or that ended on an error or because the peer disconnected
    // (closed or went silent: libsrt cannot tell), tells the manager its
    // upstream address may have changed (CSLSRelayManager::relay_ended).
    bool        end_was_failure() const;
    bool        hook_pending() const { return m_connect_sent && !m_http_passed; }
    // the role has an on_connect hook that has not admitted it (yet)
    bool        awaits_hook() const { return !m_http_passed; }
    // one JSON object for the stats document; empty for roles that are not reported (listeners)
    std::string get_stat_info();
    virtual int get_peer_info(char *peer_name, int &peer_port);

    void        set_http_url(const char *http_url, const struct sockaddr_storage *addr, socklen_t addr_len, int timeout_ms);
    int         on_connect();
    int         on_close();
    std::string build_event_url(const char *event);
    // the owning worker polls this every tick while hook_pending()
    int         check_http_passed();

    // Admission (S2): a role becomes visible to others (a publisher's map
    // data, publisher entry and pushes; a player's reader and pull) only in
    // admit(), called once the role is admitted: by the listener at accept
    // when there is no on_event_url hook, else by the owning worker when
    // the hook answers 200. Until then nothing was registered, so a refused
    // or timed-out role has nothing to undo. SLS_ERROR: refused (logged);
    // the caller closes the role.
    void        set_admit_ctx(const sls_admit_ctx_t &ctx, const std::string &app_uplive,
                              const std::string &stream_name);
    virtual int admit() { return SLS_OK; }
    // The pending fire-and-forget on_close request, ownership passes to the caller
    // (the worker group, which keeps polling it after the role is gone); NULL if none.
    CHttpClient *take_close_client();

    // Hand the role the admission slot the listener took for its socket at
    // accept; the role gives it back exactly once, in uninit (also run by the
    // destructor). Only listener-accepted roles carry a slot.
    void        set_conn_limiter(CSLSConnLimiter *limiter, const char *ip, int kind, const std::string &stream);
    // the hook admitted this publisher: its PENDING (shared) slot becomes a
    // PUBLISHER slot that may use the reserve (SLSConnLimiter.hpp)
    void        promote_conn_slot();
    bool        holds_conn_slot() const { return NULL != m_conn_limiter; }
    // ms a queued (accepted, not yet polled) role may wait; 0 = never expires
    void        set_admission_timeout(int ms) { m_admission_timeout = ms; }
    int         get_admission_timeout() const { return m_admission_timeout; }

    // The user_file credentials this role was admitted with (S4): user,
    // direction and the passphrase generation its handshake was checked
    // against. Without one (no user_file, relays) a role is never revoked.
    void        set_auth_identity(const std::shared_ptr<CSLSUserDb> &db, const std::string &user,
                                  bool publish, uint64_t gen, uint64_t epoch);
    // Called only by the worker that owns the role (or is adopting it from
    // the queue): after a users reload, true if the user is gone or that
    // direction's passphrase changed. Logs the one INFO line and sets the
    // end reason; the caller closes the role. Cheap when nothing reloaded.
    bool        auth_revoked();

protected:
    CSLSSrt     *m_srt;
    bool         m_is_write;//listener: 0, publisher: 0, player: 1
    int64_t      m_invalid_begin_tm;//
    int64_t      m_stat_bitrate_last_tm;//
    int          m_stat_bitrate_interval ;//ms
    int          m_stat_bitrate_datacount ;
    int          m_kbitrate;//kb
    int          m_idle_streams_timeout;//unit: s, -1: unlimited
    int          m_latency;//ms

    int          m_state;
    int          m_back_log;//maximum number of connections at the same time
    int          m_port;
    char         m_peer_ip[IP_MAX_LEN];
    int          m_peer_port;
    char         m_role_name[STR_MAX_LEN];
    char         m_streamid[URL_MAX_LEN];
    char         m_http_url[URL_MAX_LEN];
    struct sockaddr_storage m_http_addr;   // resolved by the listener
    socklen_t    m_http_addr_len;
    int          m_http_timeout;           // ms
    bool         m_http_passed;
    bool         m_connect_sent;   // on_connect was sent: on_close is owed, whatever the hook answered (Ruling R18)
    bool         m_close_sent;
    bool         m_ever_connected;    // connected state seen, or data carried
    bool         m_session_started;   // the start line was logged: the end line is owed
    int64_t      m_session_start_ms;
    const char * m_end_reason;        // first reason set wins; a string literal

    sls_conf_base_t    *m_conf;
    CSLSMapData        *m_map_data;
    char                m_map_data_key[URL_MAX_LEN];
    SLSRecycleArrayID   m_map_data_id;

    char          m_data[DATA_BUFF_SIZE];
    int           m_data_len;
    int           m_data_pos;
    // publishing roles: partial TS packet left over from the last SRT read,
    // so only whole 188-byte packets reach the ring (dropped on disconnect)
    uint8_t       m_ts_carry[TS_PACK_LEN];
    int           m_ts_carry_len;
    bool          m_need_reconnect;
    std::string   m_stat_stream;       // "<domain>/<app>/<stream>"; empty = not reported
    std::string   m_stat_remote_ip;
    int           m_stat_remote_port;
    int64_t       m_stat_start_ms;
    CHttpClient  *m_http_client;     // the on_connect request, while pending
    CHttpClient  *m_close_client;    // the on_close request, until the group takes it
    sls_admit_ctx_t m_admit;         // set_admit_ctx
    std::string   m_admit_app;       // "<domain>/<app>" of the publisher side
    std::string   m_admit_stream;    // the stream name

    int  handler_write_data();
    int  handler_read_data(int64_t *last_read_time=NULL);
private:
    CSLSConnLimiter *m_conn_limiter;          // non-NULL while holding a slot
    char             m_conn_ip[IP_MAX_LEN];
    int              m_conn_kind;             // the slot's SLS_SLOT_KIND and, for a player,
    std::string      m_conn_stream;           // its stream key (CSLSConnLimiter::release)
    int              m_admission_timeout;     // ms
    std::shared_ptr<CSLSUserDb> m_auth_db;    // NULL = not admitted through user_file
    std::string      m_auth_user;
    bool             m_auth_publish;
    uint64_t         m_auth_gen;
    uint64_t         m_auth_epoch;            // the db epoch last checked

    void release_conn_slot();

};


#endif
