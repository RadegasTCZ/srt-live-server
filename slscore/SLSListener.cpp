
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
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <fstream>
#include <sstream>
#include <set>
#include <vector>
#include <atomic>

#include "SLSListener.hpp"
#include "SLSLog.hpp"
#include "SLSPublisher.hpp"
#include "SLSPlayer.hpp"
#include "SLSPullerManager.hpp"
#include "SLSPusherManager.hpp"

/**
 * server conf
 */
SLS_CONF_DYNAMIC_IMPLEMENT_DEFAULTS(server, sls_conf_server_defaults)

/**
 * CSLSListener class implementation
 */

CSLSListener::CSLSListener()
{
    m_conf      = NULL;
    m_back_log  = 1024;
    m_is_write  = 0;
    m_port      = 0;
    m_lost      = false;

    m_list_role     = NULL;
    m_map_publisher = NULL;
    m_map_puller    = NULL;
    m_idle_streams_timeout      = UNLIMITED_TIMEOUT;
    m_idle_streams_timeout_role = 0;
    m_admission_timeout = SLS_DEFAULT_ADMISSION_TIMEOUT;
    m_admission_limiter = NULL;
    memset(m_http_url_role, 0, URL_MAX_LEN);
    m_on_event_timeout = SLS_DEFAULT_ON_EVENT_TIMEOUT;
    memset(&m_hook_addr, 0, sizeof(m_hook_addr));
    m_hook_addr_len = 0;

    sprintf(m_role_name, "listener");
}

CSLSListener::~CSLSListener()
{
}

int CSLSListener::init()
{
	int ret = 0;
    return CSLSRole::init();
}

int CSLSListener::uninit()
{
	CSLSLock lock(&m_mutex);
    stop();
    return CSLSRole::uninit();
}

void CSLSListener::set_role_list(CSLSRoleList *list_role)
{
	m_list_role = list_role;
}

void CSLSListener::set_map_publisher(CSLSMapPublisher * publisher)
{
	m_map_publisher = publisher;
}

void CSLSListener::set_map_puller(CSLSMapRelay *map_puller)
{
    m_map_puller     = map_puller;
}

void CSLSListener::set_map_pusher(CSLSMapRelay *map_pusher)
{
    m_map_pusher     = map_pusher;
}

void CSLSListener::set_admission_limiter(CSLSConnLimiter *limiter)
{
    m_admission_limiter = limiter;
}

// Rate-limited with every other peer-triggered line (sls_peer_log): a flood
// from many listeners still logs one line per second.
void CSLSListener::log_admission_refusal(const char *peer_name, int peer_port, int why)
{
    if (SLS_CONN_PER_IP == why) {
        sls_peer_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, refused [%s:%d]: per-IP connection limit (%d) reached.",
                     this, peer_name, peer_port, m_admission_limiter->max_per_ip());
    } else {
        sls_peer_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, refused [%s:%d]: total connection limit (%d) reached.",
                     this, peer_name, peer_port, m_admission_limiter->max_total());
    }
}

namespace {
// add_relay_manager's reference, dropped when handler() returns
struct sls_relay_manager_ref {
    CSLSRelayManager *mgr;
    explicit sls_relay_manager_ref(CSLSRelayManager *m) : mgr(m) {}
    ~sls_relay_manager_ref() { if (mgr) mgr->release(); }
};

// The admission slot taken at accept: given back when handler() returns,
// unless it was handed to the role that now owns the socket.
struct sls_slot_guard {
    CSLSConnLimiter *limiter;
    const char      *ip;
    sls_slot_guard(CSLSConnLimiter *l, const char *i) : limiter(l), ip(i) {}
    ~sls_slot_guard() { if (limiter) limiter->release(ip); }
    void hand_to(CSLSRole *role) {
        if (limiter) role->set_conn_limiter(limiter, ip);
        limiter = NULL;
    }
};
}

