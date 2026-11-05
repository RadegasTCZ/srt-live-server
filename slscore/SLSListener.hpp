
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
#include <atomic>
#include <memory>
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
#include "SLSUserDb.hpp"
#include "SLSSockTable.hpp"

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
int              max_players_per_stream;//0: unlimited
bool             srtla;//on: the front door owns the public port, libsrt listens on loopback
int              srt_lossmaxttl;//SRTO_LOSSMAXTTL on the listener; 0: off
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
SLS_SET_CONF(server, int,    max_players_per_stream, "max concurrent players of one stream (default 0 = unlimited)", 0, 100000),
SLS_SET_CONF(server, bool,   srtla,                "on: sls owns the public port (front door) and relays SRT to a loopback listener (default off)", 0, 1),
SLS_SET_CONF(server, int,    srt_lossmaxttl,       "SRTO_LOSSMAXTTL on the listener: packets of reordering tolerated before a loss is reported (default 0 = off)", 0, 1000),
SLS_CONF_CMD_DYNAMIC_DECLARE_END

#define SLS_DEFAULT_ADMISSION_TIMEOUT 5000
#define SLS_DEFAULT_ON_EVENT_TIMEOUT  2000
// connections accepted at most by the listener's one-off startup drain (on_added_to_epoll)
#define SLS_LISTENER_DRAIN_MAX 1024
#define SLS_DEFAULT_IDLE_STREAMS_TIMEOUT 10   // seconds; 0 would drop a role the moment it is idle
#define SLS_DEFAULT_BACKLOG 10                // pending handshakes; 0 makes srt_listen fail (err 5003)
inline void sls_conf_server_defaults(sls_conf_server_t *c)
{
    c->idle_streams_timeout = SLS_DEFAULT_IDLE_STREAMS_TIMEOUT;
    c->backlog = SLS_DEFAULT_BACKLOG;
    c->admission_timeout = SLS_DEFAULT_ADMISSION_TIMEOUT;
    c->on_event_timeout  = SLS_DEFAULT_ON_EVENT_TIMEOUT;
    c->srtla = false;
    c->srt_lossmaxttl = 0;
}

/**
 * With the front door: the real remote the listen callback checked a
 * handshake as, kept by socket id until its accept, which admits it only if
 * the front door still maps the connection's internal port to that same
 * remote (a port freed and reused in between must not hand the session,
 * its slot and its logs to another address).
 */
struct sls_fd_identity_t {
    uint32_t ip_be;
    uint16_t port;          // the remote's, host order
    uint16_t local_port;    // the internal socket's, host order
    sls_fd_identity_t() : ip_be(0), port(0), local_port(0) {}
};

/**
 * What the listen callback looked up for a connection it let through, kept
 * until the accept that dequeues the same socket (S4): the credentials the
 * session is admitted with are the ones its handshake was checked against,
 * even if a reload lands between the two.
 */
struct sls_inflight_t {
    std::string user;
    bool        publish;
    uint64_t    gen;       // CSLSUserDb generation of that direction's passphrase
    uint64_t    epoch;     // CSLSUserDb epoch of the lookup
    bool        verified;  // libsrt enforced a non-empty passphrase on this handshake (R40/R41)
    sls_inflight_t() : publish(false), gen(0), epoch(0), verified(false) {}
};
// handshakes between callback and accept; past it the oldest record is
// dropped, and one older than the TTL too (CSLSSockTable, R43)
#define SLS_INFLIGHT_MAX     4096
#define SLS_INFLIGHT_TTL_MS  5000


/**
 * SLSListener
 */
