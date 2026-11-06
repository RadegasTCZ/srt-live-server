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
#include <unistd.h>
#include <fcntl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "SLSFrontDoor.hpp"
#include "SLSFrontDoorProto.hpp"
#include "SLSSrtlaProto.hpp"
#include "SLSLog.hpp"
#include "common.hpp"

// group ids must be unguessable: getrandom(), else /dev/urandom; never a weak fallback
static void sls_fd_random(uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = getrandom(buf + got, len - got, 0);
        if (n > 0) {
            got += (size_t)n;
        } else if (n < 0 && errno != EINTR) {
            break;
        }
    }
    if (got < len) {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        while (fd >= 0 && got < len) {
            ssize_t n = read(fd, buf + got, len - got);
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
        }
        if (fd >= 0) {
            close(fd);
        }
    }
    if (got < len) {
        sls_log(SLS_LOG_FATAL, "CSLSFrontDoor, no source of random bytes (getrandom, /dev/urandom); exiting.");
        abort();
    }
}

static bool sls_fd_is_shutdown(const uint8_t *p, int len)
{
    return len >= SLS_SRT_HDR_LEN && p[0] == (SLS_SRT_TYPE_SHUTDOWN >> 8) && p[1] == (SLS_SRT_TYPE_SHUTDOWN & 0xFF);
}

CSLSFrontDoor::CSLSFrontDoor(int public_port, int internal_port, int max_total, int max_per_ip)
    : m_relayed_in(0), m_relayed_out(0), m_dropped_srtla(0), m_dropped_unknown(0),
      m_refused_cap(0), m_send_errors(0), m_local_errors(0), m_mappings_created(0), m_mappings_expired(0),
      m_mappings_evicted(0), m_srtla_reg_ok(0), m_srtla_reg_refused(0), m_srtla_in(0), m_srtla_acks(0),
      m_srtla_no_session(0),
      m_failed(false), m_public_port(public_port), m_internal_port(internal_port), m_public_fd(-1),
      m_peers(max_total, max_per_ip), m_pollfds_dirty(true),
      m_last_expire_ms(0), m_last_summary_ms(0), m_last_cap_log_ms(0), m_last_local_error_log_ms(-100000),
      m_summary_seen(0), m_rcvbuf(0), m_sndbuf(0),
      m_ip_check(NULL), m_ip_check_ctx(NULL),
      m_groups(NULL), m_reorder_max_ms(SLS_SRTLA_REORDER_DEFAULT_MS), m_latency_ms(120),
      m_last_srtla_cleanup_ms(0), m_srtla_log_window_ms(0), m_srtla_log_n(0), m_srtla_log_held(0)
{
    sls_srtla_caps caps = {200, 4, 8, 5};
    set_srtla(caps, SLS_SRTLA_REORDER_DEFAULT_MS, 120);
}

void CSLSFrontDoor::set_srtla(const sls_srtla_caps &caps, int reorder_max_ms, int latency_ms)
{
    m_srtla_caps     = caps;
    m_reorder_max_ms = reorder_max_ms < 0 ? 0 : reorder_max_ms;
    m_latency_ms     = latency_ms < 0 ? 0 : latency_ms;
    delete m_groups;
    m_groups = new CSLSSrtlaGroups(caps, sls_fd_random);
}

CSLSFrontDoor::~CSLSFrontDoor()
{
    stop();        // joins the thread if it runs (a no-op otherwise)
    close_all();   // a front door that never started still owns its public socket
    delete m_groups;
}

int CSLSFrontDoor::bind_public()
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSFrontDoor::bind_public, socket failed: %s.", this, strerror(errno));
        return SLS_ERROR;
    }
    int buf = SLS_FD_SOCKBUF;   // best effort: the kernel caps it at net.core.*mem_max
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    int rcv = 0, snd = 0;
    socklen_t ol = sizeof(rcv);
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, &ol);
    ol = sizeof(snd);
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, &ol);
    m_rcvbuf = rcv;
    m_sndbuf = snd;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(m_public_port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSFrontDoor::bind_public, cannot bind UDP port %d: %s.",
                this, m_public_port, strerror(errno));
        close(fd);
        return SLS_ERROR;
    }
    m_public_fd = fd;
    m_pollfds_dirty = true;
    return SLS_OK;
}

