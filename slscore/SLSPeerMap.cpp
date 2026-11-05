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


#include <string.h>

#include "SLSPeerMap.hpp"
#include "common.hpp"

CSLSPeerMap::CSLSPeerMap(int max_total, int max_per_ip)
    : m_max_total(max_total > 0 ? max_total : 1),
      m_max_per_ip(max_per_ip > 0 ? max_per_ip : 0),
      m_rand(0x9E3779B97F4A7C15ULL ^ (uint64_t)(uintptr_t)this)
{
}

CSLSPeerMap::~CSLSPeerMap()
{
}

uint64_t CSLSPeerMap::key_of(const struct sockaddr_in &remote)
{
    return ((uint64_t)ntohl(remote.sin_addr.s_addr) << 16) | ntohs(remote.sin_port);
}

CSLSPeerMap::by_remote_t::iterator CSLSPeerMap::victim_locked(bool ip_only, uint32_t ip, int64_t now_ms)
{
    by_remote_t::iterator ended = m_by_remote.end(), half = m_by_remote.end(), quiet = m_by_remote.end();
    uint64_t halves = 0;
    for (by_remote_t::iterator it = m_by_remote.begin(); it != m_by_remote.end(); ++it) {
        const entry &e = it->second;
        if (ip_only && e.remote.sin_addr.s_addr != ip) {
            continue;
        }
        if (e.closing_ms > 0) {
            // libsrt ended its session: only its last packets are left to relay
            if (ended == m_by_remote.end() || e.closing_ms < ended->second.closing_ms) {
                ended = it;
            }
        } else if (!e.established) {
            // one pass, uniformly at random (reservoir sampling)
            m_rand ^= m_rand << 13; m_rand ^= m_rand >> 7; m_rand ^= m_rand << 17;
            if (0 == m_rand % ++halves) {
                half = it;
            }
        } else if (now_ms - e.last_in_ms >= SLS_FD_QUIET_MS) {
            if (quiet == m_by_remote.end() || e.last_in_ms < quiet->second.last_in_ms) {
                quiet = it;
            }
        }
    }
    if (ended != m_by_remote.end()) {
        return ended;
    }
    return half != m_by_remote.end() ? half : quiet;
}

int CSLSPeerMap::add(const struct sockaddr_in &remote, int fd, uint16_t local_port, int64_t now_ms,
                     int *evicted_fd)
{
    if (evicted_fd) {
        *evicted_fd = -1;
    }
    CSLSLock lock(&m_mutex);
    uint64_t key = key_of(remote);
    if (m_by_remote.count(key) || m_by_fd.count(fd) || m_by_local_port.count(local_port)) {
        return SLS_ERROR;
    }
    uint32_t ip = remote.sin_addr.s_addr;
    std::map<uint32_t, int>::iterator ip_it = m_per_ip.find(ip);
    int on_ip = ip_it == m_per_ip.end() ? 0 : ip_it->second;
    bool ip_full  = m_max_per_ip > 0 && on_ip >= m_max_per_ip;
    bool all_full = (int)m_by_remote.size() >= m_max_total;
    if (ip_full || all_full) {
        // over the per-IP cap only that IP's entries may make room
        by_remote_t::iterator v = victim_locked(ip_full, ip, now_ms);
        if (v == m_by_remote.end() || NULL == evicted_fd) {
            return SLS_ERROR;
        }
        *evicted_fd = v->second.fd;
        erase_locked(v);
        on_ip = m_per_ip.count(ip) ? m_per_ip[ip] : 0;
    }
    entry e;
    memset(&e, 0, sizeof(e));
    e.remote      = remote;
    e.fd          = fd;
    e.local_port  = local_port;
    e.created_ms  = now_ms;
    e.last_in_ms  = now_ms;
    e.established = false;
    m_by_remote[key] = e;
    m_by_fd[fd] = key;
    m_by_local_port[local_port] = key;
    m_per_ip[ip] = on_ip + 1;
    return SLS_OK;
}

int CSLSPeerMap::fd_for_remote(const struct sockaddr_in &remote, int64_t now_ms, bool shutdown,
                               bool handshake)
{
    CSLSLock lock(&m_mutex);
    by_remote_t::iterator it = m_by_remote.find(key_of(remote));
    if (it == m_by_remote.end()) {
        return -1;
    }
    entry &e = it->second;
    e.last_in_ms = now_ms;
    if (handshake && e.closing_ms > 0) {
        // the remote reconnects from the same address and port after libsrt
        // ended its session: a new session on this socket, half-open again
        e.closing_ms = 0;
        e.peer_shutdown_ms = 0;
        e.established = false;
        e.created_ms = now_ms;
    }
    if (shutdown) {
        if (0 == e.peer_shutdown_ms) {
            e.peer_shutdown_ms = now_ms > 0 ? now_ms : 1;
        }
    } else {
        e.peer_shutdown_ms = 0;   // still talking: a shutdown "from" it was forged or stale
    }
    return e.fd;
}

