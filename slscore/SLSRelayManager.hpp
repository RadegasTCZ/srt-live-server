
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


#ifndef _SLSRelayManager_INCLUDE_
#define _SLSRelayManager_INCLUDE_

#include <vector>
#include <string>
#include <atomic>

#include "SLSRelay.hpp"
#include "SLSMapPublisher.hpp"
#include "conf.hpp"
#include "SLSRoleList.hpp"

struct SLS_RELAY_INFO {
    std::vector<std::string>      m_upstreams;
    char     m_type[32];
    int      m_mode;
    int      m_reconnect_interval;//unit: s
    int      m_idle_streams_timeout;//unit: s
    int      m_max_pull_streams;   // pull: concurrent managers per relay block
};

class CSLSMapRelay;

/**
 * CSLSRelayManager
 */
/**
 * Ownership: a manager is reference counted and deleted by whoever drops the
 * last reference (release()). References are held by
 *   - its CSLSMapRelay entry (taken at creation, dropped when the entry is
 *     removed: remove_relay_manager, clear),
 *   - each relay role it created (set_relay_manager .. CSLSRelay::uninit or
 *     the relay's destructor),
 *   - each worker reconnect-list entry (CSLSGroup),
 *   - the caller of CSLSMapRelay::add_relay_manager, until it calls release().
 * start(), reconnect(), on_relay_closed() and configure() serialize on
 * m_mutex. A retired manager is out of the map and never connects again;
 * only a reference holder may retire it (so release() in the map cannot
 * free it under the caller).
 */
class CSLSRelayManager
{
public :
	CSLSRelayManager();
    virtual ~CSLSRelayManager();

    void add_ref();
    void release();

    int  start();
    // SLS_OK: the caller drops its reconnect-list entry
    int  reconnect(int64_t cur_tm_ms);
    // One of this manager's relay roles ended (its worker, before uninit).
    // true: keep reconnecting, the caller puts the manager on its reconnect
    // list (with its own reference).
    bool on_relay_closed(CSLSRelay *relay);
    // stop for good unless a relay of this manager is still running
    void retire_if_idle(const char *why);

    virtual int  add_reconnect_stream(char* relay_url) = 0;

    void configure(CSLSMapData *map_data, CSLSMapPublisher *map_publisher,
                   CSLSRoleList *role_list, int listen_port);

    void set_relay_conf(SLS_RELAY_INFO *sri);
    SLS_RELAY_INFO *get_relay_conf() { return m_sri; }
    void set_relay_info(const char *app_uplive, const char *stream_name);
    void set_owner(CSLSMapRelay *owner);

    // relay roles of this manager currently alive (CSLSRelay bookkeeping)
    void relay_attached();
    void relay_detached();

protected:
    virtual int  do_start() = 0;
    virtual int  do_reconnect(int64_t cur_tm_ms) = 0;
    // under m_mutex; default (push): reconnect that relay's url
    virtual bool do_on_relay_closed(CSLSRelay *relay);
    // under m_mutex: mark retired and leave the owner map
    void retire_locked(const char *why);

    CSLSMutex            m_mutex;
    bool                 m_retired;
    std::atomic<int>     m_refs;
    std::atomic<int>     m_live_relays;
    CSLSMapRelay        *m_owner;

    CSLSMapPublisher    *m_map_publisher ;
    CSLSMapData         *m_map_data ;
    CSLSRoleList        *m_role_list;
    SLS_RELAY_INFO      *m_sri;
    int64_t              m_reconnect_begin_tm;//unit: ms
    int                  m_listen_port;

    char     m_app_uplive[1024];
    char     m_stream_name[1024];

    int connect(const char *url);
    int connect_hash();

    virtual CSLSRelay *create_relay() = 0;
    std::string get_hash_url();

    virtual int set_relay_param(CSLSRelay *relay) = 0;

};


#endif
