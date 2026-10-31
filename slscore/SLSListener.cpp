
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
#include <fstream>
#include <sstream>
#include <set>
#include <vector>

#include "SLSListener.hpp"
#include "SLSLog.hpp"
#include "SLSPublisher.hpp"
#include "SLSPlayer.hpp"
#include "SLSPullerManager.hpp"
#include "SLSPusherManager.hpp"

const char SLS_SERVER_STAT_INFO_BASE[] = "\
{\
\"port\": \"%d\",\
\"role\": \"%s\",\
\"pub_domain_app\": \"\",\
\"stream_name\": \"\",\
\"url\": \"\",\
\"remote_ip\": \"\",\
\"remote_port\": \"\",\
\"start_time\": \"%s\",\
\"kbitrate\": \"0\"\
}";

const char SLS_PUBLISHER_STAT_INFO_BASE[] = "\
{\
\"port\": \"%d\",\
\"role\": \"%s\",\
\"pub_domain_app\": \"%s\",\
\"stream_name\": \"%s\",\
\"url\": \"%s\",\
\"remote_ip\": \"%s\",\
\"remote_port\": \"%d\",\
\"start_time\": \"%s\",\
\"kbitrate\":\
";

const char SLS_PLAYER_STAT_INFO_BASE[] = "\
{\
\"port\": \"%d\",\
\"role\": \"%s\",\
\"pub_domain_app\": \"%s\",\
\"stream_name\": \"%s\",\
\"url\": \"%s\",\
\"remote_ip\": \"%s\",\
\"remote_port\": \"%d\",\
\"start_time\": \"%s\",\
\"kbitrate\":\
";

/**
 * server conf
 */
SLS_CONF_DYNAMIC_IMPLEMENT(server)

/**
 * CSLSListener class implementation
 */