int CSLSListener::init_conf_app()
{
    string strLive;
    string strUplive;
    string strLiveDomain;
    string strUpliveDomain;
    string strTemp;
    vector<string> domain_players;
    sls_conf_server_t * conf_server;

    if (NULL == m_map_puller) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app failed, m_map_puller is null.", this);
        return SLS_ERROR;
    }

    if (NULL == m_map_pusher) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app failed, m_map_pusher is null.", this);
        return SLS_ERROR;
    }

    if (!m_conf) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app failed, conf is null.", this);
        return SLS_ERROR;
    }
    conf_server = (sls_conf_server_t *)m_conf;

    m_back_log                   = conf_server->backlog;
    m_idle_streams_timeout_role  = conf_server->idle_streams_timeout;
    m_admission_timeout          = conf_server->admission_timeout;
    snprintf(m_http_url_role, sizeof(m_http_url_role), "%s", conf_server->on_event_url);
    m_on_event_timeout           = conf_server->on_event_timeout;
    if (m_http_url_role[0] != 0 && SLS_OK != resolve_hook()) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app, on_event_url='%s' is unusable (not an http:// URL or the host does not resolve).",
                this, sls_url_for_log(m_http_url_role).c_str());
        return SLS_ERROR;
    }
    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::init_conf_app, m_back_log=%d, m_idle_streams_timeout=%d, admission_timeout=%dms.",
            this, m_back_log, m_idle_streams_timeout_role, m_admission_timeout);

    //domain
    domain_players = sls_conf_string_split(string(conf_server->domain_player), string(" "));
    if (domain_players.size() == 0) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app, wrong domain_player='%s'.", this, conf_server->domain_player);
        return SLS_ERROR;
    }
    strUpliveDomain = conf_server->domain_publisher;
    if (strUpliveDomain.length() == 0) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app, wrong domain_publisher='%s'.", this, conf_server->domain_publisher);
        return SLS_ERROR;
    }
    sls_conf_app_t * conf_app = (sls_conf_app_t *)conf_server->child;
    if (!conf_app) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app, no app conf info.", this);
        return SLS_ERROR;
    }

    std::set<std::string> publisher_keys;   // host/app a publisher connects to
    std::set<std::string> player_keys;      // host/app a player connects to
    int app_count = sls_conf_get_conf_count(conf_app);
    sls_conf_app_t * ca = conf_app;
    for (int i = 0; i < app_count; i ++) {
        strUplive = ca->app_publisher;
        if (strUplive.length() == 0) {
            sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app, wrong app_publisher='%s', domain_publisher='%s'.",
                    this, strUplive.c_str(), strUpliveDomain.c_str());
            return SLS_ERROR;
        }
        strUplive = strUpliveDomain + "/" + strUplive;
        publisher_keys.insert(strUplive);
        m_map_publisher->set_conf(strUplive, ca);
        sls_log(SLS_LOG_INFO, "[%p]CSLSListener::init_conf_app, add app push '%s'.",
                this, strUplive.c_str());

        strLive = ca->app_player;
        if (strLive.length() == 0) {
            sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app, wrong app_player='%s', domain_publisher='%s'.",
                    this, strLive.c_str(), strUpliveDomain.c_str());
            return SLS_ERROR;
        }

        for (int j = 0; j < domain_players.size(); j ++) {
            strLiveDomain = domain_players[j];
            strTemp =strLiveDomain + "/" + strLive;
            if (strUplive == strTemp) {
                sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app faild, domain/uplive='%s' and domain/live='%s' must not be equal.",
                        this, strUplive.c_str(), strTemp.c_str());
                return SLS_ERROR;
            }
            player_keys.insert(strTemp);
            //m_map_live_2_uplive[strTemp]  = strUplive;
            m_map_publisher->set_live_2_uplive(strTemp, strUplive);

            sls_log(SLS_LOG_INFO, "[%p]CSLSListener::init_conf_app, add app live='%s', app push='%s'.",
                    this, strTemp.c_str(), strUplive.c_str());
        }

        if (NULL != ca->child) {
        	sls_conf_relay_t *cr = (sls_conf_relay_t *)ca->child;
            while (cr) {
				std::string relay_err;
				if (SLS_OK != sls_relay_conf_check(cr, &relay_err)) {
					sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app, %s, app push='%s'.",
							this, relay_err.c_str(), strUplive.c_str());
					return SLS_ERROR;
				}
				if (strcmp(cr->type, "pull") == 0 ) {
					if (SLS_OK != m_map_puller->add_relay_conf(strUplive.c_str(), cr)) {
						sls_log(SLS_LOG_WARNING, "[%p]CSLSListener::init_conf_app, m_map_puller.add_app_conf faile. relay type='%s', app push='%s'.",
								this, cr->type, strUplive.c_str());
					}
				}
				else if (strcmp(cr->type, "push") == 0) {
					if (SLS_OK != m_map_pusher->add_relay_conf(strUplive.c_str(), cr)) {
						sls_log(SLS_LOG_WARNING, "[%p]CSLSListener::init_conf_app, m_map_pusher.add_app_conf faile. relay type='%s', app push='%s'.",
								this, cr->type, strUplive.c_str());
					}
				} else {
					sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app, wrong relay type='%s', app push='%s'.",
							this, cr->type, strUplive.c_str());
					return SLS_ERROR;
				}
				cr = (sls_conf_relay_t *)cr->sibling;
            }
        }

        ca = (sls_conf_app_t *)ca->sibling;
    }

    // A host/app key must have exactly one role, otherwise the streamid
    // classifier (and therefore the passphrase choice) would be ambiguous.
    for (std::set<std::string>::const_iterator it = publisher_keys.begin();
         it != publisher_keys.end(); ++it) {
        if (player_keys.count(*it)) {
            sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::init_conf_app failed, '%s' is configured as both a publisher and a player app.",
                    this, it->c_str());
            return SLS_ERROR;
        }
    }
    return SLS_OK;

}

