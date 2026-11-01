
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


#include "SLSPublisher.hpp"
#include "SLSPlayer.hpp"
#include "SLSLog.hpp"
#include "SLSMapRelay.hpp"

/**
 * app conf
 */
SLS_CONF_DYNAMIC_IMPLEMENT(app)

/**
 * CSLSPublisher class implementation
 */

CSLSPublisher::CSLSPublisher()
{
    m_is_write             = 0;
    m_map_publisher        = NULL;
    m_registered           = false;

    sprintf(m_role_name, "publisher");

}

CSLSPublisher::~CSLSPublisher()
{
    //release
}

int CSLSPublisher::init()
{
    return CSLSRole::init();
}

int CSLSPublisher::uninit()
{
    int ret = SLS_OK;

	// Only what admit() registered: a publisher refused (or never admitted)
	// owns no map data, and the stream's data may be another publisher's.
	if (m_registered) {
		m_registered = false;
		if (m_map_data) {
			ret = m_map_data->remove(m_map_data_key);
			sls_log(SLS_LOG_DEBUG, "[%p]CSLSPublisher::uninit, removed publisher from m_map_data, ret=%d.",
					this, ret);
		}
		if (m_map_publisher) {
			ret = m_map_publisher->remove(this);
			sls_log(SLS_LOG_DEBUG, "[%p]CSLSPublisher::uninit, removed publisher from m_map_publisher, ret=%d.",
					this, ret);
		}
	}
    return CSLSRole::uninit();
}

// Registers the stream and starts its pushes. The publisher entry is
// claimed first, atomically under the map's lock: of two publishers admitted
// for one name (on two workers, or one admitted while a hook was pending for
// the other), the first wins and the other is refused before it touched
// anything. The map data follows; a player that looks in between sees a
// publisher without data and is refused, as if it had come a moment early.
int CSLSPublisher::admit()
{
	if (NULL == m_map_publisher || NULL == m_map_data) {
		sls_log(SLS_LOG_ERROR, "[%p]CSLSPublisher::admit, no map, stream=%s.", this, m_map_data_key);
		return SLS_ERROR;
	}
	if (SLS_OK != m_map_publisher->set_push_2_pushlisher(m_map_data_key, this)) {
		sls_peer_log(SLS_LOG_INFO, "[%p]CSLSPublisher::admit, refused, stream busy: publisher[%s:%d], stream='%s' already has a publisher.",
				this, m_stat_remote_ip.c_str(), m_stat_remote_port, m_map_data_key);
		return SLS_ERROR;
	}
	if (SLS_OK != m_map_data->add(m_map_data_key)) {
		sls_log(SLS_LOG_WARNING, "[%p]CSLSPublisher::admit, m_map_data->add failed, publisher[%s:%d], stream=%s.",
				this, m_stat_remote_ip.c_str(), m_stat_remote_port, m_map_data_key);
		m_map_publisher->remove(this);
		return SLS_ERROR;
	}
	m_registered = true;
	start_pushers();
	return SLS_OK;
}

// The stream's push relays, if its app has a push relay block. A failure is
// logged; the publisher is admitted all the same.
void CSLSPublisher::start_pushers()
{
	if (NULL == m_admit.map_pusher) {
		return;
	}
	bool created = false;
	CSLSRelayManager *mgr = m_admit.map_pusher->add_relay_manager(m_admit_app.c_str(), m_admit_stream.c_str(), &created);
	if (NULL == mgr) {
		sls_log(SLS_LOG_DEBUG, "[%p]CSLSPublisher::start_pushers, no push relay manager, stream=%s.",
				this, m_map_data_key);
		return;
	}
	mgr->configure(m_map_data, m_map_publisher, m_admit.role_list, m_admit.port);
	if (SLS_OK != mgr->start()) {
		sls_log(SLS_LOG_INFO, "[%p]CSLSPublisher::start_pushers, pusher_manager->start failed, publisher[%s:%d], stream=%s.",
				this, m_stat_remote_ip.c_str(), m_stat_remote_port, m_map_data_key);
		// one this publisher created and could not start has no pusher and
		// no reconnect entry: nothing else would ever retire it (S3)
		if (created) {
			mgr->start_failed(true, "first push failed");
		}
	}
	mgr->release();   // add_relay_manager's reference
}

void CSLSPublisher::set_map_publisher(CSLSMapPublisher * publisher)
{
	m_map_publisher = publisher;
}

int CSLSPublisher::handler()
{
    return handler_read_data();
}



