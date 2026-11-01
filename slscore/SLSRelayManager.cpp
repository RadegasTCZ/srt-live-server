
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

#include "common.hpp"
#include "SLSRelayManager.hpp"
#include "SLSMapRelay.hpp"
#include "SLSLog.hpp"


/**
 * SLS_RELAY_INFO: cached upstream addresses
 */
static std::string upstream_key(const char *host, int port)
{
	char buf[16];
	snprintf(buf, sizeof(buf), ":%d", port);
	return std::string(host) + buf;
}

// Resolves every upstream of this relay block. Nothing is stored here, so a
// failed refresh leaves the cache as it was.
int SLS_RELAY_INFO::resolve_all(std::map<std::string, struct sockaddr_in> *out)
{
	for (size_t i = 0; i < m_upstreams.size(); i ++) {
		char host[256];
		int  port = 0;
		if (SLS_OK != sls_relay_split_hostport(m_upstreams[i].c_str(), host, sizeof(host), &port)) {
			sls_log(SLS_LOG_ERROR, "SLS_RELAY_INFO::resolve_all, upstream='%s' must start with 'hostname:port'.",
					m_upstreams[i].c_str());
			return SLS_ERROR;
		}
		struct sockaddr_storage ss;
		socklen_t len = 0;
		if (SLS_OK != sls_resolve(host, port, &ss, &len, AF_INET)) {
			return SLS_ERROR;
		}
		(*out)[upstream_key(host, port)] = *(struct sockaddr_in *)&ss;
	}
	return SLS_OK;
}

int SLS_RELAY_INFO::resolve_upstreams()
{
	std::map<std::string, struct sockaddr_in> addrs;
	if (SLS_OK != resolve_all(&addrs)) {
		return SLS_ERROR;
	}
	std::lock_guard<std::mutex> lock(m_addr_mutex);
	m_addrs.swap(addrs);
	return SLS_OK;
}

int SLS_RELAY_INFO::lookup(const char *host, int port, struct sockaddr_in *out)
{
	std::lock_guard<std::mutex> lock(m_addr_mutex);
	std::map<std::string, struct sockaddr_in>::iterator it = m_addrs.find(upstream_key(host, port));
	if (it == m_addrs.end()) {
		return SLS_ERROR;
	}
	*out = it->second;
	return SLS_OK;
}

void SLS_RELAY_INFO::refresh_if_stale()
{
	if (!m_stale.exchange(false)) {
		return;
	}
	// no lock across the DNS calls; a failed refresh keeps the old addresses
	resolve_upstreams();
}

/**
 * CSLSRelayManager class implementation
 */
CSLSRelayManager::CSLSRelayManager()
{
	m_reconnect_begin_tm = 0;
    m_map_publisher      = NULL;
    m_map_data           = NULL;
    m_role_list          = NULL;
    m_sri                = NULL;
    m_listen_port        = 0;
    m_retired            = false;
    m_refs               = 1;      // the creator's (the map entry)
    m_live_relays        = 0;
    m_owner              = NULL;

    memset(m_app_uplive, 0, sizeof(m_app_uplive));
    memset(m_stream_name, 0, sizeof(m_stream_name));

}

CSLSRelayManager::~CSLSRelayManager()
{
}

void CSLSRelayManager::add_ref()
{
	m_refs++;
}

void CSLSRelayManager::release()
{
	if (--m_refs == 0) {
		sls_log(SLS_LOG_INFO, "[%p]CSLSRelayManager::release, last reference gone, delete, app_uplive=%s, stream_name=%s.",
				this, m_app_uplive, m_stream_name);
		delete this;
	}
}

void CSLSRelayManager::relay_attached_locked(CSLSRelay *relay)
{
	relay->set_relay_manager(this);
	relay->set_live_counted(true);
	m_live_relays++;
	int n = m_sri ? ++m_sri->m_live_relays : 0;
	sls_log(SLS_LOG_INFO, "[%p]CSLSRelayManager::relay_attached, live relays app=%s n=%d max_pull_streams=%d, stream_name=%s.",
			this, m_app_uplive, n, m_sri ? m_sri->m_max_pull_streams : 0, m_stream_name);
}

bool CSLSRelayManager::relay_ended(CSLSRelay *relay, bool may_reconnect)
{
	CSLSLock lock(&m_mutex);
	if (relay->live_counted()) {
		relay->set_live_counted(false);
		m_live_relays--;
		int n = m_sri ? --m_sri->m_live_relays : 0;
		sls_log(SLS_LOG_INFO, "[%p]CSLSRelayManager::relay_detached, live relays app=%s n=%d, stream_name=%s.",
				this, m_app_uplive, n, m_stream_name);
	}
	if (m_retired) {
		return false;
	}
	if (may_reconnect) {
		if (m_sri) m_sri->mark_stale();    // resolve the upstreams again before the retry
		return do_on_relay_closed(relay);
	}
	// nobody will hold a reconnect entry: do not leave a manager that
	// neither runs nor waits
	if (retires_when_idle()) {
		retire_locked("relay torn down outside its worker");
	}
	return false;
}

bool CSLSRelayManager::is_retired()
{
	CSLSLock lock(&m_mutex);
	return m_retired;
}