sls_sid_role_t CSLSListener::classify_sid(const sls_sid_t &sid, std::string *app_key_out)
{
    // The role is decided by host/app, the same lookups handler() always used,
    // never by the app name alone.
    std::string key = sid.host + "/" + sid.app;

    std::string uplive = m_map_publisher->get_uplive(key);
    if (uplive.length() > 0) {
        if (app_key_out) *app_key_out = uplive;
        return SLS_SID_PLAYER;
    }
    if (NULL != m_map_publisher->get_ca(key)) {
        if (app_key_out) *app_key_out = key;
        return SLS_SID_PUBLISHER;
    }
    return SLS_SID_REJECT;
}

int CSLSListener::load_users(const char *path, std::string *err)
{
    // Every failure below leaves m_users untouched: a bad file must never
    // weaken (or silently disable) authentication.
    // None of the messages below may echo a value from the file: with the
    // columns swapped or a stray character, a passphrase can land in the name
    // column. They carry the line number and the column name only.
    char msg[512];
    // open once, check and read the same descriptor (no swap between the two)
    int rfd = ::open(path, O_RDONLY|O_CLOEXEC);
    if (rfd < 0) {
        snprintf(msg, sizeof(msg), "cannot open '%s': %s", path, strerror(errno));
        if (err) *err = msg;
        return SLS_ERROR;
    }
    struct stat st;
    if (fstat(rfd, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(msg, sizeof(msg), "'%s' is not a regular file", path);
        if (err) *err = msg;
        ::close(rfd);
        return SLS_ERROR;
    }
    if (st.st_mode & 077) {
        snprintf(msg, sizeof(msg), "'%s' is accessible to group or others (mode %03o): it holds passphrases, "
                 "refusing to use it; run chmod 600 on it", path, (unsigned)(st.st_mode & 0777));
        if (err) *err = msg;
        ::close(rfd);
        return SLS_ERROR;
    }
    std::string content;
    {
        char buf[4096];
        ssize_t n;
        while ((n = ::read(rfd, buf, sizeof(buf))) > 0 && content.size() < (1u << 20)) {
            content.append(buf, (size_t)n);
        }
    }
    ::close(rfd);
    std::istringstream f(content);

    // Parse into a staging map, then swap under the write lock so
    // listen_callback never observes a half-loaded m_users.
    std::map<std::string, sls_user_t> staging;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        std::string::size_type hash = line.find('#');
        if (hash != std::string::npos) {
            // A comment starts at a '#' that begins the line or follows white
            // space. One inside a column would silently cut a passphrase short.
            if (hash > 0 && line[hash - 1] != ' ' && line[hash - 1] != '\t') {
                snprintf(msg, sizeof(msg), "%s:%d: '#' inside a column (it starts a comment and would cut a "
                         "passphrase short; a passphrase cannot contain '#')", path, lineno);
                if (err) *err = msg;
                return SLS_ERROR;
            }
            line = line.substr(0, hash);
        }

        std::istringstream iss(line);
        std::string name, pub, play, extra;
        if (!(iss >> name)) continue;              // blank / whitespace-only line
        if (!(iss >> pub) || !(iss >> play) || (iss >> extra)) {
            snprintf(msg, sizeof(msg), "%s:%d: wrong column count "
                     "(expected exactly 'name publish_pass play_pass')", path, lineno);
            if (err) *err = msg;
            return SLS_ERROR;
        }
        if (!sls_sid_part_ok(name)) {
            snprintf(msg, sizeof(msg), "%s:%d: invalid user name in column 1 "
                     "(allowed: 1-128 of A-Z a-z 0-9 . _ -, not '.' or '..'; are the columns in the order "
                     "name publish_pass play_pass?)", path, lineno);
            if (err) *err = msg;
            return SLS_ERROR;
        }
        if (pub  == "-") pub.clear();
        if (play == "-") play.clear();

        if (!sls_is_valid_passphrase_len(pub.length())) {
            snprintf(msg, sizeof(msg), "%s:%d: column 2 (publish_pass) length %zu out of range "
                     "(SRT requires 10-79 bytes, or '-' for plaintext)",
                     path, lineno, pub.length());
            if (err) *err = msg;
            return SLS_ERROR;
        }
        if (!sls_is_valid_passphrase_len(play.length())) {
            snprintf(msg, sizeof(msg), "%s:%d: column 3 (play_pass) length %zu out of range "
                     "(SRT requires 10-79 bytes, or '-' for plaintext)",
                     path, lineno, play.length());
            if (err) *err = msg;
            return SLS_ERROR;
        }
        if (staging.count(name)) {
            snprintf(msg, sizeof(msg), "%s:%d: duplicate user name in column 1", path, lineno);
            if (err) *err = msg;
            return SLS_ERROR;
        }

        sls_user_t u;
        u.name         = name;
        u.publish_pass = pub;
        u.play_pass    = play;
        staging[name]  = u;
    }

    if (staging.empty()) {
        snprintf(msg, sizeof(msg), "no valid users in '%s' (an empty user_file would "
                 "leave nobody able to connect)", path);
        if (err) *err = msg;
        return SLS_ERROR;
    }

    size_t loaded = staging.size();
    {
        CSLSLock lock(&m_users_lock, 1); // write lock
        m_users.swap(staging);
    }

    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::load_users, loaded %zu user(s) from '%s'.",
            this, loaded, path);
    return SLS_OK;
}