void CSLSFrontDoor::rebuild_pollfds()
{
    std::vector<int> fds;
    m_peers.all_fds(&fds);
    m_pollfds.clear();
    m_pollfds.reserve(fds.size() + 1);
    struct pollfd p;
    p.fd = m_public_fd;
    p.events = POLLIN;
    p.revents = 0;
    m_pollfds.push_back(p);
    for (size_t i = 0; i < fds.size(); i++) {
        p.fd = fds[i];
        m_pollfds.push_back(p);
    }
    m_pollfds_dirty = false;
}

void CSLSFrontDoor::local_error(const char *what, int err, int64_t now_ms)
{
    // not the peer's doing (fd limit, memory): ERROR, at most one line per 10 s
    m_local_errors++;
    if (now_ms - m_last_local_error_log_ms >= 10000) {
        m_last_local_error_log_ms = now_ms;
        sls_log(SLS_LOG_ERROR, "[%p]CSLSFrontDoor, port %d: cannot open a relay socket (%s: %s); "
                "new peers are refused (%llu time(s) so far).",
                this, m_public_port, what, strerror(err), (unsigned long long)m_local_errors);
    }
}

int CSLSFrontDoor::open_mapping(const struct sockaddr_in &remote, int64_t now_ms)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        local_error("socket", errno, now_ms);
        return -1;
    }
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    in.sin_port = htons(m_internal_port);
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    memset(&local, 0, sizeof(local));
    if (connect(fd, (struct sockaddr *)&in, sizeof(in)) < 0
            || getsockname(fd, (struct sockaddr *)&local, &local_len) < 0) {
        local_error("connect", errno, now_ms);
        close(fd);
        return -1;
    }
    // at a cap the map evicts an ended, a random half-open or a long-silent
    // entry rather than refusing (see CSLSPeerMap)
    int evicted = -1;
    if (SLS_OK != m_peers.add(remote, fd, ntohs(local.sin_port), now_ms, &evicted)) {
        close(fd);
        m_refused_cap++;
        if (now_ms - m_last_cap_log_ms >= 1000) {
            m_last_cap_log_ms = now_ms;
            char ip_str[INET_ADDRSTRLEN] = "";
            inet_ntop(AF_INET, &remote.sin_addr, ip_str, sizeof(ip_str));
            sls_peer_log(SLS_LOG_INFO, "[%p]CSLSFrontDoor, refused [%s:%d]: mapping cap reached (%d in total, %d per IP), "
                         "all of them active.",
                         this, ip_str, ntohs(remote.sin_port), m_peers.max_total(), m_peers.max_per_ip());
        }
        return -1;
    }
    if (evicted >= 0) {
        forget_group_fd(evicted);
        quarantine(evicted, now_ms);
        m_mappings_evicted++;
    }
    m_mappings_created++;
    m_pollfds_dirty = true;
    return fd;
}

void CSLSFrontDoor::on_public_readable(int64_t now_ms)
{
    uint8_t buf[SLS_UDP_MTU + 64];
    for (int n = 0; n < SLS_FD_RECV_BURST; n++) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t len = recvfrom(m_public_fd, buf, sizeof(buf), MSG_TRUNC, (struct sockaddr *)&from, &from_len);
        if (len < 0) {
            return;   // EAGAIN: drained (or a transient error: poll says when to retry)
        }
        if (len > (ssize_t)sizeof(buf) || from.sin_family != AF_INET) {
            m_dropped_unknown++;   // larger than any SRT packet: truncated
            continue;
        }
        // a registered SRTLA link: everything it sends goes to the SRTLA engine
        uint32_t handle = m_groups->on_packet(from, (int)len, sls_srt_is_data(buf, (int)len), now_ms);
        if (handle) {
            m_srtla_in++;
            on_link_packet(handle, from, buf, (int)len, now_ms);
            continue;
        }
        sls_fd_kind_t kind = sls_fd_classify(buf, (int)len);
        if (SLS_FD_SRTLA_REG1 == kind || SLS_FD_SRTLA_REG2 == kind) {
            on_srtla_reg(kind, from, buf, (int)len, now_ms);
            continue;
        }
        // the registering address of a group, but not (or no longer) one of its
        // links: its packets only reach the session through a link, and must not
        // touch the session's entry (keep it alive, undo its shutdown)
        int peek = m_peers.peek_remote(from, NULL);
        if (peek >= 0 && m_group_by_fd.count(peek)) {
            m_dropped_unknown++;
            continue;
        }
        bool shutdown = sls_fd_is_shutdown(buf, (int)len);
        int fd = m_peers.fd_for_remote(from, now_ms, shutdown, SLS_FD_SRT_HANDSHAKE == kind);
        if (fd < 0) {
            if (SLS_FD_SRT_HANDSHAKE == kind) {
                fd = open_mapping(from, now_ms);
                if (fd < 0) {
                    continue;
                }
            } else if (SLS_FD_SRTLA_OTHER == kind) {
                m_dropped_srtla++;   // keepalive or ACK from a source that is not a link
                continue;
            } else {
                m_dropped_unknown++;
                continue;
            }
        }
        if (send(fd, buf, len, 0) < 0) {
            m_send_errors++;
        } else {
            m_relayed_in++;
        }
    }
}