void CSLSRelayManager::configure(CSLSMapData *map_data, CSLSMapPublisher *map_publisher,
                                 CSLSRoleList *role_list, int listen_port)
{
	CSLSLock lock(&m_mutex);
	m_map_data      = map_data;
	m_map_publisher = map_publisher;
	m_role_list     = role_list;
	m_listen_port   = listen_port;
}

void CSLSRelayManager::set_owner(CSLSMapRelay *owner)
{
	m_owner = owner;
}

int CSLSRelayManager::start()
{
	CSLSLock lock(&m_mutex);
	if (m_retired) {
		sls_peer_log(SLS_LOG_INFO, "[%p]CSLSRelayManager::start, retired, app_uplive=%s, stream_name=%s.",
				this, m_app_uplive, m_stream_name);
		return SLS_ERROR;
	}
	return do_start();
}

int CSLSRelayManager::reconnect(int64_t cur_tm_ms)
{
	CSLSLock lock(&m_mutex);
	if (m_retired) {
		return SLS_OK;   // nothing left to do: drop the reconnect entry
	}
	return do_reconnect(cur_tm_ms);
}

bool CSLSRelayManager::do_on_relay_closed(CSLSRelay *relay)
{
	add_reconnect_stream(relay->get_url());
	return true;
}

void CSLSRelayManager::retire_if_idle(const char *why)
{
	CSLSLock lock(&m_mutex);
	retire_locked(why);
}

bool CSLSRelayManager::retire_locked(const char *why)
{
	if (m_retired) {
		return true;
	}
	if (m_live_relays > 0) {
		sls_log(SLS_LOG_INFO, "[%p]CSLSRelayManager::retire, not retired (%s): %d relay(s) still live, app_uplive=%s, stream_name=%s.",
				this, why, (int)m_live_relays, m_app_uplive, m_stream_name);
		return false;
	}
	m_retired = true;
	sls_log(SLS_LOG_INFO, "[%p]CSLSRelayManager::retire, %s, app_uplive=%s, stream_name=%s.",
			this, why, m_app_uplive, m_stream_name);
	// Lock order is always manager -> map (the map never takes a manager's
	// mutex). The caller holds a reference, so the map's release cannot free
	// this manager here.
	if (NULL != m_owner) {
		m_owner->remove_relay_manager(m_app_uplive, m_stream_name, this);
	}
	return true;
}

void CSLSRelayManager::set_relay_conf(SLS_RELAY_INFO *sri)
{
	m_sri = sri;
}

void CSLSRelayManager::set_relay_info(const char *app_uplive, const char *stream_name)
{
	snprintf(m_app_uplive, sizeof(m_app_uplive), "%s", app_uplive);
	snprintf(m_stream_name, sizeof(m_stream_name), "%s", stream_name);
}

int CSLSRelayManager::connect(const char *url)
{
	int ret = SLS_ERROR;
	if (url == NULL || strlen(url) == 0) {
	    sls_log(SLS_LOG_INFO, "[%p]CSLSRelayManager::connect, failed, url=%s.", this, url?url:"null");
        return ret;
	}

	if (NULL == m_sri) {
		return ret;
	}
	// no DNS here (a worker, under m_mutex): open() only reads the cache,
	// which the main loop refreshes once a relay end marked it stale

	CSLSRelay * cur_relay = create_relay();//new relay;
    cur_relay->init();
    ret = cur_relay->open(url, m_sri);
    if (SLS_OK == ret) {
    	cur_relay->set_idle_streams_timeout(m_sri->m_idle_streams_timeout);

    	//set stat info
        char relay_peer_name[IP_MAX_LEN] = {0};
        int  relay_peer_port = 0;
        cur_relay->get_peer_info(relay_peer_name, relay_peer_port);
        std::string stat_stream = std::string(m_app_uplive) + "/" + m_stream_name;
        cur_relay->set_stat_identity(stat_stream.c_str(), relay_peer_name, relay_peer_port);

    	ret = set_relay_param(cur_relay);
    	if (SLS_OK != ret) {
    		cur_relay->uninit();
    		delete cur_relay;
    		cur_relay = NULL;
    	}
	    return ret;
    } else {
    	cur_relay->uninit();
    	delete cur_relay ;
    	cur_relay = NULL;
    	m_sri->mark_stale();
    }
	return ret;
}

int CSLSRelayManager::connect_hash()
{
	//make hash to hostnames by stream_name
	std::string url = get_hash_url();
	char szURL[1024] = {0};
	const char *szTmp = url.c_str();
	snprintf(szURL, sizeof(szURL), "srt://%s/%s", szTmp, m_stream_name);
	int ret = connect(szURL);
	if (SLS_OK != ret) {
	    sls_log(SLS_LOG_INFO, "[%p]CSLSRelayManager::connect_hash, faild, connect szURL=%s, m_stream_name=%s.",
	    		this, szURL, m_stream_name);
	} else {
        sls_log(SLS_LOG_INFO, "[%p]CSLSRelayManager::connect_hash, ok, connect szURL=%s, m_stream_name=%s.",
    		    this, szURL, m_stream_name);
	}
    return ret;
}


std::string CSLSRelayManager::get_hash_url()
{
	if (NULL == m_sri || m_sri->m_upstreams.empty()) {
		return "";
	}
	uint32_t key = sls_hash_key(m_stream_name, strlen(m_stream_name));
	uint32_t index = key % m_sri->m_upstreams.size();
	return m_sri->m_upstreams[index];

}
