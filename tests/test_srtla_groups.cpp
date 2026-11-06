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
#include <arpa/inet.h>
#include <vector>
#include "sls_test.hpp"
#include "SLSSrtlaGroups.hpp"

// deterministic pseudo-random bytes (a plain counter would repeat ids)
static uint64_t g_fill = 88172645463325252ull;
static void fill_counter(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        g_fill ^= g_fill << 13; g_fill ^= g_fill >> 7; g_fill ^= g_fill << 17;
        buf[i] = (uint8_t)(g_fill >> 56);
    }
}

static struct sockaddr_in addr(const char *ip, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

static sls_srtla_caps caps(int groups, int per_ip, int links, int rate)
{
    sls_srtla_caps c = {groups, per_ip, links, rate};
    return c;
}

static int count_events(CSLSSrtlaGroups &g, sls_srtla_event_type t, const char *reason = NULL)
{
    std::vector<sls_srtla_event> ev;
    g.take_events(&ev);
    int n = 0;
    for (size_t i = 0; i < ev.size(); i++) {
        if (ev[i].type == t && (!reason || 0 == strcmp(reason, ev[i].reason))) n++;
    }
    return n;
}

// REG1 then REG2 from the same source; returns the group handle
static uint32_t join(CSLSSrtlaGroups &g, const struct sockaddr_in &a, int64_t now, uint8_t *id = NULL)
{
    uint8_t sid[SLS_SRTLA_ID_LEN], full[SLS_SRTLA_ID_LEN];
    memset(sid, 0x5A, sizeof(sid));
    if (g.reg1(a, sid, now, full) != SLS_SRTLA_REG_OK) return 0;
    if (g.reg2(a, full, now) != SLS_SRTLA_REG_OK) return 0;
    if (id) memcpy(id, full, SLS_SRTLA_ID_LEN);
    return g.group_of(a);
}

SLS_TEST(groups_reg1_id)
{
    CSLSSrtlaGroups g(caps(200, 4, 8, 0), fill_counter);
    uint8_t sid[SLS_SRTLA_ID_LEN], full[SLS_SRTLA_ID_LEN];
    for (int i = 0; i < SLS_SRTLA_ID_LEN; i++) sid[i] = (uint8_t)(0xFF - i);
    SLS_CHECK_EQ(g.reg1(addr("10.0.0.1", 5000), sid, 0, full), SLS_SRTLA_REG_OK);
    // the sender's first half is kept, the second half is ours
    SLS_CHECK(0 == memcmp(full, sid, SLS_SRTLA_ID_HALF));
    SLS_CHECK(0 != memcmp(full + SLS_SRTLA_ID_HALF, sid + SLS_SRTLA_ID_HALF, SLS_SRTLA_ID_HALF));
    SLS_CHECK_EQ(g.group_count(), 1u);
    SLS_CHECK_EQ(g.link_count(), 0u);   // REG1 alone adds no link
    SLS_CHECK_EQ(count_events(g, SLS_SRTLA_EV_GROUP_ADDED), 1);
}

SLS_TEST(groups_reg2_links)
{
    CSLSSrtlaGroups g(caps(200, 4, 8, 0), fill_counter);
    uint8_t id[SLS_SRTLA_ID_LEN];
    uint32_t h = join(g, addr("10.0.0.1", 5000), 0, id);
    SLS_CHECK(h != 0);
    SLS_CHECK_EQ(g.reg2(addr("10.0.0.2", 6000), id, 10), SLS_SRTLA_REG_OK);   // a second link, other IP
    SLS_CHECK_EQ(g.group_of(addr("10.0.0.2", 6000)), h);
    SLS_CHECK_EQ(g.link_count(), 2u);
    SLS_CHECK_EQ(g.reg2(addr("10.0.0.2", 6000), id, 20), SLS_SRTLA_REG_OK);   // re-register: same link
    SLS_CHECK_EQ(g.link_count(), 2u);
    struct sockaddr_in reg;
    SLS_CHECK(g.registering_addr(h, &reg));
    SLS_CHECK_EQ(ntohs(reg.sin_port), 5000);
    // an unknown id
    uint8_t bad[SLS_SRTLA_ID_LEN];
    memcpy(bad, id, sizeof(bad));
    bad[200] ^= 1;
    SLS_CHECK_EQ(g.reg2(addr("10.0.0.3", 7000), bad, 30), SLS_SRTLA_REG_NGP);
    SLS_CHECK_EQ(g.group_of(addr("10.0.0.3", 7000)), 0u);
}

SLS_TEST(groups_link_cap)
{
    CSLSSrtlaGroups g(caps(200, 4, 3, 0), fill_counter);
    uint8_t id[SLS_SRTLA_ID_LEN];
    join(g, addr("10.0.0.1", 1), 0, id);
    SLS_CHECK_EQ(g.reg2(addr("10.0.0.1", 2), id, 0), SLS_SRTLA_REG_OK);
    SLS_CHECK_EQ(g.reg2(addr("10.0.0.1", 3), id, 0), SLS_SRTLA_REG_OK);
    SLS_CHECK_EQ(g.reg2(addr("10.0.0.1", 4), id, 0), SLS_SRTLA_REG_ERR);
    SLS_CHECK_EQ(g.link_count(), 3u);
}

SLS_TEST(groups_per_ip_cap_and_eviction)
{
    CSLSSrtlaGroups g(caps(200, 2, 8, 0), fill_counter);
    uint8_t sid[SLS_SRTLA_ID_LEN], full[SLS_SRTLA_ID_LEN];
    memset(sid, 1, sizeof(sid));
    // two established groups from one IP: a third REG1 is refused
    SLS_CHECK(join(g, addr("10.0.0.1", 1), 0) != 0);
    SLS_CHECK(join(g, addr("10.0.0.1", 2), 0) != 0);
    SLS_CHECK_EQ(g.reg1(addr("10.0.0.1", 3), sid, 0, full), SLS_SRTLA_REG_ERR);
    SLS_CHECK_EQ(g.groups_for_ip(inet_addr("10.0.0.1")), 2);
    // another IP is not affected
    SLS_CHECK_EQ(g.reg1(addr("10.0.0.9", 1), sid, 0, full), SLS_SRTLA_REG_OK);
    // half-open groups (no link yet) are evicted to make room
    CSLSSrtlaGroups h(caps(200, 2, 8, 0), fill_counter);
    SLS_CHECK_EQ(h.reg1(addr("10.0.0.1", 1), sid, 0, full), SLS_SRTLA_REG_OK);
    SLS_CHECK_EQ(h.reg1(addr("10.0.0.1", 2), sid, 0, full), SLS_SRTLA_REG_OK);
    count_events(h, SLS_SRTLA_EV_GROUP_ADDED);
    SLS_CHECK_EQ(h.reg1(addr("10.0.0.1", 3), sid, 0, full), SLS_SRTLA_REG_OK);
    SLS_CHECK_EQ(h.groups_for_ip(inet_addr("10.0.0.1")), 2);
    std::vector<sls_srtla_event> ev;
    h.take_events(&ev);
    SLS_CHECK_EQ(ev.size(), 2u);
    SLS_CHECK(ev.size() == 2 && ev[0].type == SLS_SRTLA_EV_GROUP_REMOVED && 0 == strcmp(ev[0].reason, "evicted"));
}

SLS_TEST(groups_global_cap_spoof_flood)
{
    // a flood of spoofed REG1s cannot lock out a real sender: half-open groups
    // are evicted at random, and a sender that completes REG2 is safe
    CSLSSrtlaGroups g(caps(10, 0, 8, 0), fill_counter);
    uint8_t sid[SLS_SRTLA_ID_LEN], full[SLS_SRTLA_ID_LEN], real[SLS_SRTLA_ID_LEN];
    memset(sid, 2, sizeof(sid));
    SLS_CHECK_EQ(g.reg1(addr("192.0.2.1", 1), sid, 0, real), SLS_SRTLA_REG_OK);
    SLS_CHECK_EQ(g.reg2(addr("192.0.2.1", 1), real, 0), SLS_SRTLA_REG_OK);
    char ip[32];
    for (int i = 0; i < 1000; i++) {
        snprintf(ip, sizeof(ip), "10.%d.%d.1", i / 250, i % 250);
        SLS_CHECK_EQ(g.reg1(addr(ip, 1), sid, 0, full), SLS_SRTLA_REG_OK);
    }
    SLS_CHECK_EQ(g.group_count(), 10u);
    SLS_CHECK(g.group_of(addr("192.0.2.1", 1)) != 0);   // the established group survived
    // all groups established: a new REG1 is refused
    CSLSSrtlaGroups f(caps(2, 0, 8, 0), fill_counter);
    SLS_CHECK(join(f, addr("10.0.0.1", 1), 0) != 0);
    SLS_CHECK(join(f, addr("10.0.0.2", 1), 0) != 0);
    SLS_CHECK_EQ(f.reg1(addr("10.0.0.3", 1), sid, 0, full), SLS_SRTLA_REG_ERR);
}

SLS_TEST(groups_rate_limit)
{
    CSLSSrtlaGroups g(caps(200, 100, 8, 3), fill_counter);
    uint8_t sid[SLS_SRTLA_ID_LEN], full[SLS_SRTLA_ID_LEN];
    memset(sid, 3, sizeof(sid));
    SLS_CHECK_EQ(g.reg1(addr("10.0.0.1", 1), sid, 0, full), SLS_SRTLA_REG_OK);
    SLS_CHECK_EQ(g.reg2(addr("10.0.0.1", 1), full, 100), SLS_SRTLA_REG_OK);
    SLS_CHECK_EQ(g.reg1(addr("10.0.0.1", 2), sid, 200, full), SLS_SRTLA_REG_OK);
    SLS_CHECK_EQ(g.reg1(addr("10.0.0.1", 3), sid, 300, full), SLS_SRTLA_REG_ERR);   // 4th in the second
    // a registered link re-sending REG2 is not counted
    uint8_t id[SLS_SRTLA_ID_LEN];
    CSLSSrtlaGroups r(caps(200, 100, 8, 2), fill_counter);
    join(r, addr("10.0.0.5", 1), 0, id);
    for (int i = 0; i < 10; i++) SLS_CHECK_EQ(r.reg2(addr("10.0.0.5", 1), id, 10 + i), SLS_SRTLA_REG_OK);
    // an unknown REG2 counts (no free probing of ids)
    SLS_CHECK_EQ(r.reg2(addr("10.0.0.5", 9), id, 20), SLS_SRTLA_REG_ERR);
    // the next window allows again
    SLS_CHECK_EQ(g.reg1(addr("10.0.0.1", 3), sid, 1000, full), SLS_SRTLA_REG_OK);
    // other IPs are separate
    SLS_CHECK_EQ(g.reg1(addr("10.0.0.2", 1), sid, 300, full), SLS_SRTLA_REG_OK);
}

SLS_TEST(groups_link_move)
{
    CSLSSrtlaGroups g(caps(200, 4, 8, 0), fill_counter);
    uint8_t a[SLS_SRTLA_ID_LEN], b[SLS_SRTLA_ID_LEN];
    uint32_t ha = join(g, addr("10.0.0.1", 1), 0, a);
    uint32_t hb = join(g, addr("10.0.0.2", 1), 0, b);
    SLS_CHECK(ha != hb);
    count_events(g, SLS_SRTLA_EV_LINK_ADDED);
    SLS_CHECK_EQ(g.reg2(addr("10.0.0.1", 1), b, 10), SLS_SRTLA_REG_OK);
    SLS_CHECK_EQ(g.group_of(addr("10.0.0.1", 1)), hb);
    SLS_CHECK_EQ(g.link_count(), 2u);
    SLS_CHECK_EQ(count_events(g, SLS_SRTLA_EV_LINK_REMOVED, "moved"), 1);
}

SLS_TEST(groups_timeouts)
{
    CSLSSrtlaGroups g(caps(200, 4, 8, 0), fill_counter);
    uint8_t id[SLS_SRTLA_ID_LEN];
    struct sockaddr_in l1 = addr("10.0.0.1", 1), l2 = addr("10.0.0.2", 1);
    uint32_t h = join(g, l1, 0, id);
    g.reg2(l2, id, 0);
    g.mark_handshake(h);
    // l1 keeps sending, l2 goes silent
    for (int64_t t = 1000; t <= 12000; t += 1000) {
        g.on_packet(l1, 1316, true, t);
        g.cleanup(t);
    }
    SLS_CHECK_EQ(g.group_of(l2), 0u);
    SLS_CHECK_EQ(g.group_of(l1), h);
    count_events(g, SLS_SRTLA_EV_LINK_REMOVED);
    // l1 goes silent too: the link goes at 10 s, the empty group 10 s later
    g.cleanup(21999);
    SLS_CHECK_EQ(g.group_count(), 1u);
    g.cleanup(22000);
    SLS_CHECK_EQ(g.link_count(), 0u);
    SLS_CHECK_EQ(g.group_count(), 1u);
    g.cleanup(31999);
    SLS_CHECK_EQ(g.group_count(), 1u);
    g.set_fd(h, 42);
    g.cleanup(32000);
    SLS_CHECK_EQ(g.group_count(), 0u);
    std::vector<sls_srtla_event> ev;
    g.take_events(&ev);
    bool found = false;
    for (size_t i = 0; i < ev.size(); i++) {
        if (ev[i].type == SLS_SRTLA_EV_GROUP_REMOVED) {
            found = true;
            SLS_CHECK(0 == strcmp(ev[i].reason, "no links"));
            SLS_CHECK_EQ(ev[i].fd, 42);
            SLS_CHECK_EQ(ev[i].peak_links, 2);
            SLS_CHECK_EQ(ev[i].duration_ms, 32000);
        }
    }
    SLS_CHECK(found);
    SLS_CHECK_EQ(g.groups_for_ip(inet_addr("10.0.0.1")), 0);
}

SLS_TEST(groups_no_handshake)
{
    CSLSSrtlaGroups g(caps(200, 4, 8, 0), fill_counter);
    struct sockaddr_in l1 = addr("10.0.0.1", 1);
    join(g, l1, 0);
    for (int64_t t = 1000; t < 10000; t += 1000) {
        g.on_packet(l1, 100, false, t);
        g.cleanup(t);
    }
    SLS_CHECK_EQ(g.group_count(), 1u);
    g.on_packet(l1, 100, false, 10000);
    g.cleanup(10000);
    SLS_CHECK_EQ(g.group_count(), 0u);   // registered, links alive, but no SRT session
    SLS_CHECK_EQ(count_events(g, SLS_SRTLA_EV_GROUP_REMOVED, "no handshake"), 1);
}

SLS_TEST(groups_active_links_and_stats)
{
    CSLSSrtlaGroups g(caps(200, 4, 8, 0), fill_counter);
    uint8_t id[SLS_SRTLA_ID_LEN];
    struct sockaddr_in l1 = addr("10.0.0.1", 1), l2 = addr("10.0.0.2", 1), l3 = addr("10.0.0.3", 1);
    uint32_t h = join(g, l1, 0, id);
    g.reg2(l2, id, 0);
    g.reg2(l3, id, 0);
    g.on_packet(l1, 1000, true, 1000);
    g.on_packet(l2, 1000, true, 2500);
    g.on_packet(l3, 50, false, 3000);      // keepalive only: the latest link, no data
    std::vector<struct sockaddr_in> act;
    g.active_links(h, 3000, 2000, &act);
    SLS_CHECK_EQ(act.size(), 2u);          // l3 (latest, always) + l2 (data 0.5 s ago); l1 is 2 s old
    SLS_CHECK(act.size() == 2 && act[0].sin_addr.s_addr == l3.sin_addr.s_addr);
    SLS_CHECK(act.size() == 2 && act[1].sin_addr.s_addr == l2.sin_addr.s_addr);
    struct sockaddr_in latest;
    SLS_CHECK(g.latest_link(h, &latest) && latest.sin_addr.s_addr == l3.sin_addr.s_addr);
    // rates: 125000 bytes in 1 s on l1 = 1000 kbit/s
    g.cleanup(0);
    for (int i = 0; i < 100; i++) g.on_packet(l1, 1250, true, 3500);
    g.cleanup(4000);
    std::vector<sls_srtla_link_stats> st;
    g.links(h, &st);
    SLS_CHECK_EQ(st.size(), 3u);
    SLS_CHECK(st.size() == 3 && st[0].addr.sin_addr.s_addr == l1.sin_addr.s_addr);   // most recent first
    SLS_CHECK(st.size() == 3 && st[0].packets == 101 && st[0].kbitrate == 252);       // (1000 + 125000) B * 8 / 4000 ms
}

SLS_TEST(groups_clear)
{
    CSLSSrtlaGroups g(caps(200, 4, 8, 0), fill_counter);
    uint32_t h = join(g, addr("10.0.0.1", 1), 0);
    g.set_fd(h, 7);
    count_events(g, SLS_SRTLA_EV_GROUP_ADDED);
    g.clear(5);
    SLS_CHECK_EQ(g.group_count(), 0u);
    SLS_CHECK_EQ(g.link_count(), 0u);
    std::vector<sls_srtla_event> ev;
    g.take_events(&ev);
    SLS_CHECK(ev.size() == 1 && ev[0].fd == 7 && 0 == strcmp(ev[0].reason, "stopped"));
}

SLS_TEST(groups_ack_every_10)
{
    CSLSSrtlaGroups g(caps(200, 4, 8, 0), fill_counter);
    uint8_t id[SLS_SRTLA_ID_LEN];
    struct sockaddr_in l1 = addr("10.0.0.1", 1), l2 = addr("10.0.0.2", 1);
    join(g, l1, 0, id);
    g.reg2(l2, id, 0);
    uint32_t ack[SLS_SRTLA_ACK_SEQS];
    std::vector<std::vector<uint32_t> > l1_acks;
    int l2_acks = 0;
    for (uint32_t s = 0; s < 25; s++) {
        bool on_l1 = (s % 5) != 0;
        if (g.record_ack_seq(on_l1 ? l1 : l2, s, ack)) {
            if (on_l1) l1_acks.push_back(std::vector<uint32_t>(ack, ack + SLS_SRTLA_ACK_SEQS));
            else l2_acks++;
        }
    }
    // l1 got 20 sequence numbers (2 ACKs of its own), l2 only 5 (none yet)
    SLS_CHECK_EQ(l1_acks.size(), 2u);
    SLS_CHECK_EQ(l2_acks, 0);
    SLS_CHECK(l1_acks.size() == 2 && l1_acks[0][0] == 1 && l1_acks[0][9] == 12);
    SLS_CHECK(l1_acks.size() == 2 && l1_acks[1][0] == 13 && l1_acks[1][9] == 24);
    SLS_CHECK(!g.record_ack_seq(addr("10.0.0.9", 1), 1, ack));   // not a link
}