void CSLSFrontDoor::on_mapping_readable(int fd, int64_t now_ms)
{
    uint8_t buf[SLS_UDP_MTU + 64];
    for (int n = 0; n < SLS_FD_RECV_BURST; n++) {
        ssize_t len = recv(fd, buf, sizeof(buf), 0);
        if (len < 0) {
            return;   // EAGAIN, or ECONNREFUSED queued after the listener went away
        }
        // libsrt sends anything but a handshake only once the peer echoed the
        // handshake cookie: proof the remote can receive (the entry is established)
        bool confirms = len >= SLS_SRT_HDR_LEN
                        && !(buf[0] == (SLS_SRT_TYPE_HANDSHAKE >> 8) && buf[1] == (SLS_SRT_TYPE_HANDSHAKE & 0xFF));
        struct sockaddr_in remote;
        if (!m_peers.remote_for_fd(fd, &remote, now_ms, confirms, sls_fd_is_shutdown(buf, (int)len))) {
            return;   // expired meanwhile
        }
        std::map<int, uint32_t>::iterator g = m_group_by_fd.find(fd);
        if (g != m_group_by_fd.end()) {
            on_group_readable(fd, g->second, buf, (int)len, now_ms);
            continue;
        }
        if (sendto(m_public_fd, buf, len, 0, (struct sockaddr *)&remote, sizeof(remote)) < 0) {
            m_send_errors++;
        } else {
            m_relayed_out++;
        }
    }
}

void CSLSFrontDoor::quarantine(int fd, int64_t now_ms)
{
    if ((int)m_quarantine.size() >= m_peers.max_total()) {
        close(m_quarantine.front().second);   // bounded: the oldest goes early
        m_quarantine.pop_front();
    }
    m_quarantine.push_back(std::make_pair(now_ms + SLS_FD_QUARANTINE_MS, fd));
}

void CSLSFrontDoor::release_quarantine(int64_t now_ms, bool all)
{
    while (!m_quarantine.empty() && (all || m_quarantine.front().first <= now_ms)) {
        close(m_quarantine.front().second);
        m_quarantine.pop_front();
    }
}

void CSLSFrontDoor::expire(int64_t now_ms)
{
    // Expired mappings are closed at once: idle past libsrt's own peer
    // timeout, or past an SRT shutdown, or a handshake libsrt never took
    // further, so libsrt holds no connection from their ports. (Evicted ones
    // are quarantined whatever their state: an eviction does not wait for that.)
    release_quarantine(now_ms, false);
    std::vector<int> fds;
    m_peers.expire(now_ms, SLS_FD_HALFOPEN_MS, SLS_FD_IDLE_MS, &fds);
    for (size_t i = 0; i < fds.size(); i++) {
        forget_group_fd(fds[i]);
        close(fds[i]);
    }
    if (!fds.empty()) {
        m_mappings_expired += fds.size();
        m_pollfds_dirty = true;
    }
}

void CSLSFrontDoor::log_summary(int64_t now_ms, bool force)
{
    uint64_t seen = m_relayed_in + m_relayed_out + m_dropped_srtla + m_dropped_unknown + m_refused_cap;
    if (!force && seen == m_summary_seen) {
        return;
    }
    m_summary_seen = seen;
    sls_log(SLS_LOG_DEBUG, "[%p]CSLSFrontDoor, port %d: mappings=%zu relayed_in=%llu relayed_out=%llu "
            "dropped_srtla=%llu dropped_unknown=%llu refused_cap=%llu send_errors=%llu.",
            this, m_public_port, m_peers.size(),
            (unsigned long long)m_relayed_in, (unsigned long long)m_relayed_out,
            (unsigned long long)m_dropped_srtla, (unsigned long long)m_dropped_unknown,
            (unsigned long long)m_refused_cap, (unsigned long long)m_send_errors);
}