int CSLSListener::resolve_hook()
{
    std::string host;
    std::string uri;
    int         port = 0;
    if (SLS_OK != sls_parse_http_url(m_http_url_role, &host, &port, &uri)) {
        return SLS_ERROR;
    }
    struct sockaddr_storage addr;
    socklen_t               len = 0;
    if (SLS_OK != sls_resolve(host.c_str(), port, &addr, &len)) {
        return SLS_ERROR;
    }
    CSLSLock lock(&m_hook_mutex);
    m_hook_addr     = addr;
    m_hook_addr_len = len;
    return SLS_OK;
}

void CSLSListener::reload_hook()
{
    if (m_http_url_role[0] == 0) {
        return;
    }
    if (SLS_OK != resolve_hook()) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::reload_hook, cannot resolve the on_event_url host, keeping the previous address.", this);
    } else {
        sls_log(SLS_LOG_INFO, "[%p]CSLSListener::reload_hook, on_event_url host re-resolved.", this);
    }
}

// Hand a new role the hook's URL, address and timeout. The address is copied
// under the mutex because a reload may replace it from another thread.
void CSLSListener::attach_hook(CSLSRole *role)
{
    if (m_http_url_role[0] == 0) {
        return;
    }
    struct sockaddr_storage addr;
    socklen_t               len;
    {
        CSLSLock lock(&m_hook_mutex);
        addr = m_hook_addr;
        len  = m_hook_addr_len;
    }
    role->set_http_url(m_http_url_role, &addr, len, m_on_event_timeout);
}

int CSLSListener::reload_users()
{
    reload_hook();
    if (!m_conf) {
        return SLS_OK;
    }
    const char *path = ((sls_conf_server_t*)m_conf)->user_file;
    if (!path || !*path) {
        return SLS_OK; // user_file not configured, nothing to reload
    }
    std::string err;
    if (SLS_OK != load_users(path, &err)) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::reload_users, reload failed, keeping the previous users: %s.",
                this, err.c_str());
        return SLS_ERROR;
    }
    size_t n;
    {
        CSLSLock lock(&m_users_lock, 0); // read lock
        n = m_users.size();
    }
    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::reload_users, reload ok (%zu user(s)).", this, n);
    return SLS_OK;
}

int CSLSListener::listen_callback(void *opaq, SRTSOCKET ns, int hsversion,
                                  const struct sockaddr *peeraddr, const char *streamid)
{
    CSLSListener *self = static_cast<CSLSListener*>(opaq);

    if (!streamid || !*streamid) {
        sls_peer_log(SLS_LOG_ERROR, "[%p]CSLSListener::listen_callback, empty streamid; rejecting.", self);
        return -1;
    }

    // Expect streamid of the form "host/app/stream" with strict charset.
    sls_sid_t sid;
    if (!sls_sid_parse(streamid, &sid)) {
        sls_peer_log(SLS_LOG_ERROR, "[%p]CSLSListener::listen_callback, refused: streamid must be host/app/stream, sid='%s'.",
                self, sls_escape_for_log(streamid).c_str());
        return -1;
    }
    const std::string &stream = sid.stream;

    std::string app_key;
    sls_sid_role_t role = self->classify_sid(sid, &app_key);
    if (SLS_SID_REJECT == role) {
        sls_peer_log(SLS_LOG_INFO, "[%p]CSLSListener::listen_callback, refused: '%s/%s' is neither a publisher nor a player app (user '%s').",
                self, sid.host.c_str(), sid.app.c_str(), stream.c_str());
        return -1;
    }
    const char *direction = (SLS_SID_PUBLISHER == role) ? "publish" : "play";

    // Look up credentials under a read lock and copy what we need out of
    // the map so we can drop the lock before calling into libsrt (which may
    // reorder across threads).
    std::string pass_value;
    bool        pass_empty  = false;
    {
        CSLSLock lock(&self->m_users_lock, 0); // read lock
        std::map<std::string, sls_user_t>::iterator it = self->m_users.find(stream);
        if (it == self->m_users.end()) {
            sls_peer_log(SLS_LOG_INFO, "[%p]CSLSListener::listen_callback, unknown user '%s'; rejecting.",
                    self, stream.c_str());
            return -1;
        }
        pass_value = (SLS_SID_PUBLISHER == role) ? it->second.publish_pass : it->second.play_pass;
        pass_empty = pass_value.empty();
    }

    if (!pass_empty) {
        // Key length first (it must be in place before the passphrase derives the
        // KEK), then the passphrase, then refuse any peer that does not encrypt.
        int pbkeylen = ((sls_conf_server_t*)self->m_conf)->srt_pbkeylen;
        if (pbkeylen > 0 &&
            srt_setsockflag(ns, SRTO_PBKEYLEN, &pbkeylen, sizeof(pbkeylen)) < 0) {
            sls_peer_log(SLS_LOG_ERROR,
                    "[%p]CSLSListener::listen_callback, SRTO_PBKEYLEN=%d for user '%s' (%s) failed: %s.",
                    self, pbkeylen, stream.c_str(), direction, srt_getlasterror_str());
            return -1;
        }
        if (srt_setsockflag(ns, SRTO_PASSPHRASE, pass_value.c_str(), (int)pass_value.length()) < 0) {
            sls_peer_log(SLS_LOG_ERROR,
                    "[%p]CSLSListener::listen_callback, SRTO_PASSPHRASE for user '%s' (%s) failed: %s.",
                    self, stream.c_str(), direction, srt_getlasterror_str());
            return -1;
        }
        int enforce = 1;
        if (srt_setsockflag(ns, SRTO_ENFORCEDENCRYPTION, &enforce, sizeof(enforce)) < 0) {
            sls_peer_log(SLS_LOG_ERROR,
                    "[%p]CSLSListener::listen_callback, SRTO_ENFORCEDENCRYPTION for user '%s' (%s) failed: %s.",
                    self, stream.c_str(), direction, srt_getlasterror_str());
            return -1;
        }
        // per attempt, so DEBUG: the admitted session logs its start at INFO
        sls_log(SLS_LOG_DEBUG, "[%p]CSLSListener::listen_callback, accept user '%s' (%s, encrypted).",
                self, stream.c_str(), direction);
    } else {
        sls_log(SLS_LOG_DEBUG, "[%p]CSLSListener::listen_callback, accept user '%s' (%s, plaintext).",
                self, stream.c_str(), direction);
    }
    return 0;
}

