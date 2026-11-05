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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "SLSFrontDoor.hpp"
#include "SLSFrontDoorProto.hpp"
#include "SLSLog.hpp"
#include "common.hpp"

#define SLS_SRT_TYPE_SHUTDOWN 0x8005

static bool sls_fd_is_shutdown(const uint8_t *p, int len)
{
    return len >= SLS_SRT_HDR_LEN && p[0] == (SLS_SRT_TYPE_SHUTDOWN >> 8) && p[1] == (SLS_SRT_TYPE_SHUTDOWN & 0xFF);
}

CSLSFrontDoor::CSLSFrontDoor(int public_port, int internal_port, int max_total, int max_per_ip)
    : m_relayed_in(0), m_relayed_out(0), m_dropped_srtla(0), m_dropped_unknown(0),
      m_refused_cap(0), m_send_errors(0), m_local_errors(0), m_mappings_created(0), m_mappings_expired(0),
      m_mappings_evicted(0),
      m_failed(false), m_public_port(public_port), m_internal_port(internal_port), m_public_fd(-1),
      m_peers(max_total, max_per_ip), m_pollfds_dirty(true),
      m_last_expire_ms(0), m_last_summary_ms(0), m_last_cap_log_ms(0), m_last_local_error_log_ms(-100000),
      m_summary_seen(0), m_rcvbuf(0), m_sndbuf(0)
{
}

CSLSFrontDoor::~CSLSFrontDoor()
{
    stop();        // joins the thread if it runs (a no-op otherwise)
    close_all();   // a front door that never started still owns its public socket
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
    // at a cap the map evicts a half-open (or long-silent) entry rather than
    // refusing, so a flood of spoofed handshakes cannot keep real peers out
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
        close(evicted);
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
        bool shutdown = sls_fd_is_shutdown(buf, (int)len);
        int fd = m_peers.fd_for_remote(from, now_ms, shutdown);
        if (fd < 0) {
            sls_fd_kind_t kind = sls_fd_classify(buf, (int)len);
            if (SLS_FD_SRT_HANDSHAKE == kind) {
                fd = open_mapping(from, now_ms);
                if (fd < 0) {
                    continue;
                }
            } else if (SLS_FD_SRTLA_REG1 == kind || SLS_FD_SRTLA_REG2 == kind || SLS_FD_SRTLA_OTHER == kind) {
                m_dropped_srtla++;   // stage 2 answers these
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
        if (sendto(m_public_fd, buf, len, 0, (struct sockaddr *)&remote, sizeof(remote)) < 0) {
            m_send_errors++;
        } else {
            m_relayed_out++;
        }
    }
}

void CSLSFrontDoor::expire(int64_t now_ms)
{
    std::vector<int> fds;
    m_peers.expire(now_ms, SLS_FD_HALFOPEN_MS, SLS_FD_IDLE_MS, &fds);
    for (size_t i = 0; i < fds.size(); i++) {
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
    sls_log(SLS_LOG_INFO, "[%p]CSLSFrontDoor, started: public port %d -> 127.0.0.1:%d, caps %d in total, %d per IP, "
            "socket buffers rcv=%d snd=%d%s.",
            this, m_public_port, m_internal_port, m_peers.max_total(), m_peers.max_per_ip(),
            m_rcvbuf, m_sndbuf,
            (m_rcvbuf < SLS_FD_SOCKBUF || m_sndbuf < SLS_FD_SOCKBUF)
                ? " (below the 4 MB asked for: raise net.core.rmem_max/wmem_max)" : "");
    int64_t now = sls_monotonic_ms();
    m_last_expire_ms = now;
    m_last_summary_ms = now;
    while (!m_exit) {
        if (m_pollfds_dirty) {
            rebuild_pollfds();
        }
        int ret = poll(&m_pollfds[0], m_pollfds.size(), SLS_FD_POLL_MS);
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
            "relayed_in=%llu relayed_out=%llu dropped_srtla=%llu dropped_unknown=%llu refused_cap=%llu send_errors=%llu local_errors=%llu.",
            this, m_public_port, m_peers.size(),
            (unsigned long long)m_mappings_created, (unsigned long long)m_mappings_expired,
            (unsigned long long)m_mappings_evicted,
            (unsigned long long)m_relayed_in, (unsigned long long)m_relayed_out,
            (unsigned long long)m_dropped_srtla, (unsigned long long)m_dropped_unknown,
            (unsigned long long)m_refused_cap, (unsigned long long)m_send_errors,
            (unsigned long long)m_local_errors);
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
    if (m_public_fd >= 0) {
        close(m_public_fd);
        m_public_fd = -1;
    }
    m_pollfds.clear();
    m_pollfds_dirty = true;
}
