
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


#include "SLSMapRelay.hpp"

#include <errno.h>
#include <string.h>

#include "SLSPullerManager.hpp"
#include "SLSPusherManager.hpp"
#include "SLSLog.hpp"


/**
 * CSLSMapRelay class implementation
 */

CSLSMapRelay::CSLSMapRelay()
{
}

CSLSMapRelay::~CSLSMapRelay()
{
    clear();
}

CSLSRelayManager * CSLSMapRelay::add_relay_manager(const char *app_uplive, const char *stream_name)
{
    //find conf info
	SLS_RELAY_INFO * sri = get_relay_conf(std::string(app_uplive));
	if (NULL == sri) {
    	sls_log(SLS_LOG_INFO, "[%p]CSLSMapRelay::add_relay_manager, no relay conf info, app_uplive=%s, stream_name=%s.",
                this, app_uplive, stream_name);
		return NULL;
	}

    std::string key_stream_name = std::string(app_uplive) + std::string("/") + std::string(stream_name);
    bool is_pull = (strcmp(sri->m_type, "pull") == 0);
    CSLSRelayManager * cur_mananger = NULL;
    int  count = 0;
    bool existed = false;
    {
        CSLSLock lock(&m_rwclock, true);
        std::map<std::string, CSLSRelayManager *>::iterator item;
        item = m_map_relay_manager.find(key_stream_name);
        if (item != m_map_relay_manager.end() && NULL != item->second) {
            cur_mananger = item->second;
            cur_mananger->add_ref();     // the caller's
            existed = true;
        } else {
            if (is_pull) {
                for (item = m_map_relay_manager.begin(); item != m_map_relay_manager.end(); ++item) {
                    if (item->second && item->second->get_relay_conf() == sri) count++;
                }
            }
            if (is_pull && count >= sri->m_max_pull_streams) {
                // refused below, outside the lock
            } else if (is_pull) {
                cur_mananger = new CSLSPullerManager;
            } else if (strcmp(sri->m_type, "push") == 0) {
                cur_mananger = new CSLSPusherManager;
            }
            if (cur_mananger) {
                cur_mananger->set_relay_conf(sri);
                cur_mananger->set_relay_info(app_uplive, stream_name);
                cur_mananger->set_owner(this);
                m_map_relay_manager[key_stream_name] = cur_mananger;  // the map's reference
                cur_mananger->add_ref();                              // the caller's
                count++;
            }
        }
    }

    if (existed) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSMapRelay::add, cur_mananger=%p, exist, app_uplive=%s, stream_name=%s.",
                this, cur_mananger, app_uplive, stream_name);
    } else if (cur_mananger) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSMapRelay::add_relay_manager, ok, app_uplive=%s, stream_name=%s, cur_mananger=%p%s.",
                this, app_uplive, stream_name, cur_mananger,
                is_pull ? (std::string(", pull managers=") + std::to_string(count) + "/" + std::to_string(sri->m_max_pull_streams)).c_str() : "");
    } else if (is_pull) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSMapRelay::add_relay_manager, refused, app_uplive=%s, stream_name=%s: max_pull_streams=%d reached.",
                this, app_uplive, stream_name, sri->m_max_pull_streams);
    } else {
        sls_log(SLS_LOG_INFO, "[%p]CSLSMapRelay::add, failed, wrong type='%s', app_uplive=%s, stream_name=%s.",
                this, sri->m_type, app_uplive, stream_name);
    }
    return cur_mananger;
}

int CSLSMapRelay::remove_relay_manager(const char *app_uplive, const char *stream_name,
                                       CSLSRelayManager *expected)
{
    std::string key_stream_name = std::string(app_uplive) + std::string("/") + std::string(stream_name);
    CSLSRelayManager *mgr = NULL;
    {
        CSLSLock lock(&m_rwclock, true);
        std::map<std::string, CSLSRelayManager *>::iterator item = m_map_relay_manager.find(key_stream_name);
        if (item != m_map_relay_manager.end() && (NULL == expected || item->second == expected)) {
            mgr = item->second;
            m_map_relay_manager.erase(item);
        }
    }
    if (NULL == mgr) {
        return SLS_ERROR;
    }
    sls_log(SLS_LOG_INFO, "[%p]CSLSMapRelay::remove_relay_manager, ok, app_uplive=%s, stream_name=%s, manager=%p.",
            this, app_uplive, stream_name, mgr);
    mgr->release();   // the map's reference; holders keep it alive
    return SLS_OK;
}