int CSLSFrontDoor::work()
{
    if (m_public_fd < 0) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSFrontDoor::work, port %d is not bound.", this, m_public_port);
        m_failed = true;
        return SLS_ERROR;
    }
    char per_ip[32];
    if (m_peers.max_per_ip() > 0) {
        snprintf(per_ip, sizeof(per_ip), "%d", m_peers.max_per_ip());
    } else {
        snprintf(per_ip, sizeof(per_ip), "unlimited");
    }
    sls_log(SLS_LOG_INFO, "[%p]CSLSFrontDoor, started: public port %d -> 127.0.0.1:%d, caps %d in total, %s per IP, "
            "socket buffers rcv=%d snd=%d%s.",
            this, m_public_port, m_internal_port, m_peers.max_total(), per_ip,
            m_rcvbuf, m_sndbuf,
            (m_rcvbuf < SLS_FD_SOCKBUF || m_sndbuf < SLS_FD_SOCKBUF)
                ? " (below the 4 MB asked for: raise net.core.rmem_max/wmem_max)" : "");
    int64_t now = sls_monotonic_ms();
    m_last_expire_ms = now;
    m_last_summary_ms = now;
    m_last_srtla_cleanup_ms = now;
    while (!m_exit) {
        if (m_pollfds_dirty) {
            rebuild_pollfds();
        }
        int ret = poll(&m_pollfds[0], m_pollfds.size(), srtla_poll_timeout(now));
        now = sls_monotonic_ms();
        if (ret < 0 && errno != EINTR) {
            sls_log(SLS_LOG_ERROR, "[%p]CSLSFrontDoor::work, poll failed: %s.", this, strerror(errno));
            m_failed = true;
            break;
        }
        if (ret > 0) {
            // handlers only mark the set dirty; it is rebuilt before the next poll
            size_t n = m_pollfds.size();
            for (size_t i = 0; i < n; i++) {
                if (0 == m_pollfds[i].revents) {
                    continue;
                }
                if (0 == i) {
                    on_public_readable(now);
                } else {
                    on_mapping_readable(m_pollfds[i].fd, now);
                }
            }
        }
        srtla_tick(now);
        if (now - m_last_srtla_cleanup_ms >= SLS_SRTLA_CLEANUP_MS) {
            m_last_srtla_cleanup_ms = now;
            m_groups->cleanup(now);
        }
        srtla_events(now);
        if (now - m_last_expire_ms >= SLS_FD_EXPIRE_EVERY_MS) {
            m_last_expire_ms = now;
            expire(now);
        }
        if (now - m_last_summary_ms >= SLS_FD_SUMMARY_MS) {
            m_last_summary_ms = now;
            log_summary(now, false);
        }
    }
    sls_log(SLS_LOG_INFO, "[%p]CSLSFrontDoor, front door stopped: port=%d mappings=%zu created=%llu expired=%llu evicted=%llu "
            "relayed_in=%llu relayed_out=%llu dropped_srtla=%llu dropped_unknown=%llu refused_cap=%llu send_errors=%llu local_errors=%llu "
            "srtla_groups=%zu srtla_reg_ok=%llu srtla_reg_refused=%llu srtla_in=%llu srtla_acks=%llu srtla_no_session=%llu.",
            this, m_public_port, m_peers.size(),
            (unsigned long long)m_mappings_created, (unsigned long long)m_mappings_expired,
            (unsigned long long)m_mappings_evicted,
            (unsigned long long)m_relayed_in, (unsigned long long)m_relayed_out,
            (unsigned long long)m_dropped_srtla, (unsigned long long)m_dropped_unknown,
            (unsigned long long)m_refused_cap, (unsigned long long)m_send_errors,
            (unsigned long long)m_local_errors,
            m_groups->group_count(), (unsigned long long)m_srtla_reg_ok, (unsigned long long)m_srtla_reg_refused,
            (unsigned long long)m_srtla_in, (unsigned long long)m_srtla_acks,
            (unsigned long long)m_srtla_no_session);
    close_all();
    return SLS_OK;
}

