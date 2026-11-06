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
#include <algorithm>

#include "SLSSrtlaGroups.hpp"

CSLSSrtlaGroups::CSLSSrtlaGroups(const sls_srtla_caps &caps, rand_fill_fn rand_fill)
{
    m_caps        = caps;
    m_rand_fill   = rand_fill;
    m_next_handle = 1;
    m_rand        = 0;
    m_rand_fill((uint8_t *)&m_rand, sizeof(m_rand));
    if (0 == m_rand) {
        m_rand = 0x9E3779B97F4A7C15ull;   // xorshift must not start at 0
    }
}

CSLSSrtlaGroups::~CSLSSrtlaGroups()
{
}

uint64_t CSLSSrtlaGroups::key_of(const struct sockaddr_in &a)
{
    return ((uint64_t)a.sin_addr.s_addr << 16) | a.sin_port;
}

uint32_t CSLSSrtlaGroups::next_rand()
{
    m_rand ^= m_rand << 13;
    m_rand ^= m_rand >> 7;
    m_rand ^= m_rand << 17;
    return (uint32_t)(m_rand >> 32);
}

void CSLSSrtlaGroups::event(sls_srtla_event_type type, uint32_t handle,
                            const struct sockaddr_in &addr, const char *reason)
{
    sls_srtla_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type   = type;
    ev.group  = handle;
    ev.addr   = addr;
    ev.reason = reason;
    ev.fd     = -1;
    m_events.push_back(ev);
}

bool CSLSSrtlaGroups::rate_ok(uint32_t ip_be, int64_t now_ms)
{
    if (m_caps.reg_per_ip_per_sec <= 0) {
        return true;
    }
    std::map<uint32_t, rate>::iterator it = m_rate.find(ip_be);
    if (it == m_rate.end()) {
        if (m_rate.size() >= SLS_SRTLA_RATE_TABLE_MAX) {
            for (std::map<uint32_t, rate>::iterator r = m_rate.begin(); r != m_rate.end();) {
                if (now_ms - r->second.window_ms >= SLS_SRTLA_RATE_WINDOW_MS) {
                    m_rate.erase(r++);
                } else {
                    ++r;
                }
            }
            if (m_rate.size() >= SLS_SRTLA_RATE_TABLE_MAX) {
                return false;   // a flood from more IPs than we track: refuse until it ages out
            }
        }
        rate r = {now_ms, 0};
        it = m_rate.insert(std::make_pair(ip_be, r)).first;
    }
    if (now_ms - it->second.window_ms >= SLS_SRTLA_RATE_WINDOW_MS) {
        it->second.window_ms = now_ms;
        it->second.count     = 0;
    }
    if (it->second.count >= m_caps.reg_per_ip_per_sec) {
        return false;
    }
    it->second.count++;
    return true;
}

int CSLSSrtlaGroups::groups_for_ip(uint32_t ip_be) const
{
    std::map<uint32_t, int>::const_iterator it = m_per_ip.find(ip_be);
    return it == m_per_ip.end() ? 0 : it->second;
}