class CSLSFrontDoor;

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
    virtual bool is_listener() const { return true; }
    // A worker dropped this listener (its socket broke or the epoll add
    // failed): it no longer accepts, and the manager stops sls. It stays
    // allocated until its group's clear(), so the manager's pointer stays valid.
    void         set_lost() { m_lost = true; }
    // also true when this block's front door thread failed (it no longer relays)
    bool         is_lost() const;
    // SIGHUP: re-resolve the on_event_url host (failure keeps the previous address)
    void reload_hook();

    void set_role_list(CSLSRoleList *list_role);
    void set_map_publisher(CSLSMapPublisher * publisher);
    void set_map_puller(CSLSMapRelay *map_puller);
    void set_map_pusher(CSLSMapRelay *map_puller);
    // shared admission limiter (owned by CSLSManager); NULL = no caps
    void set_admission_limiter(CSLSConnLimiter *limiter);

    // srtla on: libsrt listens on 127.0.0.1:internal_port() and the front door
    // owns the public port m_port. Off: internal_port() is 0.
    bool front_door_enabled() const { return m_front_door_enabled; }
    int  internal_port() const { return m_internal_port; }
    // srtla on: this block's front door (owned here; the manager starts its
    // thread and stops it before any listener is torn down). NULL when off.
    CSLSFrontDoor *front_door() const { return m_front_door; }
    // The peer as SLS logs and limits it: the address libsrt reports, or with
    // the front door the real remote behind its internal socket. false: the
    // front door has no such mapping (a local process connected to the
    // internal port directly); ip/port are then the address libsrt reported.
    bool real_peer(const struct sockaddr *sa, char *ip, size_t ip_len, int *port,
                   struct sockaddr_in *remote_out = NULL) const;
    // the front door's mapping caps (from the srt block), set before start()
    void set_front_door_caps(int max_total, int max_per_ip) { m_fd_max_total = max_total; m_fd_max_per_ip = max_per_ip; }


private:
    CSLSRoleList      * m_list_role;
    std::atomic<bool>   m_lost;
    CSLSMapPublisher  * m_map_publisher;
    CSLSMapRelay      * m_map_puller;
    CSLSMapRelay      * m_map_pusher;

    CSLSMutex           m_mutex;

    int                 m_idle_streams_timeout_role;
    int                 m_admission_timeout;   // ms, from the server block
    CSLSConnLimiter   * m_admission_limiter;
    bool                m_front_door_enabled;
    int                 m_internal_port;   // libsrt's loopback port in srtla mode, else 0
    CSLSFrontDoor     * m_front_door;
    int                 m_fd_max_total;
    int                 m_fd_max_per_ip;
    int                 m_max_players_per_stream;   // 0 = unlimited
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

    // user_file's users; shared with every role admitted through them (S4)
    std::shared_ptr<CSLSUserDb>            m_userdb;
    // listen callback -> accept, keyed by the new socket (see sls_inflight_t)
    CSLSSockTable<sls_inflight_t>          m_inflight;
    // listen callback -> accept, srtla on only (see sls_fd_identity_t)
    CSLSSockTable<sls_fd_identity_t>       m_fd_identity;
    // the real remote of an accepted front-door connection, as the callback
    // checked it; false: refuse it
    bool pinned_peer(int fd, const char *libsrt_ip, int libsrt_port, char *ip, size_t ip_len, int *port);
    void pin_identity(int ns, const struct sockaddr_in &remote, const struct sockaddr *peeraddr);

    int  init_conf_app();
    // One accept and its admission: 1 = a socket came off the queue (admitted
    // or refused), 0 = the accept failed (more may be queued), -1 = nothing queued.
    int  accept_one();
    // one INFO line per second at most for accept-time refusals
    void log_admission_refusal(const char *where, const char *peer_name, int peer_port, int why, const char *stream);
    // the limiter's per-stream key of a player of key_stream_name
    std::string player_stream_key(const char *key_stream_name) const;
    // the SLS_SLOT_KIND an accepted publisher takes (see the .cpp); with
    // user_file, verified is what the callback enforced on its handshake
    int  publisher_slot_kind(const std::string &user, const bool *verified = NULL);
    // Role of a parsed streamid by host/app (never by app name alone); on
    // PUBLISHER/PLAYER *app_key_out is the publisher-side "host/app" key.
    sls_sid_role_t classify_sid(const sls_sid_t &sid, std::string *app_key_out);
    // Parse the whole file into a staging map; SLS_OK only if every line is
    // valid and there is at least one user, then swap it into m_userdb. On any
    // error the users are left untouched and *err (if given) names file:line.
    int  load_users(const char *path, std::string *err);

    static int listen_callback(void *opaq, SRTSOCKET ns, int hsversion,
                               const struct sockaddr *peeraddr, const char *streamid);

public:
    // libsrt refused socket ns for a wrong passphrase (on its receive
    // thread): its handshake record, in whichever listener holds it, is
    // dropped, as it will never be accepted.
    static void forget_handshake(int ns);

    // Re-read the user_file configured for this listener and swap it into
    // m_userdb under its write lock. Safe to call from the main thread while
    // listen_callback is firing on SRT's internal thread. Returns SLS_ERROR
    // (and logs ERROR "reload failed") if the file is rejected; the previous
    // users stay in force. On success every admitted role re-checks its
    // credentials on its worker's next pass (CSLSRole::auth_revoked).
    int reload_users();
};


#endif
