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
#include <deque>
#include <map>
#include <utility>
#include <vector>
#include <stdint.h>
#include <poll.h>

#include "SLSThread.hpp"
#include "SLSPeerMap.hpp"
#include "SLSSrtlaGroups.hpp"
#include "SLSSrtlaReorder.hpp"

#define SLS_FD_RECV_BURST      256      // datagrams read from one socket per wakeup
#define SLS_FD_POLL_MS         100
#define SLS_FD_EXPIRE_EVERY_MS 1000
#define SLS_FD_SUMMARY_MS      10000    // DEBUG summary interval (only when something moved)
#define SLS_FD_SOCKBUF         (4 * 1024 * 1024)
// An evicted mapping's socket stays open (unpolled) this long before it is
// closed, so the kernel cannot give its port to a new peer while libsrt may
// still hold a connection from it (libsrt drops a silent peer after 5 s).
#define SLS_FD_QUARANTINE_MS   6000
// sls stop: how long the front doors keep relaying after every libsrt socket
// closed, so its shutdowns reach the peers
#define SLS_FD_STOP_DRAIN_MS   200
// SRTLA: SRT ACKs go to every link that delivered data this recently
#define SLS_SRTLA_ACK_FANOUT_MS 2000
#define SLS_SRTLA_REORDER_DEFAULT_MS 500
// SRTLA log lines (group/link events): at most this many per window, then counted
#define SLS_SRTLA_LOG_BURST     20
#define SLS_SRTLA_LOG_WINDOW_MS 10000

/**
 * The front door of one srtla-enabled server block (stage 1: plain SRT).
 *
 * It owns the block's public UDP port. Each plain SRT peer gets its own UDP
 * socket connect()ed to the block's loopback libsrt listener, so libsrt sees
 * one distinct peer per remote address; the socket's local port is how the
 * listener maps a connection back to the real remote (peers()). Packets are
 * copied both ways. A mapping is created only for an SRT handshake; it is
 * half-open until libsrt answers it with more than a handshake (the peer can
 * receive), and at a cap an ended, a random half-open or a long-silent
 * mapping makes room (see CSLSPeerMap). SRTLA packets are recognised and dropped (stage 2), and
 * anything else from an unknown source is dropped. Nothing is logged per
 * packet: there are counters, a DEBUG summary and an INFO line at stop.
 *
 * Threads: work() runs on this object's thread and is the only one that
 * creates, reads or closes the sockets. libsrt's thread reads peers() (the
 * map has its own lock). Lifetime: the listener owns it until sls stops;
 * then the manager takes it over and stops it last, SLS_FD_STOP_DRAIN_MS
 * after every libsrt socket closed, so the shutdowns libsrt sends at stop
 * still reach the peers (they disconnect at once).
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
    // SRTLA settings (before start): caps, srtla_reorder_max, and the
    // listener's latency (the hold until a session's latency is known)
    void set_srtla(const sls_srtla_caps &caps, int reorder_max_ms, int latency_ms);
    // asked before a REG1/REG2 is answered (on this thread): true refuses it
    // (the listener: wrong-passphrase lockout, handshake rate)
    typedef bool (*srtla_ip_check_fn)(void *ctx, uint32_t ip_be);
    void set_srtla_ip_check(srtla_ip_check_fn fn, void *ctx) { m_ip_check = fn; m_ip_check_ctx = ctx; }
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
    std::atomic<uint64_t> m_mappings_evicted;  // ended, half-open or long-silent, to make room at a cap
    std::atomic<uint64_t> m_srtla_reg_ok;      // REG1/REG2 answered REG2/REG3
    std::atomic<uint64_t> m_srtla_reg_refused; // answered REG_ERR or REG_NGP
    std::atomic<uint64_t> m_srtla_in;          // packets from registered links
    std::atomic<uint64_t> m_srtla_acks;        // SRTLA ACKs sent
    std::atomic<uint64_t> m_srtla_no_session;  // SRT data/control from a group without an SRT session

private:
    std::atomic<bool> m_failed;
    int  m_public_port;
    int  m_internal_port;
    int  m_public_fd;
    CSLSPeerMap m_peers;
    std::vector<struct pollfd> m_pollfds;   // [0] = the public socket
    // evicted sockets waiting to be closed: (closable at ms, fd), oldest first;
    // at most max_total of them (past that the oldest is closed early)
    std::deque<std::pair<int64_t, int> > m_quarantine;
    void quarantine(int fd, int64_t now_ms);
    void release_quarantine(int64_t now_ms, bool all);
    bool m_pollfds_dirty;
    int64_t m_last_expire_ms;
    int64_t m_last_summary_ms;
    int64_t m_last_cap_log_ms;
    int64_t m_last_local_error_log_ms;
    uint64_t m_summary_seen;   // the sum of the moving counters at the last summary
    int  m_rcvbuf;             // the public socket's buffers as the kernel granted them
    int  m_sndbuf;

    // SRTLA
    struct group_rt {
        CSLSFrontDoor    *door;
        uint32_t          handle;
        int               fd;       // the group's SRT session socket, or -1
        CSLSSrtlaReorder *buf;
        bool              logged;   // "group registered" was logged
        uint32_t          peer_sock;  // the caller's SRT socket id of the current session (0: none)
    };
    srtla_ip_check_fn m_ip_check;
    void             *m_ip_check_ctx;
    CSLSSrtlaGroups *m_groups;
    sls_srtla_caps   m_srtla_caps;
    int  m_reorder_max_ms;
    int  m_latency_ms;
    std::map<uint32_t, group_rt *> m_group_rt;
    std::map<int, uint32_t>        m_group_by_fd;
    int64_t  m_last_srtla_cleanup_ms;
    int64_t  m_srtla_log_window_ms;
    int      m_srtla_log_n;
    uint64_t m_srtla_log_held;
    static void emit_to_session(void *ctx, const uint8_t *pkt, int len);
    int  initial_hold() const;
    void on_srtla_reg(int kind, const struct sockaddr_in &from, const uint8_t *buf, int len, int64_t now_ms);
    void on_link_packet(uint32_t handle, const struct sockaddr_in &from, const uint8_t *buf, int len, int64_t now_ms);
    void on_group_readable(int fd, uint32_t handle, const uint8_t *buf, int len, int64_t now_ms);
    group_rt *rt_of(uint32_t handle);
    int  open_group_session(group_rt *rt, int64_t now_ms);
    void end_group_session(group_rt *rt, int64_t now_ms);
    void forget_group_fd(int fd);
    void srtla_events(int64_t now_ms);
    void srtla_publish_stats(int64_t now_ms, bool summary);
    int64_t m_last_srtla_summary_ms;
    void srtla_tick(int64_t now_ms);
    int  srtla_poll_timeout(int64_t now_ms) const;
    void reply(const struct sockaddr_in &to, const uint8_t *buf, int len);
    bool srtla_log_ok(int64_t now_ms);

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
