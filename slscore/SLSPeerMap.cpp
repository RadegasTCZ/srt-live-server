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
      m_max_per_ip(max_per_ip > 0 ? max_per_ip : 0)
{
}

CSLSPeerMap::~CSLSPeerMap()
{
}

uint64_t CSLSPeerMap::key_of(const struct sockaddr_in &remote)
{
    return ((uint64_t)ntohl(remote.sin_addr.s_addr) << 16) | ntohs(remote.sin_port);
}

int CSLSPeerMap::add(const struct sockaddr_in &remote, int fd, uint16_t local_port, int64_t now_ms)
{
    CSLSLock lock(&m_mutex);
    uint64_t key = key_of(remote);
    if ((int)m_by_remote.size() >= m_max_total) {
        return SLS_ERROR;
    }
    if (m_by_remote.count(key) || m_by_fd.count(fd) || m_by_local_port.count(local_port)) {
        return SLS_ERROR;
    }
    uint32_t ip = remote.sin_addr.s_addr;
    std::map<uint32_t, int>::iterator ip_it = m_per_ip.find(ip);
    int on_ip = ip_it == m_per_ip.end() ? 0 : ip_it->second;
    if (m_max_per_ip > 0 && on_ip >= m_max_per_ip) {
        return SLS_ERROR;
    }
    entry e;
    memset(&e, 0, sizeof(e));
    e.remote      = remote;
    e.fd          = fd;
    e.local_port  = local_port;
    e.created_ms  = now_ms;
    e.last_in_ms  = now_ms;
    e.last_out_ms = now_ms;
    e.established = false;
    m_by_remote[key] = e;
    m_by_fd[fd] = key;
    m_by_local_port[local_port] = key;
    m_per_ip[ip] = on_ip + 1;
    return SLS_OK;
}

int CSLSPeerMap::fd_for_remote(const struct sockaddr_in &remote, int64_t now_ms)
{
    CSLSLock lock(&m_mutex);
    std::map<uint64_t, entry>::iterator it = m_by_remote.find(key_of(remote));
    if (it == m_by_remote.end()) {
        return -1;
    }
    it->second.last_in_ms = now_ms;
    it->second.established = true;   // add() counted the first packet: this is a later one
    return it->second.fd;
}

bool CSLSPeerMap::remote_for_fd(int fd, struct sockaddr_in *remote, int64_t now_ms)
{
    CSLSLock lock(&m_mutex);
    std::map<int, uint64_t>::iterator f = m_by_fd.find(fd);
    if (f == m_by_fd.end()) {
        return false;
    }
    std::map<uint64_t, entry>::iterator it = m_by_remote.find(f->second);
    if (it == m_by_remote.end()) {
        return false;
    }
    it->second.last_out_ms = now_ms;
    if (remote) {
        *remote = it->second.remote;
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
    std::map<uint64_t, entry>::const_iterator it = m_by_remote.find(p->second);
    if (it == m_by_remote.end()) {
        return false;
    }
    if (remote) {
        *remote = it->second.remote;
    }
    return true;
}

void CSLSPeerMap::erase_locked(std::map<uint64_t, entry>::iterator it)
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
    std::map<uint64_t, entry>::iterator it = m_by_remote.begin();
    while (it != m_by_remote.end()) {
        const entry &e = it->second;
        bool dead;
        if (e.closing_ms > 0 && now_ms - e.closing_ms > SLS_FD_CLOSING_MS) {
            dead = true;
        } else if (!e.established) {
            dead = now_ms - e.created_ms > halfopen_ms;
        } else {
            int64_t last = e.last_in_ms > e.last_out_ms ? e.last_in_ms : e.last_out_ms;
            dead = now_ms - last > idle_ms;
        }
        if (!dead) {
            ++it;
            continue;
        }
        if (expired_fds) {
            expired_fds->push_back(e.fd);
        }
        std::map<uint64_t, entry>::iterator victim = it++;
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
    std::map<uint64_t, entry>::iterator it = m_by_remote.find(f->second);
    if (it == m_by_remote.end()) {
        m_by_fd.erase(f);
        return;
    }
    erase_locked(it);
}

void CSLSPeerMap::mark_closing(int fd, int64_t now_ms)
{
    CSLSLock lock(&m_mutex);
    std::map<int, uint64_t>::iterator f = m_by_fd.find(fd);
    if (f == m_by_fd.end()) {
        return;
    }
    std::map<uint64_t, entry>::iterator it = m_by_remote.find(f->second);
    if (it != m_by_remote.end() && 0 == it->second.closing_ms) {
        it->second.closing_ms = now_ms > 0 ? now_ms : 1;
    }
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