void CSLSMapRelay::clear()
{
    CSLSLock lock(&m_rwclock, true);
	sls_log(SLS_LOG_INFO, "[%p]CSLSMapRelay::clear.",
            this);

    std::map<std::string, CSLSRelayManager *>::iterator it;
    for(it=m_map_relay_manager.begin(); it!=m_map_relay_manager.end(); ) {
        CSLSRelayManager * relay_manager = it->second;
        if (NULL != relay_manager) {
		    relay_manager->release();   // the map's reference
		}
    	it ++;
    }
    m_map_relay_manager.clear();

    std::map<std::string, SLS_RELAY_INFO *>::iterator it_sri;
    for(it_sri=m_map_relay_info.begin(); it_sri!=m_map_relay_info.end(); ) {
        SLS_RELAY_INFO * sri = it_sri->second;
        if (NULL != sri) {
		    delete sri;
		}
        it_sri ++;
    }
    m_map_relay_info.clear();
}

int CSLSMapRelay::add_relay_conf(std::string app_uplive, sls_conf_relay_t * cr)
{
    if (NULL == cr) {
    	sls_log(SLS_LOG_INFO, "[%p]CSLSMapRelay::add_app_conf, failed, cr is null.",
                this);
    	return SLS_ERROR;
    }

    SLS_RELAY_INFO * sri = get_relay_conf(app_uplive);
    if (NULL != sri) {
    	sls_log(SLS_LOG_INFO, "[%p]CSLSMapRelay::add_app_conf, failed, sri exist, app_uplive=%s.",
                this, app_uplive.c_str());
    	return SLS_ERROR;
    }
    sri = new SLS_RELAY_INFO;
    strcpy(sri->m_type, cr->type);
    sri->m_reconnect_interval   = cr->reconnect_interval;
    sri->m_idle_streams_timeout = cr->idle_streams_timeout;
    sri->m_max_pull_streams     = cr->max_pull_streams;
    sri->m_live_relays          = 0;

    if (strcmp(cr->mode, "loop") == 0) {
    	sri->m_mode = SLS_PM_LOOP;
    } else if (strcmp(cr->mode, "all") == 0) {
    	sri->m_mode = SLS_PM_ALL;
    } else if (strcmp(cr->mode, "hash") == 0) {
    	sri->m_mode = SLS_PM_HASH;
    } else {
    	sri->m_mode = SLS_PM_HASH;
    	sls_log(SLS_LOG_INFO, "[%p]CSLSMapRelay::add_app_conf, wrong mode='%s', use default SLS_PM_LOOP.",
                this, cr->mode);
    }

    //parse upstreams
    sri->m_upstreams = sls_conf_string_split(string(cr->upstreams), string(" "));
    if (sri->m_upstreams.size() == 0) {
        // the listener rejects this at startup (sls_relay_conf_check); never
        // keep a relay that would divide by zero picking an upstream
        sls_log(SLS_LOG_ERROR, "[%p]CSLSMapRelay::add_app_conf, no upstreams, app_uplive=%s.", this, app_uplive.c_str());
        delete sri;
        return SLS_ERROR;
    }
    if (SLS_OK != sri->resolve_upstreams()) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSMapRelay::add_app_conf, cannot resolve the upstreams '%s', relay refused, app_uplive=%s.",
                this, cr->upstreams, app_uplive.c_str());
        delete sri;
        return SLS_ERROR;
    }
    m_map_relay_info[app_uplive] = sri;
    return SLS_OK;

}

SLS_RELAY_INFO *CSLSMapRelay::get_relay_conf(std::string app_uplive)
{
    SLS_RELAY_INFO * sri = NULL;
	std::map<std::string, SLS_RELAY_INFO *>::iterator item;
    item = m_map_relay_info.find(app_uplive);
    if (item != m_map_relay_info.end()) {
    	sri = item->second;
    }
    return sri;

}