void CSLSFrontDoor::close_all()
{
    std::vector<int> fds;
    m_peers.all_fds(&fds);
    for (size_t i = 0; i < fds.size(); i++) {
        m_peers.remove_fd(fds[i]);
        close(fds[i]);
    }
    release_quarantine(0, true);
    m_group_by_fd.clear();
    for (std::map<uint32_t, group_rt *>::iterator it = m_group_rt.begin(); it != m_group_rt.end(); ++it) {
        delete it->second->buf;
        delete it->second;
    }
    m_group_rt.clear();
    if (m_groups) {
        m_groups->clear(0);
        std::vector<sls_srtla_event> ev;
        m_groups->take_events(&ev);   // their sockets are closed above
    }
    if (m_public_fd >= 0) {
        close(m_public_fd);
        m_public_fd = -1;
    }
    m_pollfds.clear();
    m_pollfds_dirty = true;
}

// ---------------------------------------------------------------------------
// SRTLA (srtla2): registration, links, the reorder buffer and the return path.
// Everything here runs on the front door thread.

static const char *sls_fd_ip(const struct sockaddr_in &a, char *buf, size_t len)
{
    buf[0] = 0;
    inet_ntop(AF_INET, &a.sin_addr, buf, (socklen_t)len);
    return buf;
}

bool CSLSFrontDoor::srtla_log_ok(int64_t now_ms)
{
    if (now_ms - m_srtla_log_window_ms >= SLS_SRTLA_LOG_WINDOW_MS) {
        if (m_srtla_log_held) {
            sls_log(SLS_LOG_INFO, "[%p]CSLSFrontDoor, port %d: %llu SRTLA group/link line(s) held back in the last %d s.",
                    this, m_public_port, (unsigned long long)m_srtla_log_held, SLS_SRTLA_LOG_WINDOW_MS / 1000);
        }
        m_srtla_log_window_ms = now_ms;
        m_srtla_log_n = 0;
        m_srtla_log_held = 0;
    }
    if (m_srtla_log_n >= SLS_SRTLA_LOG_BURST) {
        m_srtla_log_held++;
        return false;
    }
    m_srtla_log_n++;
    return true;
}

void CSLSFrontDoor::reply(const struct sockaddr_in &to, const uint8_t *buf, int len)
{
    if (sendto(m_public_fd, buf, len, 0, (const struct sockaddr *)&to, sizeof(to)) < 0) {
        m_send_errors++;
    } else {
        m_relayed_out++;
    }
}

int CSLSFrontDoor::initial_hold() const
{
    int h = m_latency_ms / 4;
    return h < m_reorder_max_ms ? h : m_reorder_max_ms;
}

CSLSFrontDoor::group_rt *CSLSFrontDoor::rt_of(uint32_t handle)
{
    std::map<uint32_t, group_rt *>::iterator it = m_group_rt.find(handle);
    if (it != m_group_rt.end()) {
        return it->second;
    }
    group_rt *rt = new group_rt;
    rt->door   = this;
    rt->handle = handle;
    rt->fd     = -1;
    rt->buf    = new CSLSSrtlaReorder(initial_hold(), emit_to_session, rt);
    rt->logged = false;
    rt->peer_sock = 0;
    m_group_rt[handle] = rt;
    return rt;
}

void CSLSFrontDoor::emit_to_session(void *ctx, const uint8_t *pkt, int len)
{
    group_rt *rt = (group_rt *)ctx;
    if (rt->fd < 0) {
        rt->door->m_srtla_no_session++;
        return;
    }
    if (send(rt->fd, pkt, len, 0) < 0) {
        rt->door->m_send_errors++;
    } else {
        rt->door->m_relayed_in++;
    }
}

