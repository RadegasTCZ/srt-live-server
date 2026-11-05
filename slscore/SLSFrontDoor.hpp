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


#ifndef _SLSFrontDoor_INCLUDE_
#define _SLSFrontDoor_INCLUDE_

#include <atomic>
#include <vector>
#include <stdint.h>
#include <poll.h>

#include "SLSThread.hpp"
#include "SLSPeerMap.hpp"

#define SLS_FD_RECV_BURST      256      // datagrams read from one socket per wakeup
#define SLS_FD_POLL_MS         100
#define SLS_FD_EXPIRE_EVERY_MS 1000
#define SLS_FD_SUMMARY_MS      10000    // DEBUG summary interval (only when something moved)
#define SLS_FD_SOCKBUF         (4 * 1024 * 1024)

/**
 * The front door of one srtla-enabled server block (stage 1: plain SRT).
 *
 * It owns the block's public UDP port. Each plain SRT peer gets its own UDP
 * socket connect()ed to the block's loopback libsrt listener, so libsrt sees
 * one distinct peer per remote address; the socket's local port is how the
 * listener maps a connection back to the real remote (peers()). Packets are
 * copied both ways. A mapping is created only for an SRT handshake; it is
 * half-open until libsrt answers it with more than a handshake (the peer can
 * receive), and at a cap a half-open or long-silent mapping makes room (see
 * CSLSPeerMap). SRTLA packets are recognised and dropped (stage 2), and
 * anything else from an unknown source is dropped. Nothing is logged per
 * packet: there are counters, a DEBUG summary and an INFO line at stop.
 *
 * Threads: work() runs on this object's thread and is the only one that
 * creates, reads or closes the sockets. libsrt's thread reads peers() (the
 * map has its own lock). Lifetime: the listener owns it; the manager stops
 * (joins) it first in its stop(), before any listener is torn down. So the
 * shutdowns libsrt sends while sls stops are not relayed: peers notice the
 * stop by their idle timeout (about 5 s), as after a network loss.
 * Timeouts are checked once a second, so each one may run up to 1 s longer.
 */
class CSLSFrontDoor : public CSLSThread
{
public:
    CSLSFrontDoor(int public_port, int internal_port, int max_total, int max_per_ip);
    virtual ~CSLSFrontDoor();

    // Binds 0.0.0.0:public_port (non-blocking). SLS_ERROR if it cannot (port taken).
    int  bind_public();
    virtual int work();

    CSLSPeerMap &peers() { return m_peers; }
    // work() ended on an error: the port is no longer served (the manager stops sls)
    bool failed() const { return m_failed; }
    int  public_port() const { return m_public_port; }
    int  internal_port() const { return m_internal_port; }

    // counters (read by the stop line and the summary)
    std::atomic<uint64_t> m_relayed_in;      // public -> internal
    std::atomic<uint64_t> m_relayed_out;     // internal -> public
    std::atomic<uint64_t> m_dropped_srtla;   // SRTLA from an unknown source (stage 1)
    std::atomic<uint64_t> m_dropped_unknown; // not a handshake, from an unknown source
    std::atomic<uint64_t> m_refused_cap;     // a handshake over a cap
    std::atomic<uint64_t> m_send_errors;     // a relay send that failed (dropped)
    std::atomic<uint64_t> m_local_errors;    // no relay socket for a new peer (fd limit, ...)
    std::atomic<uint64_t> m_mappings_created;
    std::atomic<uint64_t> m_mappings_expired;
    std::atomic<uint64_t> m_mappings_evicted;  // half-open or long-silent, to make room at a cap

private:
    std::atomic<bool> m_failed;
    int  m_public_port;
    int  m_internal_port;
    int  m_public_fd;
    CSLSPeerMap m_peers;
    std::vector<struct pollfd> m_pollfds;   // [0] = the public socket
    bool m_pollfds_dirty;
    int64_t m_last_expire_ms;
    int64_t m_last_summary_ms;
    int64_t m_last_cap_log_ms;
    int64_t m_last_local_error_log_ms;
    uint64_t m_summary_seen;   // the sum of the moving counters at the last summary
    int  m_rcvbuf;             // the public socket's buffers as the kernel granted them
    int  m_sndbuf;

    void on_public_readable(int64_t now_ms);
    void on_mapping_readable(int fd, int64_t now_ms);
    int  open_mapping(const struct sockaddr_in &remote, int64_t now_ms);
    void local_error(const char *what, int err, int64_t now_ms);
    void expire(int64_t now_ms);
    void rebuild_pollfds();
    void log_summary(int64_t now_ms, bool force);
    void close_all();
};

#endif
