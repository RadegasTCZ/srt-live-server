
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
#include <map>
#include <mutex>
#include <netinet/in.h>

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
    std::atomic<int> m_live_relays; // relay roles alive for this block (all its managers)

    SLS_RELAY_INFO() : m_mode(0), m_reconnect_interval(0), m_idle_streams_timeout(0),
                       m_max_pull_streams(0), m_live_relays(0), m_stale(false) { m_type[0] = 0; }

    // Upstream addresses are resolved here, once, when the relay is configured,
    // and cached per "host:port"; relay opens only read the cache (no DNS on
    // the connect path). A relay that ended marks the cache stale and the next
    // connect attempt resolves again, so a moved upstream is picked up after a
    // failure and never per packet.
    int  resolve_upstreams();     // SLS_ERROR: an upstream is malformed or cannot be resolved
    int  lookup(const char *host, int port, struct sockaddr_in *out);
    void mark_stale() { m_stale = true; }
    void refresh_if_stale();

private:
    std::mutex                                 m_addr_mutex;
    std::map<std::string, struct sockaddr_in>  m_addrs;
    std::atomic<bool>                          m_stale;
    int  resolve_all(std::map<std::string, struct sockaddr_in> *out);
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
 * Retiring removes only the map's reference. A retired manager is out of the
 * map and never connects again; only a reference holder retires it, so the
 * map's release() cannot free it under the caller.
 *
 * State (pull): outside the creator's start() call, every manager is in
 * exactly one of
 *   LIVE      m_live_relays > 0: it has a running puller (in the map, counted
 *             against max_pull_streams),
 *   WAITING   m_live_relays == 0, not retired, a worker reconnect-list entry
 *             will call reconnect(), which ends in LIVE, RETIRED, or WAITING
 *             again (attempt failed, players still attached),
 *   RETIRED   out of the map, no relay, never connects again.
 * Every transition happens under m_mutex: start()/do_start (-> LIVE),
 * relay_ended() (LIVE -> WAITING/RETIRED, the one place a relay stops
 * counting), reconnect() and retire_if_idle(). m_live_relays is a plain int
 * guarded by m_mutex. A retire is refused while a relay is live.
 *
 * Lock order: manager m_mutex -> CSLSMapRelay::m_rwclock (retire removes the
 * map entry), and manager m_mutex -> publisher map / map data / role list
 * locks. CSLSMapRelay never takes a manager's mutex while holding its own
 * lock (add_relay_manager/remove_relay_manager/clear only touch the atomic
 * refcount, and release() outside or after the last holder), and none of the
 * other locks call back into a manager.
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
    // One of this manager's relay roles ended. The relay stops counting as
    // live here and nowhere else. may_reconnect: the caller (its worker,
    // before uninit) can hold a reconnect-list entry; true is returned when
    // it must put the manager on its list (with its own reference). Without
    // one (relay torn down outside a worker) a pull manager left with no
    // live relay retires on the spot.
    bool relay_ended(CSLSRelay *relay, bool may_reconnect);
    // stop for good unless a relay of this manager is still running
    void retire_if_idle(const char *why);
    bool is_retired();

    virtual int  add_reconnect_stream(char* relay_url) = 0;

    void configure(CSLSMapData *map_data, CSLSMapPublisher *map_publisher,
                   CSLSRoleList *role_list, int listen_port);

    void set_relay_conf(SLS_RELAY_INFO *sri);
    SLS_RELAY_INFO *get_relay_conf() { return m_sri; }
    void set_relay_info(const char *app_uplive, const char *stream_name);
    void set_owner(CSLSMapRelay *owner);

protected:
    // a relay of this manager starts (from set_relay_param, under m_mutex)
    void relay_attached_locked(CSLSRelay *relay);
    // pull managers exist only for running pulls; push managers persist
    virtual bool retires_when_idle() { return false; }

    virtual int  do_start() = 0;
    virtual int  do_reconnect(int64_t cur_tm_ms) = 0;
    // under m_mutex; default (push): reconnect that relay's url
    virtual bool do_on_relay_closed(CSLSRelay *relay);
    // Under m_mutex: mark retired and leave the owner map, but only while no
    // relay of this manager is alive (m_live_relays == 0); false otherwise.
    // So a retired manager never has a live relay, and every live relay
    // belongs to a manager that is still in the map and counts against
    // max_pull_streams. start() refuses on a retired manager.
    bool retire_locked(const char *why);

    CSLSMutex            m_mutex;
    bool                 m_retired;
    std::atomic<int>     m_refs;
    int                  m_live_relays;      // guarded by m_mutex
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