bool CSLSPeerMap::remote_for_fd(int fd, struct sockaddr_in *remote, int64_t now_ms,
                                bool confirms, bool shutdown)
{
    CSLSLock lock(&m_mutex);
    std::map<int, uint64_t>::iterator f = m_by_fd.find(fd);
    if (f == m_by_fd.end()) {
        return false;
    }
    by_remote_t::iterator it = m_by_remote.find(f->second);
    if (it == m_by_remote.end()) {
        return false;
    }
    entry &e = it->second;
    if (confirms && !e.established) {
        e.established = true;
        e.last_in_ms = now_ms;   // idle counts from the handshake's end
    }
    if (shutdown && 0 == e.closing_ms) {
        e.closing_ms = now_ms > 0 ? now_ms : 1;
    }
    if (remote) {
        *remote = e.remote;
    }
    return true;
}

bool CSLSPeerMap::remote_for_local_port(uint16_t local_port, struct sockaddr_in *remote) const
{
    CSLSLock lock(&m_mutex);
    std::map<uint16_t, uint64_t>::const_iterator p = m_by_local_port.find(local_port);
    if (p == m_by_local_port.end()) {
        return false;
    }
    by_remote_t::const_iterator it = m_by_remote.find(p->second);
    if (it == m_by_remote.end()) {
        return false;
    }
    if (remote) {
        *remote = it->second.remote;
    }
    return true;
}

void CSLSPeerMap::erase_locked(by_remote_t::iterator it)
{
    uint32_t ip = it->second.remote.sin_addr.s_addr;
    std::map<uint32_t, int>::iterator ip_it = m_per_ip.find(ip);
    if (ip_it != m_per_ip.end()) {
        if (--ip_it->second <= 0) {
            m_per_ip.erase(ip_it);
        }
    }
    m_by_fd.erase(it->second.fd);
    m_by_local_port.erase(it->second.local_port);
    m_by_remote.erase(it);
}

void CSLSPeerMap::expire(int64_t now_ms, int halfopen_ms, int idle_ms, std::vector<int> *expired_fds)
{
    CSLSLock lock(&m_mutex);
    by_remote_t::iterator it = m_by_remote.begin();
    while (it != m_by_remote.end()) {
        const entry &e = it->second;
        bool dead;
        if (e.closing_ms > 0 && now_ms - e.closing_ms > SLS_FD_CLOSING_MS) {
            dead = true;
        } else if (e.peer_shutdown_ms > 0 && now_ms - e.peer_shutdown_ms > SLS_FD_CLOSING_MS) {
            dead = true;
        } else if (!e.established) {
            dead = now_ms - e.created_ms > halfopen_ms;
        } else {
            dead = now_ms - e.last_in_ms > idle_ms;
        }
        if (!dead) {
            ++it;
            continue;
        }
        if (expired_fds) {
            expired_fds->push_back(e.fd);
        }
        by_remote_t::iterator victim = it++;
        erase_locked(victim);
    }
}

void CSLSPeerMap::remove_fd(int fd)
{
    CSLSLock lock(&m_mutex);
    std::map<int, uint64_t>::iterator f = m_by_fd.find(fd);
    if (f == m_by_fd.end()) {
        return;
    }
    by_remote_t::iterator it = m_by_remote.find(f->second);
    if (it == m_by_remote.end()) {
        m_by_fd.erase(f);
        return;
    }
    erase_locked(it);
}

void CSLSPeerMap::all_fds(std::vector<int> *fds) const
{
    CSLSLock lock(&m_mutex);
    for (std::map<int, uint64_t>::const_iterator f = m_by_fd.begin(); f != m_by_fd.end(); ++f) {
        fds->push_back(f->first);
    }
}

size_t CSLSPeerMap::size() const
{
    CSLSLock lock(&m_mutex);
    return m_by_remote.size();
}

int CSLSPeerMap::count_for_ip(uint32_t ip_be) const
{
    CSLSLock lock(&m_mutex);
    std::map<uint32_t, int>::const_iterator it = m_per_ip.find(ip_be);
    return it == m_per_ip.end() ? 0 : it->second;
}