int CSLSListener::start()
{
	int ret = 0;
    std::string strLive;
    std::string strUplive;
    std::string strLiveDomain;
    std::string strUpliveDomain;


	if (NULL == m_conf) {
	    sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::start failed, conf is null.", this);
	    return SLS_ERROR;
	}
    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::start...", this);

    ret = init_conf_app();
    if (SLS_OK != ret) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::start, init_conf_app failed.", this);
        return SLS_ERROR;
    }

    //init listener
    if (NULL == m_srt)
        m_srt = new CSLSSrt();

    int latency = ((sls_conf_server_t*)m_conf)->latency;
    if (latency > 0) {
        m_srt->libsrt_set_latency(latency);
    }

    const char *user_file  = ((sls_conf_server_t*)m_conf)->user_file;
    const char *passphrase = ((sls_conf_server_t*)m_conf)->srt_passphrase;
    int         pbkeylen   = ((sls_conf_server_t*)m_conf)->srt_pbkeylen;
    if (!sls_is_valid_pbkeylen(pbkeylen)) {
        sls_log(SLS_LOG_ERROR,
                "[%p]CSLSListener::start, srt_pbkeylen=%d is invalid (allowed: 0, 16, 24, 32).",
                this, pbkeylen);
        return SLS_ERROR;
    }
    if (passphrase && *passphrase && !sls_is_valid_passphrase_len(strlen(passphrase))) {
        sls_log(SLS_LOG_ERROR,
                "[%p]CSLSListener::start, srt_passphrase length %zu is out of range "
                "(SRT requires 10-79 bytes).",
                this, strlen(passphrase));
        return SLS_ERROR;
    }
    if (user_file && *user_file) {
        std::string err;
        if (SLS_OK != load_users(user_file, &err)) {
            sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::start, user_file unusable, refusing to start without authentication: %s.",
                    this, err.c_str());
            return SLS_ERROR;
        }
        if (passphrase && *passphrase) {
            sls_log(SLS_LOG_WARNING,
                    "[%p]CSLSListener::start, user_file='%s' is set; ignoring listener-wide srt_passphrase. "
                    "Per-user passphrases will be applied by the listen callback.",
                    this, user_file);
        }
    } else if (passphrase && *passphrase) {
        m_srt->libsrt_set_passphrase(passphrase, pbkeylen);
    }

    if (!(user_file && *user_file) && !(passphrase && *passphrase) && m_http_url_role[0] == 0) {
        sls_log(SLS_LOG_WARNING,
                "[%p]CSLSListener::start, port %d has NO authentication (no user_file, no srt_passphrase, no on_event_url): "
                "anyone who can reach it may publish and play. Set user_file (recommended), or an on_event_url hook.",
                this, ((sls_conf_server_t*)m_conf)->listen);
    }

    m_port = ((sls_conf_server_t*)m_conf)->listen;
    ret = m_srt->libsrt_setup(m_port);
    if (SLS_OK != ret) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::start, libsrt_setup failure.", this);
        return ret;
    }
    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::start, libsrt_setup ok.", this);

    // The callback is installed whenever user_file is set, regardless of how many
    // users are loaded: with no callback every streamid would be accepted.
    if (user_file && *user_file) {
        if (srt_listen_callback(m_srt->libsrt_get_fd(), listen_callback, this) < 0) {
            sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::start, srt_listen_callback failed: %s.",
                    this, srt_getlasterror_str());
            return SLS_ERROR;
        }
        sls_log(SLS_LOG_INFO, "[%p]CSLSListener::start, per-user auth active (%zu user(s) loaded).",
                this, m_users.size());
    }

    // Non-blocking: no accept on a worker may ever wait. A queued connection
    // can vanish between the epoll report and the accept (the peer aborts),
    // and a blocking accept would then hang the worker for good.
    if (m_srt->libsrt_socket_nonblock(0) < 0) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::start, cannot make the listener non-blocking: %s.",
                this, srt_getlasterror_str());
        return SLS_ERROR;
    }
    // Here, at startup: a listen failure (a bad backlog, a port another
    // server block already listens on) stops sls before it serves anything.
    ret = m_srt->libsrt_listen(m_back_log);
    if (SLS_OK != ret) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::start, libsrt_listen failed on port %d (err=%d, %s).",
                this, m_port, ret, srt_getlasterror_str());
        return SLS_ERROR;
    }

    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::start, m_list_role=%p.", this, m_list_role);
    if (NULL == m_list_role) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSListener::start, m_roleList is null.", this);
        return ret;
    }


    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::start, push to m_list_role=%p.", this, m_list_role);
    m_list_role->push(this);

	return ret;
}

