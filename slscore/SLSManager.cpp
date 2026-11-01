
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
#include "SLSManager.hpp"
#include "SLSLog.hpp"
#include "SLSListener.hpp"
#include "SLSPublisher.hpp"

/**
 * srt conf
 */
SLS_CONF_DYNAMIC_IMPLEMENT_DEFAULTS(srt, sls_conf_srt_defaults)

/**
 * CSLSManager class implementation
 */
#define DEFAULT_GROUP 1

CSLSManager::CSLSManager()
{
    m_worker_threads = DEFAULT_GROUP;
    m_server_count = 1;
    m_list_role      = NULL;
    m_single_group   = NULL;

    m_map_data       = NULL;
    m_map_publisher  = NULL;
    m_map_puller     = NULL;
    m_map_pusher     = NULL;
    m_conn_limiter   = NULL;

}

CSLSManager::~CSLSManager()
{
}

/*
 * Startup is split in two so the caller can drop privileges in between:
 *   start_listeners(): bind + srt_listen (needs the privileged port), no threads.
 *                      The listeners only queue in m_list_role; nothing is
 *                      accepted or served until a worker polls them.
 *   start_workers():   create the groups and start their threads.
 */
int CSLSManager::start_listeners()
{
	int i = 0;

    //read config info from config file
    sls_conf_srt_t * conf_srt = (sls_conf_srt_t *)sls_conf_get_root_conf();

    if (!conf_srt) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSManager::start, no srt info, please check the conf file.", this);
        return SLS_ERROR;
    }
    //set log level
    if (strlen(conf_srt->log_level) > 0) {
        sls_set_log_level(conf_srt->log_level);
    }
    //set log file
    if (strlen(conf_srt->log_file) > 0) {
        sls_set_log_file(conf_srt->log_file);
    }

    sls_conf_server_t * conf_server = (sls_conf_server_t *)conf_srt->child;
    if (!conf_server) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSManager::start, no server info, please check the conf file.", this);
        return SLS_ERROR;
    }
    m_server_count = sls_conf_get_conf_count(conf_server);
    sls_conf_server_t * conf = conf_server;
    m_map_data      = new CSLSMapData[m_server_count];
    m_map_publisher = new CSLSMapPublisher[m_server_count];
    m_map_puller    = new CSLSMapRelay[m_server_count];
    m_map_pusher    = new CSLSMapRelay[m_server_count];

    //role list
    m_list_role = new CSLSRoleList;
    sls_log(SLS_LOG_INFO, "[%p]CSLSManager::start, new m_list_role=%p.", this, m_list_role);

    // Accepted connections are capped at what the workers can hold.
    {
        int threads = conf_srt->worker_threads > 1 ? conf_srt->worker_threads : 1;
        int max_total = conf_srt->worker_connections * threads;
        m_conn_limiter = new CSLSConnLimiter(max_total, conf_srt->max_connections_per_ip);
        sls_log(SLS_LOG_INFO, "[%p]CSLSManager::start, connection limits: total=%d (worker_connections=%d x %d), per_ip=%d%s.",
                this, max_total, conf_srt->worker_connections, threads, conf_srt->max_connections_per_ip,
                conf_srt->max_connections_per_ip == 0 ? " (unlimited)" : "");
    }

    //create listeners according config, delete by groups
    for (i = 0; i < m_server_count; i ++) {
    	CSLSListener * p = new CSLSListener();//deleted by groups
    	p->set_role_list(m_list_role);
        p->set_admission_limiter(m_conn_limiter);
        p->set_conf(conf);
        p->set_map_data("", &m_map_data[i]);
        p->set_map_publisher(&m_map_publisher[i]);
        p->set_map_puller(&m_map_puller[i]);
        p->set_map_pusher(&m_map_pusher[i]);
    	if (p->init() != SLS_OK) {
            sls_log(SLS_LOG_INFO, "[%p]CSLSManager::start, p->init failed.", this);
            return SLS_ERROR;
    	}
        if (p->start() != SLS_OK) {
            sls_log(SLS_LOG_INFO, "[%p]CSLSManager::start, p->start failed.", this);
            return SLS_ERROR;
        }
    	m_servers.push_back(p);
    	conf = (sls_conf_server_t *)conf->sibling;
    }
    sls_log(SLS_LOG_INFO, "[%p]CSLSManager::start, init listeners, count=%d.", this, m_server_count);
    return SLS_OK;
}