void CSLSFrontDoor::on_srtla_reg(int kind, const struct sockaddr_in &from, const uint8_t *buf, int len,
                                 int64_t now_ms)
{
    uint8_t id[SLS_SRTLA_ID_LEN];
    uint8_t out[SLS_SRTLA_REG_LEN];
    int type = (SLS_FD_SRTLA_REG1 == kind) ? SLS_SRTLA_TYPE_REG1 : SLS_SRTLA_TYPE_REG2;
    if (!sls_srtla_parse_reg(buf, len, type, id)) {
        m_dropped_srtla++;
        return;
    }
    sls_srtla_reg_result r;
    int n = 0;
    int pstate = SLS_FD_STATE_NONE;
    int pfd = m_peers.peek_remote(from, &pstate);
    if (m_ip_check && m_ip_check(m_ip_check_ctx, from.sin_addr.s_addr)) {
        // locked out (wrong passphrases) or over its handshake rate: no group, no link
        r = SLS_SRTLA_REG_ERR;
    } else if (SLS_SRTLA_TYPE_REG2 == type && pfd >= 0 && !m_group_by_fd.count(pfd)
               && SLS_FD_STATE_ENDED != pstate) {
        // a live plain SRT peer: a REG2 "from" it may be forged to steal its traffic
        r = SLS_SRTLA_REG_ERR;
    } else if (SLS_SRTLA_TYPE_REG1 == type) {
        uint8_t full[SLS_SRTLA_ID_LEN];
        r = m_groups->reg1(from, id, now_ms, full);
        n = (SLS_SRTLA_REG_OK == r) ? sls_srtla_build_reg(out, sizeof(out), SLS_SRTLA_TYPE_REG2, full) : 0;
    } else {
        r = m_groups->reg2(from, id, now_ms);
        n = (SLS_SRTLA_REG_OK == r) ? sls_srtla_build_short(out, sizeof(out), SLS_SRTLA_TYPE_REG3) : 0;
        if (SLS_SRTLA_REG_OK == r) {
            srtla_events(now_ms);   // a new group's rt exists before its first packet
        }
        if (SLS_SRTLA_REG_OK == r) {
            rt_of(m_groups->group_of(from));
        }
    }
    if (SLS_SRTLA_REG_OK == r) {
        m_srtla_reg_ok++;
    } else {
        m_srtla_reg_refused++;
        n = sls_srtla_build_short(out, sizeof(out),
                                  SLS_SRTLA_REG_NGP == r ? SLS_SRTLA_TYPE_REG_NGP : SLS_SRTLA_TYPE_REG_ERR);
        char ip[INET_ADDRSTRLEN];
        sls_peer_log(SLS_LOG_INFO, "[%p]CSLSFrontDoor, refused SRTLA %s from [%s:%d]: %s.",
                     this, SLS_SRTLA_TYPE_REG1 == type ? "REG1" : "REG2", sls_fd_ip(from, ip, sizeof(ip)),
                     ntohs(from.sin_port),
                     SLS_SRTLA_REG_NGP == r ? "unknown or expired group id" : "a cap, the rate, a lockout or a live peer");
    }
    reply(from, out, n);
}

int CSLSFrontDoor::open_group_session(group_rt *rt, int64_t now_ms)
{
    struct sockaddr_in reg;
    if (!m_groups->registering_addr(rt->handle, &reg)) {
        return -1;
    }
    // the registering address may still hold another session's mapping (a
    // plain one, or an older group from the same source): the newest wins
    int old = m_peers.fd_for_remote(reg, now_ms);
    if (old >= 0) {
        std::map<int, uint32_t>::iterator g = m_group_by_fd.find(old);
        if (g != m_group_by_fd.end()) {
            group_rt *other = rt_of(g->second);
            end_group_session(other, now_ms);
        } else {
            m_peers.remove_fd(old);
            quarantine(old, now_ms);
            m_pollfds_dirty = true;
        }
    }
    int fd = open_mapping(reg, now_ms);
    if (fd < 0) {
        return -1;
    }
    m_peers.set_srtla(fd);
    m_groups->set_fd(rt->handle, fd);
    m_group_by_fd[fd] = rt->handle;
    rt->fd = fd;
    rt->peer_sock = 0;
    rt->buf->reset();
    rt->buf->set_hold(initial_hold(), now_ms);
    return fd;
}

void CSLSFrontDoor::end_group_session(group_rt *rt, int64_t now_ms)
{
    if (rt->fd < 0) {
        return;
    }
    int fd = rt->fd;
    rt->buf->reset();
    rt->fd = -1;
    m_groups->set_fd(rt->handle, -1);
    m_group_by_fd.erase(fd);
    m_peers.remove_fd(fd);
    quarantine(fd, now_ms);   // libsrt may still hold a connection from its port
    m_pollfds_dirty = true;
}

void CSLSFrontDoor::forget_group_fd(int fd)
{
    // fd is being closed or quarantined by the caller (expired or evicted)
    std::map<int, uint32_t>::iterator g = m_group_by_fd.find(fd);
    if (g == m_group_by_fd.end()) {
        return;
    }
    std::map<uint32_t, group_rt *>::iterator it = m_group_rt.find(g->second);
    if (it != m_group_rt.end()) {
        it->second->fd = -1;
        it->second->buf->reset();
    }
    m_groups->set_fd(g->second, -1);
    m_group_by_fd.erase(g);
}