sls_srtla_reg_result CSLSSrtlaGroups::reg1(const struct sockaddr_in &src, const uint8_t *sender_id,
                                           int64_t now_ms, uint8_t *full_id_out)
{
    uint32_t ip = src.sin_addr.s_addr;
    if (!rate_ok(ip, now_ms)) {
        return SLS_SRTLA_REG_ERR;
    }
    // at a cap, make room by evicting a random group of that scope that has no links
    for (int scope = 0; scope < 2; scope++) {
        bool per_ip = (0 == scope);
        bool full = per_ip ? (m_caps.max_groups_per_ip > 0 && groups_for_ip(ip) >= m_caps.max_groups_per_ip)
                           : (m_caps.max_groups > 0 && (int)m_groups.size() >= m_caps.max_groups);
        if (!full) {
            continue;
        }
        groups_t::iterator victim = m_groups.end();
        uint32_t seen = 0;
        for (groups_t::iterator it = m_groups.begin(); it != m_groups.end(); ++it) {
            if (!it->second.links.empty() || (per_ip && it->second.reg_addr.sin_addr.s_addr != ip)) {
                continue;
            }
            seen++;
            if (0 == next_rand() % seen) {   // reservoir sampling: each candidate equally likely
                victim = it;
            }
        }
        if (victim == m_groups.end()) {
            return SLS_SRTLA_REG_ERR;
        }
        remove_group(victim, "evicted", now_ms);
    }

    group g;
    memcpy(g.id, sender_id, SLS_SRTLA_ID_HALF);
    m_rand_fill(g.id + SLS_SRTLA_ID_HALF, SLS_SRTLA_ID_LEN - SLS_SRTLA_ID_HALF);
    std::string key((const char *)g.id, SLS_SRTLA_ID_LEN);
    if (m_by_id.count(key)) {
        return SLS_SRTLA_REG_ERR;   // 2^-1024 for real randomness; fail closed anyway
    }
    g.reg_addr       = src;
    g.created_ms     = now_ms;
    g.empty_since_ms = now_ms;
    g.handshake      = false;
    g.fd             = -1;
    g.peak_links     = 0;

    uint32_t handle = m_next_handle++;
    if (0 == m_next_handle) {
        m_next_handle = 1;
    }
    m_groups[handle] = g;
    m_by_id[key] = handle;
    m_per_ip[ip]++;
    event(SLS_SRTLA_EV_GROUP_ADDED, handle, src, "registered");
    memcpy(full_id_out, g.id, SLS_SRTLA_ID_LEN);
    return SLS_SRTLA_REG_OK;
}

CSLSSrtlaGroups::link *CSLSSrtlaGroups::find_link(const struct sockaddr_in &src, uint32_t *handle)
{
    std::map<uint64_t, uint32_t>::iterator li = m_links.find(key_of(src));
    if (li == m_links.end()) {
        return NULL;
    }
    groups_t::iterator gi = m_groups.find(li->second);
    if (gi == m_groups.end()) {
        return NULL;
    }
    std::vector<link> &ls = gi->second.links;
    for (size_t i = 0; i < ls.size(); i++) {
        if (key_of(ls[i].addr) == li->first) {
            if (handle) {
                *handle = li->second;
            }
            return &ls[i];
        }
    }
    return NULL;
}

sls_srtla_reg_result CSLSSrtlaGroups::reg2(const struct sockaddr_in &src, const uint8_t *full_id,
                                           int64_t now_ms)
{
    std::map<std::string, uint32_t>::iterator ii =
        m_by_id.find(std::string((const char *)full_id, SLS_SRTLA_ID_LEN));
    uint32_t cur = 0;
    link *l = find_link(src, &cur);
    if (ii != m_by_id.end() && l && cur == ii->second) {
        l->last_ms = now_ms;   // a link re-registering: not counted against the rate
        return SLS_SRTLA_REG_OK;
    }
    if (!rate_ok(src.sin_addr.s_addr, now_ms)) {
        return SLS_SRTLA_REG_ERR;
    }
    if (ii == m_by_id.end()) {
        return SLS_SRTLA_REG_NGP;
    }
    group &g = m_groups[ii->second];
    if (m_caps.max_links_per_group > 0 && (int)g.links.size() >= m_caps.max_links_per_group) {
        return SLS_SRTLA_REG_ERR;
    }
    if (l) {   // a link of another group moves here
        group &old = m_groups[cur];
        for (size_t i = 0; i < old.links.size(); i++) {
            if (key_of(old.links[i].addr) == key_of(src)) {
                remove_link(old, cur, i, "moved");
                break;
            }
        }
        if (old.links.empty()) {
            old.empty_since_ms = now_ms;
        }
    }
    link nl;
    memset(&nl, 0, sizeof(nl));
    nl.addr         = src;
    nl.last_ms      = now_ms;
    nl.last_data_ms = 0;
    nl.rate_ms      = now_ms;
    g.links.push_back(nl);
    if ((int)g.links.size() > g.peak_links) {
        g.peak_links = (int)g.links.size();
    }
    m_links[key_of(src)] = ii->second;
    event(SLS_SRTLA_EV_LINK_ADDED, ii->second, src, "registered");
    return SLS_SRTLA_REG_OK;
}