int CSLSManager::start_workers()
{
	int ret = 0;
	int i = 0;

    sls_conf_srt_t * conf_srt = (sls_conf_srt_t *)sls_conf_get_root_conf();
    if (!conf_srt) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSManager::start_workers, no srt info, please check the conf file.", this);
        return SLS_ERROR;
    }

    //create groups
    m_worker_threads = conf_srt->worker_threads;
    if (m_worker_threads == 0) {
        CSLSGroup * p = new CSLSGroup();
        p->set_worker_number(0);
        p->set_role_list(m_list_role);
        p->set_worker_connections(conf_srt->worker_connections);
        p->set_stat_post_interval(conf_srt->stat_post_interval);
        if (SLS_OK != p->init_epoll()) {
            sls_log(SLS_LOG_INFO, "[%p]CSLSManager::start, p->init_epoll failed.", this);
            return SLS_ERROR;
        }
        m_workers.push_back(p);
        m_single_group  =p;

    } else {
        for (i = 0; i < m_worker_threads; i ++) {
            CSLSGroup * p = new CSLSGroup();
            p->set_worker_number(i);
            p->set_role_list(m_list_role);
            p->set_worker_connections(conf_srt->worker_connections);
            p->set_stat_post_interval(conf_srt->stat_post_interval);
            if (SLS_OK != p->init_epoll()) {
                sls_log(SLS_LOG_INFO, "[%p]CSLSManager::start, p->init_epoll failed.", this);
                return SLS_ERROR;
            }
            p->start();
            m_workers.push_back(p);
        }
    }
    sls_log(SLS_LOG_INFO, "[%p]CSLSManager::start, init worker, count=%d.", this, m_worker_threads);

	return ret;

}

int CSLSManager::single_thread_handler()
{
    if (m_single_group) {
        return m_single_group->handler();
    }
    return SLS_OK;
}

bool CSLSManager::is_single_thread(){
    if (m_single_group)
        return true;
    return false;
}

int CSLSManager::stop()
{
	int ret = 0;
	int i = 0;
    //
    sls_log(SLS_LOG_INFO, "[%p]CSLSManager::stop.", this);

    // From here on a worker adopts no queued role: they are released below
    // with the role list, not handed to a worker that is about to stop.
    for (std::list<CSLSGroup *>::iterator it_w = m_workers.begin(); it_w != m_workers.end(); it_w++) {
        if (*it_w) (*it_w)->stop_intake();
    }

    //stop all listeners
    std::list<CSLSListener *>::iterator it;
    for ( it = m_servers.begin(); it != m_servers.end(); it++) {
    	CSLSListener *server = *it;
    	if (NULL == server) {
    		continue;
    	}
    	server->uninit();
    }
    m_servers.clear();

    std::list<CSLSGroup *>::iterator it_worker;
    for ( it_worker = m_workers.begin(); it_worker != m_workers.end(); it_worker++) {
    	CSLSGroup *p = *it_worker;
    	if (p) {
    		p->stop();
    		p->uninit_epoll();
    		delete p;
    		p = NULL;
    	}
    }
    m_workers.clear();

    // Roles still queued (accepted but never polled by a worker) reference the
    // maps below: Publisher::uninit() removes itself from them. Release the
    // queue now that no worker can pop from it, and only then free the maps.
    if(m_list_role) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSManager::stop, release rolelist, size=%d.", this, m_list_role->size());
    	m_list_role->erase();
    	delete m_list_role;
    	m_list_role = NULL;
    }

    if (m_map_data) {
    	delete[] m_map_data;
    	m_map_data = NULL;
    }
    if (m_map_publisher) {
    	delete[] m_map_publisher;
    	m_map_publisher = NULL;
    }

    if (m_map_puller) {
    	delete[] m_map_puller;
    	m_map_puller = NULL;
    }

    if (m_map_pusher) {
    	delete[] m_map_pusher;
    	m_map_pusher = NULL;
    }

    // every role that could hold a slot is gone: the groups deleted theirs,
    // the role list was erased above
    if (m_conn_limiter) {
        if (m_conn_limiter->total() != 0) {
            sls_log(SLS_LOG_WARNING, "[%p]CSLSManager::stop, %d connection slot(s) still held.",
                    this, m_conn_limiter->total());
        }
        delete m_conn_limiter;
        m_conn_limiter = NULL;
    }
    return ret;
}