void CSLSFrontDoor::on_link_packet(uint32_t handle, const struct sockaddr_in &from, const uint8_t *buf, int len,
                                   int64_t now_ms)
{
    int type = sls_srtla_type(buf, len);
    if (SLS_SRTLA_TYPE_KEEPALIVE == type) {
        reply(from, buf, len);   // echoed to its link
        return;
    }
    if (SLS_SRTLA_TYPE_REG1 == type || SLS_SRTLA_TYPE_REG2 == type) {
        on_srtla_reg(SLS_SRTLA_TYPE_REG1 == type ? SLS_FD_SRTLA_REG1 : SLS_FD_SRTLA_REG2, from, buf, len, now_ms);
        return;
    }
    if (type < 0 || (type >= SLS_SRTLA_TYPE_FIRST && type <= SLS_SRTLA_TYPE_LAST) || len < SLS_SRT_HDR_LEN) {
        m_dropped_srtla++;   // nothing else SRTLA comes from a sender; too short for SRT
        return;
    }
    group_rt *rt = rt_of(handle);
    struct sockaddr_in reg;
    m_groups->registering_addr(handle, &reg);

    if (sls_srt_is_data(buf, len)) {
        // the link's SRTLA ACK counts what arrived on it, before the buffer
        uint32_t seqs[SLS_SRTLA_ACK_SEQS];
        if (m_groups->record_ack_seq(from, sls_srt_data_seq(buf), seqs)) {
            uint8_t ack[SLS_SRTLA_ACK_LEN];
            int n = sls_srtla_build_ack(ack, sizeof(ack), seqs);
            reply(from, ack, n);
            m_srtla_acks++;
        }
        if (rt->fd < 0) {
            m_srtla_no_session++;
            return;
        }
        m_peers.fd_for_remote(reg, now_ms);   // the session is alive
        rt->buf->push(buf, len, now_ms);
        return;
    }

    // SRT control from the sender bypasses the buffer
    if (SLS_SRT_TYPE_HANDSHAKE == type) {
        uint32_t hs_type = 0, isn = 0, sock = 0;
        if (!sls_srt_hs_fields(buf, len, &hs_type, &isn, &sock)) {
            m_dropped_srtla++;
            return;
        }
        int state = rt->fd >= 0 ? m_peers.state_for_fd(rt->fd) : SLS_FD_STATE_NONE;
        // a new session: none yet, or an induction from another SRT socket once
        // the current one got going (a late duplicate of this session's own
        // induction, over a slower link, is just relayed)
        bool fresh = SLS_FD_STATE_NONE == state
                     || (SLS_SRT_HS_INDUCTION == hs_type && sock != rt->peer_sock
                         && (SLS_FD_STATE_ESTABLISHED == state || SLS_FD_STATE_ENDED == state));
        if (fresh) {
            // a new SRT session in this group: a new socket and an empty buffer
            end_group_session(rt, now_ms);
            if (open_group_session(rt, now_ms) < 0) {
                return;   // over a mapping cap: the sender retries its handshake
            }
        }
        if (0 == rt->peer_sock) {
            rt->peer_sock = sock;
        }
        if (SLS_SRT_HS_CONCLUSION == hs_type) {
            rt->buf->start_at(isn);
        }
        m_groups->mark_handshake(handle);
    }
    if (rt->fd < 0) {
        m_srtla_no_session++;
        return;
    }
    m_peers.fd_for_remote(reg, now_ms, sls_fd_is_shutdown(buf, len));
    if (send(rt->fd, buf, len, 0) < 0) {
        m_send_errors++;
    } else {
        m_relayed_in++;
    }
}

