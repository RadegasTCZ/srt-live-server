
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


#include "SLSPlayer.hpp"
#include "SLSLog.hpp"
#include "SLSMapPublisher.hpp"
#include "SLSMapRelay.hpp"

/**
 * CSLSPlayer class implementation
 */

CSLSPlayer::CSLSPlayer()
{
    m_is_write = 1;
    m_reader_attached = false;

    sprintf(m_role_name, "player");
}

CSLSPlayer::~CSLSPlayer()
{
    detach_reader();
}

int CSLSPlayer::uninit()
{
    detach_reader();
    return CSLSRole::uninit();
}

void CSLSPlayer::attach_reader()
{
    if (!m_reader_attached && NULL != m_map_data && strlen(m_map_data_key) > 0) {
        m_map_data->add_reader(m_map_data_key);
        m_reader_attached = true;
    }
}

void CSLSPlayer::detach_reader()
{
    if (m_reader_attached) {
        m_reader_attached = false;
        m_map_data->remove_reader(m_map_data_key);
    }
}



int CSLSPlayer::handler()
{
    return handler_write_data() ;
}

// Never log what a publisher found here points to: another thread can tear
// it down as soon as the map's lock is dropped; only its NULL-ness counts.
int CSLSPlayer::admit()
{
    if (NULL == m_map_data || NULL == m_admit.map_publisher) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSPlayer::admit, no map, stream=%s.", this, m_map_data_key);
        return SLS_ERROR;
    }
    if (NULL == m_admit.map_publisher->get_publisher(m_map_data_key)) {
        if (SLS_OK != start_pull()) {
            return SLS_ERROR;   // logged
        }
    }
    if (!m_map_data->is_exist(m_map_data_key)) {
        sls_peer_log(SLS_LOG_ERROR, "[%p]CSLSPlayer::admit, refused, player[%s:%d], stream=%s, but publisher data doesn't exist in m_map_data.",
                     this, m_stat_remote_ip.c_str(), m_stat_remote_port, m_map_data_key);
        return SLS_ERROR;
    }
    attach_reader();
    return SLS_OK;
}

// The stream has no publisher: pull it from an upstream, if the app has a
// pull relay. On success the puller is the stream's publisher.
int CSLSPlayer::start_pull()
{
    if (NULL == m_admit.map_puller) {
        sls_peer_log(SLS_LOG_INFO, "[%p]CSLSPlayer::start_pull, refused, player[%s:%d], stream='%s', publisher is NULL and m_map_puller is NULL.",
                     this, m_stat_remote_ip.c_str(), m_stat_remote_port, m_map_data_key);
        return SLS_ERROR;
    }
    bool created = false;
    CSLSRelayManager *mgr = m_admit.map_puller->add_relay_manager(m_admit_app.c_str(), m_admit_stream.c_str(), &created);
    if (NULL == mgr) {
        sls_peer_log(SLS_LOG_INFO, "[%p]CSLSPlayer::start_pull, m_map_puller->add_relay_manager failed, player[%s:%d], stream='%s', publisher is NULL, no puller_manager.",
                     this, m_stat_remote_ip.c_str(), m_stat_remote_port, m_map_data_key);
        return SLS_ERROR;
    }
    mgr->configure(m_map_data, m_admit.map_publisher, m_admit.role_list, m_admit.port);
    int ret = mgr->start();
    if (SLS_OK != ret) {
        sls_peer_log(SLS_LOG_INFO, "[%p]CSLSPlayer::start_pull, puller_manager->start failed, player[%s:%d], stream='%s'.",
                     this, m_stat_remote_ip.c_str(), m_stat_remote_port, m_map_data_key);
        // One this request created has no puller to keep it: it must not
        // keep counting against max_pull_streams. An existing one may be
        // waiting to reconnect for players already attached: it stays.
        mgr->start_failed(created, "first pull failed");
    } else {
        sls_log(SLS_LOG_DEBUG, "[%p]CSLSPlayer::start_pull, puller_manager->start ok, player[%s:%d], stream=%s.",
                this, m_stat_remote_ip.c_str(), m_stat_remote_port, m_map_data_key);
        if (NULL == m_admit.map_publisher->get_publisher(m_map_data_key)) {
            sls_peer_log(SLS_LOG_INFO, "[%p]CSLSPlayer::start_pull, m_map_publisher->get_publisher failed, player[%s:%d], stream=%s.",
                         this, m_stat_remote_ip.c_str(), m_stat_remote_port, m_map_data_key);
            ret = SLS_ERROR;
        }
    }
    mgr->release();   // add_relay_manager's reference
    return ret;
}