uint32_t CSLSSrtlaGroups::on_packet(const struct sockaddr_in &src, int len, bool data, int64_t now_ms)
{
    uint32_t handle = 0;
    link *l = find_link(src, &handle);
    if (!l) {
        return 0;
    }
    l->packets++;
    l->bytes += len > 0 ? (uint64_t)len : 0;
    l->last_ms = now_ms;
    if (data) {
        l->last_data_ms = now_ms;
    }
    return handle;
}

bool CSLSSrtlaGroups::record_ack_seq(const struct sockaddr_in &src, uint32_t seq, uint32_t *ack_out)
{
    link *l = find_link(src, NULL);
    if (!l) {
        return false;
    }
    l->ack_seqs[l->ack_n++] = seq;
    if (l->ack_n < SLS_SRTLA_ACK_SEQS) {
        return false;
    }
    memcpy(ack_out, l->ack_seqs, sizeof(l->ack_seqs));
    l->ack_n = 0;
    return true;
}

uint32_t CSLSSrtlaGroups::group_of(const struct sockaddr_in &src) const
{
    std::map<uint64_t, uint32_t>::const_iterator li = m_links.find(key_of(src));
    return li == m_links.end() ? 0 : li->second;
}

void CSLSSrtlaGroups::mark_handshake(uint32_t handle)
{
    groups_t::iterator it = m_groups.find(handle);
    if (it != m_groups.end()) {
        it->second.handshake = true;
    }
}

int CSLSSrtlaGroups::set_fd(uint32_t handle, int fd)
{
    groups_t::iterator it = m_groups.find(handle);
    if (it == m_groups.end()) {
        return -1;
    }
    int old = it->second.fd;
    it->second.fd = fd;
    return old;
}

int CSLSSrtlaGroups::fd_of(uint32_t handle) const
{
    groups_t::const_iterator it = m_groups.find(handle);
    return it == m_groups.end() ? -1 : it->second.fd;
}

bool CSLSSrtlaGroups::registering_addr(uint32_t handle, struct sockaddr_in *addr) const
{
    groups_t::const_iterator it = m_groups.find(handle);
    if (it == m_groups.end()) {
        return false;
    }
    *addr = it->second.reg_addr;
    return true;
}

static bool by_last_desc(const sls_srtla_link_stats &a, const sls_srtla_link_stats &b)
{
    return a.last_ms > b.last_ms;
}

void CSLSSrtlaGroups::links(uint32_t handle, std::vector<sls_srtla_link_stats> *out) const
{
    out->clear();
    groups_t::const_iterator it = m_groups.find(handle);
    if (it == m_groups.end()) {
        return;
    }
    for (size_t i = 0; i < it->second.links.size(); i++) {
        const link &l = it->second.links[i];
        sls_srtla_link_stats s;
        s.addr     = l.addr;
        s.packets  = l.packets;
        s.bytes    = l.bytes;
        s.last_ms  = l.last_ms;
        s.kbitrate = l.kbitrate;
        out->push_back(s);
    }
    std::stable_sort(out->begin(), out->end(), by_last_desc);
}

bool CSLSSrtlaGroups::latest_link(uint32_t handle, struct sockaddr_in *out) const
{
    groups_t::const_iterator it = m_groups.find(handle);
    if (it == m_groups.end() || it->second.links.empty()) {
        return false;
    }
    const std::vector<link> &ls = it->second.links;
    size_t best = 0;
    for (size_t i = 1; i < ls.size(); i++) {
        if (ls[i].last_ms > ls[best].last_ms) {
            best = i;
        }
    }
    *out = ls[best].addr;
    return true;
}