void CSLSFrontDoor::on_group_readable(int fd, uint32_t handle, const uint8_t *buf, int len, int64_t now_ms)
{
    (void)fd;
    group_rt *rt = rt_of(handle);
    int type = sls_srtla_type(buf, len);
    if (SLS_SRT_TYPE_HANDSHAKE == type) {
        // libsrt's answer carries the session's TSBPD delays: hold a quarter of
        // the latency, at most srtla_reorder_max
        int cmd = 0, rcv = 0, snd = 0;
        if (sls_srt_hs_tsbpd(buf, len, &cmd, &rcv, &snd) && SLS_SRT_HS_CMD_HSRSP == cmd) {
            int lat  = rcv > snd ? rcv : snd;
            int hold = lat / 4 < m_reorder_max_ms ? lat / 4 : m_reorder_max_ms;
            rt->buf->set_hold(hold, now_ms);
            sls_log(SLS_LOG_DEBUG, "[%p]CSLSFrontDoor, SRTLA group %u: session latency rcv=%d snd=%d ms, reorder hold %d ms.",
                    this, handle, rcv, snd, hold);
        }
    }
    if (SLS_SRT_TYPE_ACK == type) {
        std::vector<struct sockaddr_in> to;
        m_groups->fanout_links(handle, now_ms, SLS_SRTLA_ACK_FANOUT_MS, &to);
        for (size_t i = 0; i < to.size(); i++) {
            reply(to[i], buf, len);
        }
        return;
    }
    struct sockaddr_in latest;
    if (m_groups->latest_link(handle, &latest)) {
        reply(latest, buf, len);
    }
}

void CSLSFrontDoor::srtla_tick(int64_t now_ms)
{
    for (std::map<uint32_t, group_rt *>::iterator it = m_group_rt.begin(); it != m_group_rt.end(); ++it) {
        if (0 == it->second->buf->next_deadline(now_ms)) {
            it->second->buf->tick(now_ms);
        }
    }
}

int CSLSFrontDoor::srtla_poll_timeout(int64_t now_ms) const
{
    int t = SLS_FD_POLL_MS;
    for (std::map<uint32_t, group_rt *>::const_iterator it = m_group_rt.begin(); it != m_group_rt.end(); ++it) {
        int d = it->second->buf->next_deadline(now_ms);
        if (d >= 0 && d < t) {
            t = d;
        }
    }
    return t;
}

void CSLSFrontDoor::srtla_events(int64_t now_ms)
{
    std::vector<sls_srtla_event> ev;
    m_groups->take_events(&ev);
    for (size_t i = 0; i < ev.size(); i++) {
        const sls_srtla_event &e = ev[i];
        char ip[INET_ADDRSTRLEN];
        sls_fd_ip(e.addr, ip, sizeof(ip));
        if (SLS_SRTLA_EV_GROUP_REMOVED == e.type) {
            std::map<uint32_t, group_rt *>::iterator it = m_group_rt.find(e.group);
            bool logged = false;
            if (it != m_group_rt.end()) {
                logged = it->second->logged;
                end_group_session(it->second, now_ms);
                delete it->second->buf;
                delete it->second;
                m_group_rt.erase(it);
            }
            // half-open groups (never a link) go silently: they may be spoofed
            if (logged && srtla_log_ok(now_ms)) {
                sls_log(SLS_LOG_INFO, "[%p]CSLSFrontDoor, SRTLA group %u ended: registered by %s:%d, reason=%s, "
                        "duration=%.1fs, peak links=%d.",
                        this, e.group, ip, ntohs(e.addr.sin_port), e.reason, e.duration_ms / 1000.0, e.peak_links);
            }
        } else if (SLS_SRTLA_EV_LINK_ADDED == e.type) {
            group_rt *rt = rt_of(e.group);
            if (!rt->logged) {
                rt->logged = true;
                struct sockaddr_in reg;
                m_groups->registering_addr(e.group, &reg);
                char rip[INET_ADDRSTRLEN];
                if (srtla_log_ok(now_ms)) {
                    sls_log(SLS_LOG_INFO, "[%p]CSLSFrontDoor, SRTLA group %u registered by %s:%d.",
                            this, e.group, sls_fd_ip(reg, rip, sizeof(rip)), ntohs(reg.sin_port));
                }
            }
            if (srtla_log_ok(now_ms)) {
                sls_log(SLS_LOG_INFO, "[%p]CSLSFrontDoor, SRTLA group %u: link %s:%d joined.",
                        this, e.group, ip, ntohs(e.addr.sin_port));
            }
        } else if (SLS_SRTLA_EV_LINK_REMOVED == e.type) {
            if (srtla_log_ok(now_ms)) {
                sls_log(SLS_LOG_INFO, "[%p]CSLSFrontDoor, SRTLA group %u: link %s:%d left (%s).",
                        this, e.group, ip, ntohs(e.addr.sin_port), e.reason);
            }
        }
    }
}