int CSLSManager::expire_queued_roles(int64_t now_ms)
{
    if (NULL == m_list_role) {
        return 0;
    }
    return m_list_role->expire(now_ms, SLS_DEFAULT_ADMISSION_TIMEOUT);
}

int CSLSManager::reload()
{
    sls_log(SLS_LOG_INFO, "[%p]CSLSManager::reload begin.", this);

    //stop all listeners
    std::list<CSLSListener *>::iterator it;
    for ( it = m_servers.begin(); it != m_servers.end(); it++) {
    	CSLSListener *server = *it;
    	if (NULL == server) {
    		continue;
    	}
    	server->uninit();
    }
    m_servers.clear();

    //set all groups reload flag
    std::list<CSLSGroup *>::iterator it_worker;
    for ( it_worker = m_workers.begin(); it_worker != m_workers.end(); it_worker++) {
    	CSLSGroup *worker = *it_worker;
    	if (worker) {
    		worker->reload();
    	}
    }
	return 0;
}

int CSLSManager::reload_users()
{
    int reloaded = 0, failed = 0;
    for (std::list<CSLSListener *>::iterator it = m_servers.begin();
         it != m_servers.end(); it++) {
        CSLSListener *server = *it;
        if (server) {
            if (SLS_OK == server->reload_users()) reloaded++;
            else failed++;
        }
    }
    sls_log(failed ? SLS_LOG_ERROR : SLS_LOG_INFO,
            "[%p]CSLSManager::reload_users, %d listener(s) refreshed, %d failed.",
            this, reloaded, failed);
    return failed ? SLS_ERROR : SLS_OK;
}

int  CSLSManager::check_invalid()
{
    std::list<CSLSGroup *>::iterator it;
    std::list<CSLSGroup *>::iterator it_erase;
    std::list<CSLSGroup *>::iterator it_end = m_workers.end();
    for ( it = m_workers.begin(); it != it_end; ) {
    	CSLSGroup *worker = *it;
    	it_erase = it;
    	it++;
    	if (NULL == worker) {
			m_workers.erase(it_erase);
			continue;
    	}
		if (worker->is_exit()) {
			sls_log(SLS_LOG_INFO, "[%p]CSLSManager::check_invalid, delete worker=%p.",
					this, worker);
			worker->stop();
			worker->uninit_epoll();
			delete worker;
            m_workers.erase(it_erase);
		}
    }

    if (m_workers.size() == 0)
        return SLS_OK;
    return SLS_ERROR;
}

void CSLSManager::get_stat_info(std::string &info)
{
    std::list<CSLSGroup *>::iterator it;
    std::list<CSLSGroup *>::iterator it_end = m_workers.end();
    for ( it = m_workers.begin(); it != it_end; ) {
    	CSLSGroup *worker = *it;
    	it++;
    	if (NULL != worker) {
    		worker->get_stat_info(info);
    	}
    }
}

int  CSLSManager::stat_client_callback(void *p, HTTP_CALLBACK_TYPE type, void *v, void* context)
{
	CSLSManager *manager = (CSLSManager *)context;
	if (HCT_REQUEST_CONTENT == type) {
		std::string * p_response = (std::string *)v;
		// the whole document: the groups only contribute their role objects
		std::string roles;
		manager->get_stat_info(roles);
		*p_response = std::string("{\"server\":\"sls\",\"version\":\"") + SLS_VERSION_STRING +
			"\",\"generated_ms\":" + std::to_string(sls_gettime_ms()) +
			",\"roles\":[" + roles + "]}";
	} else if (HCT_RESPONSE_END == type) {
		//response info maybe include info that server send client, such as reload cmd...
	} else {

	}
	return SLS_OK;
}