CSLSListener::CSLSListener()
{
    m_conf      = NULL;
    m_back_log  = 1024;
    m_is_write  = 0;
    m_port      = 0;

    m_list_role     = NULL;
    m_map_publisher = NULL;
    m_map_puller    = NULL;
    m_idle_streams_timeout      = UNLIMITED_TIMEOUT;
    m_idle_streams_timeout_role = 0;
    m_stat_info = std::string("");
    memset(m_http_url_role, 0, URL_MAX_LEN);

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
    snprintf(m_http_url_role, sizeof(m_http_url_role), "%s", conf_server->on_event_url);
    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::init_conf_app, m_back_log=%d, m_idle_streams_timeout=%d.",
            this, m_back_log, m_idle_streams_timeout_role);

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
    char msg[512];
    std::ifstream f(path);
    if (!f.is_open()) {
        snprintf(msg, sizeof(msg), "cannot open '%s': %s", path, strerror(errno));
        if (err) *err = msg;
        return SLS_ERROR;
    }

    // Parse into a staging map, then swap under the write lock so
    // listen_callback never observes a half-loaded m_users.
    std::map<std::string, sls_user_t> staging;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        std::string::size_type hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);

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
            snprintf(msg, sizeof(msg), "%s:%d: invalid user name '%s' "
                     "(allowed: 1-128 of A-Z a-z 0-9 . _ -, not '.' or '..')",
                     path, lineno, sls_escape_for_log(name.c_str()).c_str());
            if (err) *err = msg;
            return SLS_ERROR;
        }
        if (pub  == "-") pub.clear();
        if (play == "-") play.clear();

        if (!sls_is_valid_passphrase_len(pub.length())) {
            snprintf(msg, sizeof(msg), "%s:%d: publish_pass length %zu out of range "
                     "(SRT requires 10-79 bytes, or '-' for plaintext) for user '%s'",
                     path, lineno, pub.length(), name.c_str());
            if (err) *err = msg;
            return SLS_ERROR;
        }
        if (!sls_is_valid_passphrase_len(play.length())) {
            snprintf(msg, sizeof(msg), "%s:%d: play_pass length %zu out of range "
                     "(SRT requires 10-79 bytes, or '-' for plaintext) for user '%s'",
                     path, lineno, play.length(), name.c_str());
            if (err) *err = msg;
            return SLS_ERROR;
        }
        if (staging.count(name)) {
            snprintf(msg, sizeof(msg), "%s:%d: duplicate user '%s'", path, lineno, name.c_str());
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

int CSLSListener::reload_users()
{
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
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::listen_callback, empty streamid; rejecting.", self);
        return -1;
    }

    // Expect streamid of the form "host/app/stream" with strict charset.
    sls_sid_t sid;
    if (!sls_sid_parse(streamid, &sid)) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::listen_callback, refused: streamid must be host/app/stream, sid='%s'.",
                self, sls_escape_for_log(streamid).c_str());
        return -1;
    }
    const std::string &stream = sid.stream;

    std::string app_key;
    sls_sid_role_t role = self->classify_sid(sid, &app_key);
    if (SLS_SID_REJECT == role) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSListener::listen_callback, refused: '%s/%s' is neither a publisher nor a player app (user '%s').",
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
            sls_log(SLS_LOG_INFO, "[%p]CSLSListener::listen_callback, unknown user '%s'; rejecting.",
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
            sls_log(SLS_LOG_ERROR,
                    "[%p]CSLSListener::listen_callback, SRTO_PBKEYLEN=%d for user '%s' (%s) failed: %s.",
                    self, pbkeylen, stream.c_str(), direction, srt_getlasterror_str());
            return -1;
        }
        if (srt_setsockflag(ns, SRTO_PASSPHRASE, pass_value.c_str(), (int)pass_value.length()) < 0) {
            sls_log(SLS_LOG_ERROR,
                    "[%p]CSLSListener::listen_callback, SRTO_PASSPHRASE for user '%s' (%s) failed: %s.",
                    self, stream.c_str(), direction, srt_getlasterror_str());
            return -1;
        }
        int enforce = 1;
        if (srt_setsockflag(ns, SRTO_ENFORCEDENCRYPTION, &enforce, sizeof(enforce)) < 0) {
            sls_log(SLS_LOG_ERROR,
                    "[%p]CSLSListener::listen_callback, SRTO_ENFORCEDENCRYPTION for user '%s' (%s) failed: %s.",
                    self, stream.c_str(), direction, srt_getlasterror_str());
            return -1;
        }
        sls_log(SLS_LOG_INFO, "[%p]CSLSListener::listen_callback, accept user '%s' (%s, encrypted).",
                self, stream.c_str(), direction);
    } else {
        sls_log(SLS_LOG_INFO, "[%p]CSLSListener::listen_callback, accept user '%s' (%s, plaintext).",
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

    ret = m_srt->libsrt_listen(m_back_log);
    if (SLS_OK != ret) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSListener::start, libsrt_listen failure.", this);
        return ret;
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


int CSLSListener::handler()
{
	int ret = SLS_OK;
	int fd_client = 0;
	CSLSSrt *srt = NULL;
	char sid[1024] = {0};
	int  sid_size = sizeof(sid);
	sls_sid_t psid;
	char key_app[URL_MAX_LEN] = {0};
	char key_stream_name[URL_MAX_LEN] = {0};
    char tmp[URL_MAX_LEN] = {0};
    char peer_name[IP_MAX_LEN] = {0};
    int  peer_port = 0;
    int  client_count = 0;

    //1: accept
    fd_client = m_srt->libsrt_accept();
    if (fd_client < 0) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, srt_accept failed, fd=%d.", this, get_fd());
        CSLSSrt::libsrt_neterrno();
        return client_count;
    }
    client_count = 1;

    //2.check streamid, split it
	srt = new CSLSSrt;
	srt->libsrt_set_fd(fd_client);
    ret = srt->libsrt_getpeeraddr(peer_name, peer_port);
    if (ret != 0) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, libsrt_getpeeraddr failed, fd=%d.", this, srt->libsrt_get_fd());
        srt->libsrt_close();
        delete srt;
        return client_count;
    }
    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, new client[%s:%d], fd=%d.", this, peer_name, peer_port, fd_client);

    if (0 != srt->libsrt_getsockopt(SRTO_STREAMID, "SRTO_STREAMID", &sid, &sid_size)) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, [%s:%d], fd=%d, get streamid info failed.",
                this, peer_name, peer_port, srt->libsrt_get_fd());
    	srt->libsrt_close();
        delete srt;
    	return client_count;
    }

    if (!sls_sid_parse(sid, &psid)) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, [%s:%d], refused: streamid must be host/app/stream, sid='%s'.",
                this, peer_name, peer_port, sls_escape_for_log(sid).c_str());
    	srt->libsrt_close();
    	delete srt;
    	return client_count;
    }
    const char *stream_name = psid.stream.c_str();
    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, [%s:%d], sid '%s/%s/%s'",
            this, peer_name, peer_port, psid.host.c_str(), psid.app.c_str(), stream_name);

    // app exist?
    snprintf(key_app, sizeof(key_app), "%s/%s", psid.host.c_str(), psid.app.c_str());

    std::string app_uplive = "";
    sls_conf_app_t * ca = NULL;
    sls_sid_role_t role = classify_sid(psid, &app_uplive);
    if (SLS_SID_REJECT == role) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, refused, new role[%s:%d], '%s' is neither a publisher nor a player app.",
                this, peer_name, peer_port, key_app);
        srt->libsrt_close();
        delete srt;
        return client_count;
    }

    char cur_time[STR_DATE_TIME_LEN] = {0};
    sls_gettime_default_string(cur_time);

    //3.is player?
    if (SLS_SID_PLAYER == role) {
        snprintf(key_stream_name, sizeof(key_stream_name), "%s/%s", app_uplive.c_str(), stream_name);
        CSLSRole * pub = m_map_publisher->get_publisher(key_stream_name);
        if (NULL == pub) {
        	//*
        	//3.1 check pullers
        	if (NULL == m_map_puller) {
    			sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, refused, new role[%s:%d], stream='%s', publisher is NULL and m_map_puller is NULL.",
    						this, peer_name, peer_port, key_stream_name);
    			srt->libsrt_close();
    			delete srt;
    			return client_count;
        	}
        	CSLSRelayManager *puller_manager = m_map_puller->add_relay_manager(app_uplive.c_str(), stream_name);
        	if (NULL == puller_manager) {
    			sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, m_map_puller->add_relay_manager failed, new role[%s:%d], stream='%s', publisher is NULL, no puller_manager.",
    						this, peer_name, peer_port, key_stream_name);
    			srt->libsrt_close();
    			delete srt;
    			return client_count;
        	}

        	puller_manager->set_map_data(m_map_data);
        	puller_manager->set_map_publisher(m_map_publisher);
        	puller_manager->set_role_list(m_list_role);
        	puller_manager->set_listen_port(m_port);

        	if (SLS_OK != puller_manager->start()) {
    			sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, puller_manager->start failed, new client[%s:%d], stream='%s'.",
    						this, peer_name, peer_port, key_stream_name);
    			srt->libsrt_close();
    			delete srt;
    			return client_count;
            }
    	    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, puller_manager->start ok, new client[%s:%d], stream=%s.",
	            this, peer_name, peer_port, key_stream_name);

    	    pub = m_map_publisher->get_publisher(key_stream_name);
            if (NULL == pub) {
        	    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, m_map_publisher->get_publisher failed, new client[%s:%d], stream=%s.",
    	            this, peer_name, peer_port, key_stream_name);
    			srt->libsrt_close();
    			delete srt;
    			return client_count;
            } else {
        	    sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, m_map_publisher->get_publisher ok, pub=%p, new client[%s:%d], stream=%s.",
    	            this, pub, peer_name, peer_port, key_stream_name);
            }
        }

        //3.2 handle new play
        if (!m_map_data->is_exist(key_stream_name)) {
            sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, refused, new role[%s:%d], stream=%s,but publisher data doesn't exist in m_map_data.",
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
		player->set_idle_streams_timeout(m_idle_streams_timeout_role);
		player->set_srt(srt);
		player->set_map_data(key_stream_name, m_map_data);
		//stat info
	    snprintf(tmp, sizeof(tmp), SLS_PLAYER_STAT_INFO_BASE,
	    		m_port, player->get_role_name(), app_uplive.c_str(), stream_name, sid, peer_name, peer_port, cur_time);
	    std::string stat_info = std::string(tmp);
	    player->set_stat_info_base(stat_info);
	    player->set_http_url(m_http_url_role);
	    player->on_connect();

		m_list_role->push(player);
		sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, new player[%p]=[%s:%d], key_stream_name=%s, %s=%p, m_list_role->size=%d.",
				this, player, peer_name, peer_port, key_stream_name, pub->get_role_name(), pub, m_list_role->size());
        return client_count;
    }

    //4. is publisher?
    app_uplive = key_app;
	snprintf(key_stream_name, sizeof(key_stream_name), "%s/%s", app_uplive.c_str(), stream_name);
    ca = (sls_conf_app_t *)m_map_publisher->get_ca(app_uplive);
	if (NULL == ca) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, refused, new role[%s:%d], stream='%s',but no sls_conf_app_t info.",
                    this, peer_name, peer_port, key_stream_name);
        srt->libsrt_close();
        delete srt;
        return client_count;
	}

	CSLSRole * publisher = m_map_publisher->get_publisher(key_stream_name);
	if (NULL != publisher) {
		sls_log(SLS_LOG_ERROR, "[%p]CSLSListener::handler, refused, new role[%s:%d], stream='%s',but publisher=%p is not NULL.",
					this, peer_name, peer_port, key_stream_name, publisher);
		srt->libsrt_close();
		delete srt;
		return client_count;
	}
	//create new publisher
	CSLSPublisher * pub = new CSLSPublisher;
	pub->set_srt(srt);
	pub->set_conf(ca);
	pub->init();
	pub->set_idle_streams_timeout(m_idle_streams_timeout_role);
	//stat info
    snprintf(tmp, sizeof(tmp), SLS_PUBLISHER_STAT_INFO_BASE,
    		m_port, pub->get_role_name(), app_uplive.c_str(), stream_name, sid, peer_name, peer_port, cur_time);
    std::string stat_info = std::string(tmp);
    pub->set_stat_info_base(stat_info);
    pub->set_http_url(m_http_url_role);

	sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, new pub=%p, key_stream_name=%s.",
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
	pub->on_connect();
	m_list_role->push(pub);
	sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, new publisher[%s:%d], key_stream_name=%s.",
			this, peer_name, peer_port, key_stream_name);

	//5. check pusher
	if (NULL == m_map_pusher) {
        return client_count;
	}
	CSLSRelayManager *pusher_manager = m_map_pusher->add_relay_manager(app_uplive.c_str(), stream_name);
	if (NULL == pusher_manager) {
		sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, m_map_pusher->add_relay_manager failed, new role[%s:%d], key_stream_name=%s.",
					this, peer_name, peer_port, key_stream_name);
		return client_count;
	}
	pusher_manager->set_map_data(m_map_data);
	pusher_manager->set_map_publisher(m_map_publisher);
	pusher_manager->set_role_list(m_list_role);
	pusher_manager->set_listen_port(m_port);

	if (SLS_OK != pusher_manager->start()) {
		sls_log(SLS_LOG_INFO, "[%p]CSLSListener::handler, pusher_manager->start failed, new role[%s:%d], key_stream_name=%s.",
					this, peer_name, peer_port, key_stream_name);
	}
	return client_count;
}

std::string   CSLSListener::get_stat_info()
{
	if (m_stat_info.length() == 0) {
	    char tmp[STR_MAX_LEN] = {0};
	    char cur_time[STR_DATE_TIME_LEN] = {0};
	    sls_gettime_default_string(cur_time);
	    snprintf(tmp, sizeof(tmp), SLS_SERVER_STAT_INFO_BASE, m_port, m_role_name, cur_time);
	    m_stat_info = std::string(tmp);
	}
	return m_stat_info;
}