int CSLSListener::stop()
{
	int ret = SLS_OK;
    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::stop.", this);

 	return ret;
}


// libsrt raises the listener's readable event only for a connection that
// completes while the socket is subscribed to an epoll; adding an already
// listening socket does not report the connections queued before. The socket
// listens from start() on, but a worker adds it to its epoll only later, so a
// client whose handshake landed in between (a few ms; ASan builds, a busy
// host) would never be accepted. The worker calls this right after its own
// add: non-blocking accepts until none is queued (SRT_EASYNCRCV) drain what
// came before, and the worker's epoll reports every later connection.
// An accept that fails (libsrt took a queued connection off the queue that
// had closed meanwhile) does not end the drain: the connections queued
// behind it raise no event of their own either. Only "nothing queued" does,
// or the cap: each round takes one socket off a queue that is at most
// backlog long, so the loop cannot spin.
int CSLSListener::on_added_to_epoll()
{
    int drained = 0;
    int rounds  = 0;
    while (rounds < SLS_LISTENER_DRAIN_MAX) {
        int r = accept_one();
        if (r < 0) {
            break;            // nothing queued
        }
        rounds++;
        drained += r;
    }
    // The text is the one the startup line always had (scripts wait for it);
    // it now also means a worker polls the listener.
    sls_log(SLS_LOG_INFO, "[%p]CSLSSrt::libsrt_listen, ok, fd=%d, at port=%d, polled by a worker (%d queued connection(s) accepted).",
            this, m_srt->libsrt_get_fd(), m_port, drained);
    return SLS_OK;
}

int CSLSListener::handler()
{
    return accept_one() > 0 ? 1 : 0;
}

