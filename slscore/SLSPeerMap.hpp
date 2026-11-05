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


#ifndef _SLSPeerMap_INCLUDE_
#define _SLSPeerMap_INCLUDE_

#include <map>
#include <vector>
#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

#include "SLSLock.hpp"

#define SLS_FD_HALFOPEN_MS   5000    // created, libsrt never answered past the handshake
#define SLS_FD_IDLE_MS       10000   // established, nothing from the remote (libsrt drops a peer after 5 s)
#define SLS_FD_CLOSING_MS    2000    // after an SRT shutdown: the session is over
#define SLS_FD_QUIET_MS      5000    // established but silent this long: may be evicted at a cap

/**
 * The front door's map of plain SRT peers: a remote address <-> the internal
 * UDP socket (fd) that relays it to the loopback listener <-> that socket's
 * local port (which is what libsrt reports as the peer).
 *
 * Thread-safe with one mutex, never held across a system call: the front
 * door thread owns the fds and does all socket work outside the map;
 * libsrt's thread only reads remote_for_local_port(). Times are monotonic ms
 * passed in by the caller.
 *
 * Spoofing: a remote's source address proves nothing, so an entry is
 * half-open until libsrt sends it a non-handshake packet (it only does so
 * after the handshake cookie came back, i.e. the remote can receive). At a
 * cap, add() evicts, in this order, an entry whose session libsrt ended,
 * the oldest half-open one, or one established but silent for
 * SLS_FD_QUIET_MS, of that scope instead of refusing (a client retrying in
 * a loop leaves ended sessions that must not hold its slots), so a
 * handshake flood cannot keep a real peer out. A shutdown from the remote
 * side is undone by any later packet from it (a forged one cannot end a
 * live session); one from libsrt's side is final. Idle time counts packets
 * from the remote only: libsrt's own retries to a vanished peer keep nothing.
 */
class CSLSPeerMap
{
public:
    CSLSPeerMap(int max_total, int max_per_ip);   // max_per_ip 0 = unlimited
    ~CSLSPeerMap();

    // SLS_ERROR when the remote, fd or local port is already mapped, or a cap is
    // reached and nothing can be evicted. An evicted entry's fd goes to
    // *evicted_fd (else -1); the caller closes it.
    int  add(const struct sockaddr_in &remote, int fd, uint16_t local_port, int64_t now_ms,
             int *evicted_fd = NULL);
    // the fd for a packet from remote, or -1; shutdown: the packet is an SRT shutdown
    int  fd_for_remote(const struct sockaddr_in &remote, int64_t now_ms, bool shutdown = false);
    // the remote a packet from libsrt read on fd goes to. confirms: libsrt sent a
    // non-handshake packet (the entry is established); shutdown: an SRT shutdown
    bool remote_for_fd(int fd, struct sockaddr_in *remote, int64_t now_ms,
                       bool confirms = false, bool shutdown = false);
    // identity: the real peer behind an internal socket's local port
    bool remote_for_local_port(uint16_t local_port, struct sockaddr_in *remote) const;
    // removes expired entries and appends their fds (the caller closes them)
    void expire(int64_t now_ms, int halfopen_ms, int idle_ms, std::vector<int> *expired_fds);
    void remove_fd(int fd);
    void all_fds(std::vector<int> *fds) const;

    size_t size() const;
    int    count_for_ip(uint32_t ip_be) const;
    int    max_total() const { return m_max_total; }
    int    max_per_ip() const { return m_max_per_ip; }
    uint64_t evictions() const;

private:
    struct entry {
        struct sockaddr_in remote;
        int      fd;
        uint16_t local_port;
        int64_t  created_ms;
        int64_t  last_in_ms;
        bool     established;
        int64_t  closing_ms;        // 0, or when libsrt sent a shutdown (final)
        int64_t  peer_shutdown_ms;  // 0, or when the remote sent one (undone by later packets)
    };
    typedef std::map<uint64_t, entry> by_remote_t;
    static uint64_t key_of(const struct sockaddr_in &remote);
    void erase_locked(by_remote_t::iterator it);
    // the entry to evict among those of ip (or all, when ip_only is false), or end()
    by_remote_t::iterator victim_locked(bool ip_only, uint32_t ip, int64_t now_ms);

    int  m_max_total;
    int  m_max_per_ip;
    uint64_t m_evictions;
    mutable CSLSMutex m_mutex;
    by_remote_t                  m_by_remote;
    std::map<int, uint64_t>      m_by_fd;
    std::map<uint16_t, uint64_t> m_by_local_port;
    std::map<uint32_t, int>      m_per_ip;
};

#endif