void CSLSSrtlaGroups::active_links(uint32_t handle, int64_t now_ms, int64_t window_ms,
                                   std::vector<struct sockaddr_in> *out) const
{
    out->clear();
    struct sockaddr_in latest;
    if (!latest_link(handle, &latest)) {
        return;
    }
    out->push_back(latest);
    const std::vector<link> &ls = m_groups.find(handle)->second.links;
    for (size_t i = 0; i < ls.size(); i++) {
        if (key_of(ls[i].addr) == key_of(latest)) {
            continue;
        }
        if (ls[i].last_data_ms > 0 && now_ms - ls[i].last_data_ms < window_ms) {
            out->push_back(ls[i].addr);
        }
    }
}

void CSLSSrtlaGroups::remove_link(group &g, uint32_t handle, size_t i, const char *reason)
{
    struct sockaddr_in a = g.links[i].addr;
    m_links.erase(key_of(a));
    g.links.erase(g.links.begin() + i);
    event(SLS_SRTLA_EV_LINK_REMOVED, handle, a, reason);
}

void CSLSSrtlaGroups::remove_group(groups_t::iterator it, const char *reason, int64_t now_ms)
{
    group &g = it->second;
    for (size_t i = 0; i < g.links.size(); i++) {
        m_links.erase(key_of(g.links[i].addr));
    }
    m_by_id.erase(std::string((const char *)g.id, SLS_SRTLA_ID_LEN));
    uint32_t ip = g.reg_addr.sin_addr.s_addr;
    if (--m_per_ip[ip] <= 0) {
        m_per_ip.erase(ip);
    }
    event(SLS_SRTLA_EV_GROUP_REMOVED, it->first, g.reg_addr, reason);
    m_events.back().fd          = g.fd;
    m_events.back().duration_ms = now_ms - g.created_ms;
    m_events.back().peak_links  = g.peak_links;
    m_groups.erase(it);
}

void CSLSSrtlaGroups::cleanup(int64_t now_ms)
{
    for (groups_t::iterator it = m_groups.begin(); it != m_groups.end();) {
        group &g = it->second;
        bool had_links = !g.links.empty();
        for (size_t i = g.links.size(); i-- > 0;) {
            link &l = g.links[i];
            if (now_ms - l.last_ms >= SLS_SRTLA_LINK_TIMEOUT_MS) {
                remove_link(g, it->first, i, "timed out");
                continue;
            }
            int64_t dt = now_ms - l.rate_ms;
            if (dt >= 1000) {
                l.kbitrate   = (int)((l.bytes - l.rate_bytes) * 8 / (uint64_t)dt);
                l.rate_bytes = l.bytes;
                l.rate_ms    = now_ms;
            }
        }
        if (had_links && g.links.empty()) {
            g.empty_since_ms = now_ms;
        }
        groups_t::iterator cur = it++;
        if (!g.handshake && now_ms - g.created_ms >= SLS_SRTLA_HANDSHAKE_MS) {
            remove_group(cur, "no handshake", now_ms);
        } else if (g.links.empty() && now_ms - g.empty_since_ms >= SLS_SRTLA_GROUP_EMPTY_MS) {
            remove_group(cur, "no links", now_ms);
        }
    }
    for (std::map<uint32_t, rate>::iterator r = m_rate.begin(); r != m_rate.end();) {
        if (now_ms - r->second.window_ms >= SLS_SRTLA_RATE_WINDOW_MS) {
            m_rate.erase(r++);
        } else {
            ++r;
        }
    }
}

void CSLSSrtlaGroups::clear(int64_t now_ms)
{
    while (!m_groups.empty()) {
        remove_group(m_groups.begin(), "stopped", now_ms);
    }
    m_rate.clear();
}

void CSLSSrtlaGroups::take_events(std::vector<sls_srtla_event> *out)
{
    out->swap(m_events);
    m_events.clear();
}