int CSLSListener::accept_one()
{
	int ret = SLS_OK;
	int fd_client = 0;
	CSLSSrt *srt = NULL;
	char sid[1024] = {0};
	int  sid_size = sizeof(sid);
	sls_sid_t psid;
	char key_app[URL_MAX_LEN] = {0};
	char key_stream_name[URL_MAX_LEN] = {0};
    char peer_name[IP_MAX_LEN] = {0};
    int  peer_port = 0;
    int  client_count = 0;

    //1: accept
    fd_client = m_srt->libsrt_accept();
    if (SLS_ACCEPT_EMPTY == fd_client) {
        return -1;
    }
    if (fd_client < 0) {
        // dequeued but failed; libsrt_accept has logged it (through sls_peer_log)
        return 0;
    }
    client_count = 1;

    //2.check streamid, split it
	srt = new CSLSSrt;
	srt->libsrt_set_fd(fd_client);
	// An accepted socket inherits the listener's modes, and the listener is
	// non-blocking only for its own accepts: give the connection the
	// blocking mode it always had (players switch to non-blocking below).
	srt->libsrt_socket_nonblock(1);
    ret = srt->libsrt_getpeeraddr(peer_name, peer_port);
    if (ret != 0) {
        // the peer may already be gone again (it closed right after the handshake)
        sls_peer_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, libsrt_getpeeraddr failed, fd=%d.", this, srt->libsrt_get_fd());
        srt->libsrt_close();
        delete srt;
        return client_count;
    }

    // admission: over the total or per-IP cap the socket is closed right here
    int why = SLS_CONN_OK;
    if (NULL != m_admission_limiter && !m_admission_limiter->try_acquire(peer_name, &why)) {
        log_admission_refusal(peer_name, peer_port, why);
        srt->libsrt_close();
        delete srt;
        return client_count;
    }
    sls_slot_guard slot(m_admission_limiter, peer_name);
    // Per-attempt lines are DEBUG, so they never take the peer-line budget
    // from the refusal that explains the attempt; an admitted session logs
    // its start at INFO ("new publisher[...]" / "new player[...]").
    sls_log(SLS_LOG_DEBUG, "[%p]CSLSListener::handler, new client[%s:%d], fd=%d.", this, peer_name, peer_port, fd_client);

    if (0 != srt->libsrt_getsockopt(SRTO_STREAMID, "SRTO_STREAMID", &sid, &sid_size, true)) {
        sls_peer_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, [%s:%d], fd=%d, get streamid info failed.",
                this, peer_name, peer_port, srt->libsrt_get_fd());
    	srt->libsrt_close();
        delete srt;
    	return client_count;
    }

    if (!sls_sid_parse(sid, &psid)) {
        sls_peer_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, [%s:%d], refused: streamid must be host/app/stream, sid='%s'.",
                this, peer_name, peer_port, sls_escape_for_log(sid).c_str());
    	srt->libsrt_close();
    	delete srt;
    	return client_count;
    }
    const char *stream_name = psid.stream.c_str();
    sls_log(SLS_LOG_DEBUG, "[%p]CSLSListener::handler, [%s:%d], sid '%s/%s/%s'",
            this, peer_name, peer_port, psid.host.c_str(), psid.app.c_str(), stream_name);

    // app exist?
    snprintf(key_app, sizeof(key_app), "%s/%s", psid.host.c_str(), psid.app.c_str());

    std::string app_uplive = "";
    sls_conf_app_t * ca = NULL;
    sls_sid_role_t role = classify_sid(psid, &app_uplive);
    if (SLS_SID_REJECT == role) {
        sls_peer_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, refused, new role[%s:%d], '%s' is neither a publisher nor a player app.",
                this, peer_name, peer_port, key_app);
        srt->libsrt_close();
        delete srt;
        return client_count;
    }

    //3.is player?
    if (SLS_SID_PLAYER == role) {
        snprintf(key_stream_name, sizeof(key_stream_name), "%s/%s", app_uplive.c_str(), stream_name);
        // Only test pub against NULL / log its value: a publisher found here
        // can be closed and deleted by its worker (or by CSLSRoleList::expire
        // while still queued) as soon as the map lock is dropped.
        CSLSRole * pub = m_map_publisher->get_publisher(key_stream_name);
        if (NULL == pub) {
        	//*
        	//3.1 check pullers
        	if (NULL == m_map_puller) {
    			sls_peer_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, refused, new role[%s:%d], stream='%s', publisher is NULL and m_map_puller is NULL.",
    						this, peer_name, peer_port, key_stream_name);
    			srt->libsrt_close();
    			delete srt;
    			return client_count;
        	}
        	// with user_file set, only listed stream names may trigger a pull
        	// (the listen callback already refuses others; this is the backstop)
        	const char *user_file = ((sls_conf_server_t*)m_conf)->user_file;
        	if (user_file && *user_file) {
        		bool known;
        		{
        			CSLSLock lock(&m_users_lock, 0); // read lock
        			known = m_users.count(psid.stream) > 0;
        		}
        		if (!known) {
    				sls_peer_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, refused, new role[%s:%d], stream='%s': not in user_file, no pull.",
    							this, peer_name, peer_port, key_stream_name);
    				srt->libsrt_close();
    				delete srt;
    				return client_count;
        		}
        	}
        	bool created = false;
        	CSLSRelayManager *puller_manager = m_map_puller->add_relay_manager(app_uplive.c_str(), stream_name, &created);
        	if (NULL == puller_manager) {
    			sls_peer_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, m_map_puller->add_relay_manager failed, new role[%s:%d], stream='%s', publisher is NULL, no puller_manager.",
    						this, peer_name, peer_port, key_stream_name);
    			srt->libsrt_close();
    			delete srt;
    			return client_count;
        	}
        	sls_relay_manager_ref puller_ref(puller_manager);   // add_relay_manager's reference

        	puller_manager->configure(m_map_data, m_map_publisher, m_list_role, m_port);

        	if (SLS_OK != puller_manager->start()) {
    			sls_peer_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, puller_manager->start failed, new client[%s:%d], stream='%s'.",
    						this, peer_name, peer_port, key_stream_name);
    			// One this request created has no puller to keep it: it must not
    			// keep counting against max_pull_streams. An existing one may be
    			// waiting to reconnect for players already attached: it stays.
    			puller_manager->start_failed(created, "first pull failed");
    			srt->libsrt_close();
    			delete srt;
    			return client_count;
            }
    	    sls_log(SLS_LOG_DEBUG, "[%p]CSLSListener::handler, puller_manager->start ok, new client[%s:%d], stream=%s.",
	            this, peer_name, peer_port, key_stream_name);

    	    pub = m_map_publisher->get_publisher(key_stream_name);
            if (NULL == pub) {
        	    sls_peer_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, m_map_publisher->get_publisher failed, new client[%s:%d], stream=%s.",
    	            this, peer_name, peer_port, key_stream_name);
    			srt->libsrt_close();
    			delete srt;
    			return client_count;
            } else {
        	    sls_log(SLS_LOG_DEBUG, "[%p]CSLSListener::handler, m_map_publisher->get_publisher ok, pub=%p, new client[%s:%d], stream=%s.",
    	            this, pub, peer_name, peer_port, key_stream_name);
            }
        }

        //3.2 handle new play
        if (!m_map_data->is_exist(key_stream_name)) {
            sls_peer_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, refused, new role[%s:%d], stream=%s,but publisher data doesn't exist in m_map_data.",
                        this, peer_name, peer_port, key_stream_name);
            srt->libsrt_close();
            delete srt;
            return client_count;
        }

		//new player
		if (srt->libsrt_socket_nonblock(0) < 0)
			sls_log(SLS_LOG_WARNING, "[%p]CSLSListener::handler, new player[%s:%d], libsrt_socket_nonblock failed.",
					this, peer_name, peer_port);

		CSLSPlayer * player = new CSLSPlayer;
		player->init();
		slot.hand_to(player);
		player->set_admission_timeout(m_admission_timeout);
		player->set_idle_streams_timeout(m_idle_streams_timeout_role);
		player->set_srt(srt);
		player->set_map_data(key_stream_name, m_map_data);
		player->attach_reader();
		//stat info
	    player->set_stat_identity(key_stream_name, peer_name, peer_port);
	    // on_connect is sent by the worker that adopts the role (Ruling R17):
	    // a role that expires while queued has announced nothing
	    attach_hook(player);

		// pub (and, once pushed, player) may already be gone: another thread
		// can tear a role down right after the lookup, so log pointer values
		// and the key, never what they point to
		m_list_role->push(player);
		sls_log(SLS_LOG_DEBUG, "[%p]CSLSListener::handler, new player[%p]=[%s:%d], key_stream_name=%s, publisher=%p, m_list_role->size=%d.",
				this, player, peer_name, peer_port, key_stream_name, pub, m_list_role->size());
        return client_count;
    }

    //4. is publisher?
    app_uplive = key_app;
	snprintf(key_stream_name, sizeof(key_stream_name), "%s/%s", app_uplive.c_str(), stream_name);
    ca = (sls_conf_app_t *)m_map_publisher->get_ca(app_uplive);
	if (NULL == ca) {
        sls_peer_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, refused, new role[%s:%d], stream='%s',but no sls_conf_app_t info.",
                    this, peer_name, peer_port, key_stream_name);
        srt->libsrt_close();
        delete srt;
        return client_count;
	}

	CSLSRole * publisher = m_map_publisher->get_publisher(key_stream_name);
	if (NULL != publisher) {
		sls_peer_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, refused, stream busy: new publisher[%s:%d], stream='%s' already has publisher=%p.",
					this, peer_name, peer_port, key_stream_name, publisher);
		srt->libsrt_close();
		delete srt;
		return client_count;
	}
	//create new publisher
	CSLSPublisher * pub = new CSLSPublisher;
	slot.hand_to(pub);
	pub->set_admission_timeout(m_admission_timeout);
	pub->set_srt(srt);
	pub->set_conf(ca);
	pub->init();
	pub->set_idle_streams_timeout(m_idle_streams_timeout_role);
	//stat info
    pub->set_stat_identity(key_stream_name, peer_name, peer_port);
    attach_hook(pub);

	sls_log(SLS_LOG_DEBUG, "[%p]CSLSListener::handler, new pub=%p, key_stream_name=%s.",
			this, pub, key_stream_name);

	//init data array
	if (SLS_OK != m_map_data->add(key_stream_name)) {
		sls_log(SLS_LOG_WARNING, "[%p]CSLSListener::handler, m_map_data->add failed, new pub[%s:%d], stream=%s.",
				this, peer_name, peer_port, key_stream_name);
		pub->uninit();
		delete pub;
		pub = NULL;
		return client_count;
	}

	if (SLS_OK != m_map_publisher->set_push_2_pushlisher(key_stream_name, pub)) {
		sls_log(SLS_LOG_WARNING, "[%p]CSLSListener::handler, m_map_publisher->set_push_2_pushlisher failed, key_stream_name=%s.",
					this, key_stream_name);
		pub->uninit();
		delete pub;
		pub = NULL;
		return client_count;
	}
	pub->set_map_publisher(m_map_publisher);
	pub->set_map_data(key_stream_name, m_map_data);
	// on_connect: sent by the adopting worker, see CSLSGroup::check_new_role
	m_list_role->push(pub);
	sls_log(SLS_LOG_DEBUG, "[%p]CSLSListener::handler, new publisher[%s:%d], key_stream_name=%s.",
			this, peer_name, peer_port, key_stream_name);

	//5. check pusher
	if (NULL == m_map_pusher) {
        return client_count;
	}
	CSLSRelayManager *pusher_manager = m_map_pusher->add_relay_manager(app_uplive.c_str(), stream_name);
	if (NULL == pusher_manager) {
		sls_log(SLS_LOG_DEBUG, "[%p]CSLSListener::handler, m_map_pusher->add_relay_manager failed, new role[%s:%d], key_stream_name=%s.",
					this, peer_name, peer_port, key_stream_name);
		return client_count;
	}
	sls_relay_manager_ref pusher_ref(pusher_manager);   // add_relay_manager's reference
	pusher_manager->configure(m_map_data, m_map_publisher, m_list_role, m_port);

	if (SLS_OK != pusher_manager->start()) {
		sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, pusher_manager->start failed, new role[%s:%d], key_stream_name=%s.",
					this, peer_name, peer_port, key_stream_name);
	}
	return client_count;
}



