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
#include "SLSPeerMap.hpp"
#include "common.hpp"

static struct sockaddr_in addr(const char *ip, int port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}
static uint32_t ip_of(const char *ip) { return addr(ip, 0).sin_addr.s_addr; }

static size_t expire_n(CSLSPeerMap &m, int64_t now, std::vector<int> *fds = NULL)
{
    std::vector<int> local;
    m.expire(now, SLS_FD_HALFOPEN_MS, SLS_FD_IDLE_MS, fds ? fds : &local);
    return fds ? fds->size() : local.size();
}

SLS_TEST(peermap_add_lookup)
{
    CSLSPeerMap m(16, 0);
    struct sockaddr_in r = addr("1.2.3.4", 5000);
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
    SLS_CHECK_EQ(m.size(), 1);
    SLS_CHECK_EQ(m.fd_for_remote(r, 10), 7);
    struct sockaddr_in got;
    memset(&got, 0, sizeof(got));
    SLS_CHECK(m.remote_for_fd(7, &got, 10));
    SLS_CHECK_EQ(got.sin_addr.s_addr, r.sin_addr.s_addr);
    SLS_CHECK_EQ(got.sin_port, r.sin_port);
    memset(&got, 0, sizeof(got));
    SLS_CHECK(m.remote_for_local_port(40001, &got));
    SLS_CHECK_EQ(got.sin_addr.s_addr, r.sin_addr.s_addr);
    SLS_CHECK_EQ(ntohs(got.sin_port), 5000);
    SLS_CHECK_EQ(m.count_for_ip(r.sin_addr.s_addr), 1);
    SLS_CHECK_EQ(m.fd_for_remote(addr("1.2.3.4", 5001), 10), -1);
    SLS_CHECK(!m.remote_for_fd(8, &got, 10));
    SLS_CHECK(!m.remote_for_local_port(40002, &got));
}

SLS_TEST(peermap_duplicate)
{
    CSLSPeerMap m(16, 0);
    int ev;
    SLS_CHECK_EQ(m.add(addr("1.2.3.4", 5000), 7, 40001, 0), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("1.2.3.4", 5000), 8, 40002, 0, &ev), SLS_ERROR);   // same remote
    SLS_CHECK_EQ(m.add(addr("1.2.3.4", 5001), 7, 40002, 0, &ev), SLS_ERROR);   // same fd
    SLS_CHECK_EQ(m.add(addr("1.2.3.4", 5001), 8, 40001, 0, &ev), SLS_ERROR);   // same local port
    SLS_CHECK_EQ(ev, -1);
    SLS_CHECK_EQ(m.size(), 1);
    SLS_CHECK_EQ(m.count_for_ip(ip_of("1.2.3.4")), 1);
}

SLS_TEST(peermap_global_cap)
{
    CSLSPeerMap m(3, 0);
    SLS_CHECK_EQ(m.add(addr("1.1.1.1", 1), 10, 40010, 0), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("1.1.1.2", 1), 11, 40011, 0), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("1.1.1.3", 1), 12, 40012, 0), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("1.1.1.4", 1), 13, 40013, 0), SLS_ERROR);   // no eviction asked for
    SLS_CHECK_EQ(m.size(), 3);
    m.remove_fd(11);
    SLS_CHECK_EQ(m.add(addr("1.1.1.4", 1), 13, 40013, 0), SLS_OK);   // a slot freed
}

SLS_TEST(peermap_per_ip_cap)
{
    CSLSPeerMap m(16, 2);
    SLS_CHECK_EQ(m.add(addr("2.2.2.2", 1), 10, 40010, 0), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("2.2.2.2", 2), 11, 40011, 0), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("2.2.2.2", 3), 12, 40012, 0), SLS_ERROR);
    SLS_CHECK_EQ(m.add(addr("3.3.3.3", 1), 12, 40012, 0), SLS_OK);
    SLS_CHECK_EQ(m.count_for_ip(ip_of("2.2.2.2")), 2);
    SLS_CHECK_EQ(m.count_for_ip(ip_of("3.3.3.3")), 1);
    SLS_CHECK_EQ(m.count_for_ip(ip_of("4.4.4.4")), 0);
}

SLS_TEST(peermap_halfopen_expiry)
{
    CSLSPeerMap m(16, 0);
    SLS_CHECK_EQ(m.add(addr("1.2.3.4", 5000), 7, 40001, 0), SLS_OK);
    std::vector<int> fds;
    SLS_CHECK_EQ(expire_n(m, 4999, &fds), 0);
    SLS_CHECK_EQ(expire_n(m, 5001, &fds), 1);
    if (fds.size() == 1) SLS_CHECK_EQ(fds[0], 7);
    SLS_CHECK_EQ(m.size(), 0);
    SLS_CHECK_EQ(m.count_for_ip(ip_of("1.2.3.4")), 0);
    struct sockaddr_in got;
    SLS_CHECK(!m.remote_for_local_port(40001, &got));
}

SLS_TEST(peermap_remote_packets_do_not_establish)
{
    // a spoofed source can send any number of packets: only libsrt's answer counts
    CSLSPeerMap m(16, 0);
    struct sockaddr_in r = addr("1.2.3.4", 5000);
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
    for (int t = 100; t < 5000; t += 100) m.fd_for_remote(r, t);
    struct sockaddr_in got;
    SLS_CHECK(m.remote_for_fd(7, &got, 1000));   // handshake replies: not confirming
    SLS_CHECK_EQ(expire_n(m, 5001), 1);
}

SLS_TEST(peermap_established_survives)
{
    CSLSPeerMap m(16, 0);
    struct sockaddr_in r = addr("1.2.3.4", 5000);
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
    SLS_CHECK(m.remote_for_fd(7, NULL, 100, true));   // libsrt sent data/ACK: routable
    SLS_CHECK_EQ(m.fd_for_remote(r, 200), 7);
    SLS_CHECK_EQ(expire_n(m, 6000), 0);
    SLS_CHECK_EQ(m.size(), 1);
    SLS_CHECK_EQ(expire_n(m, 200 + SLS_FD_IDLE_MS - 1), 0);
    SLS_CHECK_EQ(expire_n(m, 200 + SLS_FD_IDLE_MS + 1), 1);
    SLS_CHECK_EQ(m.size(), 0);
}

SLS_TEST(peermap_outbound_does_not_keep_alive)
{
    // libsrt retransmitting to a vanished peer keeps nothing alive
    CSLSPeerMap m(16, 0);
    struct sockaddr_in r = addr("1.2.3.4", 5000);
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
    m.remote_for_fd(7, NULL, 100, true);
    m.fd_for_remote(r, 200);
    for (int t = 1000; t < 20000; t += 1000) m.remote_for_fd(7, NULL, t, true);
    SLS_CHECK_EQ(expire_n(m, 200 + SLS_FD_IDLE_MS + 1), 1);
}

SLS_TEST(peermap_evicts_a_halfopen_one)
{
    CSLSPeerMap m(3, 0);
    int ev = 0;
    SLS_CHECK_EQ(m.add(addr("1.1.1.1", 1), 10, 40010, 0), SLS_OK);
    m.remote_for_fd(10, NULL, 50, true);                              // established, active
    m.fd_for_remote(addr("1.1.1.1", 1), 900);
    SLS_CHECK_EQ(m.add(addr("6.6.6.1", 1), 11, 40011, 100), SLS_OK);   // half-open (older)
    SLS_CHECK_EQ(m.add(addr("6.6.6.2", 1), 12, 40012, 200), SLS_OK);   // half-open
    SLS_CHECK_EQ(m.add(addr("1.1.1.9", 1), 13, 40013, 1000, &ev), SLS_OK);
    SLS_CHECK(ev == 11 || ev == 12);                                   // one of the half-open ones
    SLS_CHECK_EQ(m.size(), 3);
    SLS_CHECK_EQ(m.fd_for_remote(addr("1.1.1.1", 1), 1000), 10);       // the live peer stays
}

SLS_TEST(peermap_evicts_quiet_established_never_active)
{
    CSLSPeerMap m(2, 0);
    int ev = 0;
    SLS_CHECK_EQ(m.add(addr("1.1.1.1", 1), 10, 40010, 0), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("1.1.1.2", 1), 11, 40011, 0), SLS_OK);
    m.remote_for_fd(10, NULL, 10, true);
    m.remote_for_fd(11, NULL, 10, true);
    m.fd_for_remote(addr("1.1.1.1", 1), 1000);   // quiet since 1000
    m.fd_for_remote(addr("1.1.1.2", 1), 5500);   // active
    // at 5600 neither is quiet for SLS_FD_QUIET_MS: nothing to evict
    SLS_CHECK_EQ(m.add(addr("1.1.1.3", 1), 12, 40012, 5600, &ev), SLS_ERROR);
    SLS_CHECK_EQ(ev, -1);
    // at 6100, 1.1.1.1 has been silent for 5100 ms
    SLS_CHECK_EQ(m.add(addr("1.1.1.3", 1), 12, 40012, 6100, &ev), SLS_OK);
    SLS_CHECK_EQ(ev, 10);
}

SLS_TEST(peermap_per_ip_eviction_stays_in_ip)
{
    // a flood spoofing one victim IP can only evict that IP's own half-open entries
    CSLSPeerMap m(16, 2);
    int ev = 0;
    SLS_CHECK_EQ(m.add(addr("6.6.6.6", 1), 10, 40010, 0), SLS_OK);   // other IP, half-open
    SLS_CHECK_EQ(m.add(addr("2.2.2.2", 1), 11, 40011, 10), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("2.2.2.2", 2), 12, 40012, 20), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("2.2.2.2", 3), 13, 40013, 30, &ev), SLS_OK);
    SLS_CHECK(ev == 11 || ev == 12);
    SLS_CHECK_EQ(m.count_for_ip(ip_of("2.2.2.2")), 2);
    SLS_CHECK_EQ(m.fd_for_remote(addr("6.6.6.6", 1), 40), 10);
}

SLS_TEST(peermap_peer_shutdown_undone_by_traffic)
{
    // a forged shutdown "from" a live peer: its next real packet cancels it
    CSLSPeerMap m(16, 0);
    struct sockaddr_in r = addr("1.2.3.4", 5000);
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
    m.remote_for_fd(7, NULL, 50, true);
    m.fd_for_remote(r, 1000, true);
    m.fd_for_remote(r, 1500);
    SLS_CHECK_EQ(expire_n(m, 4000), 0);
    // a real one: nothing follows it
    m.fd_for_remote(r, 5000, true);
    SLS_CHECK_EQ(expire_n(m, 5000 + SLS_FD_CLOSING_MS - 1), 0);
    SLS_CHECK_EQ(expire_n(m, 5000 + SLS_FD_CLOSING_MS + 1), 1);
}

SLS_TEST(peermap_libsrt_shutdown_final)
{
    CSLSPeerMap m(16, 0);
    struct sockaddr_in r = addr("1.2.3.4", 5000);
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
    m.remote_for_fd(7, NULL, 50, true);
    m.remote_for_fd(7, NULL, 1000, false, true);   // libsrt closed the session
    m.fd_for_remote(r, 1500);                     // the peer still sends: changes nothing
    m.remote_for_fd(7, NULL, 1800, false, true);  // the first mark counts
    SLS_CHECK_EQ(expire_n(m, 1000 + SLS_FD_CLOSING_MS - 1), 0);
    SLS_CHECK_EQ(expire_n(m, 1000 + SLS_FD_CLOSING_MS + 1), 1);
}

SLS_TEST(peermap_remove_fd)
{
    CSLSPeerMap m(16, 2);
    struct sockaddr_in r = addr("1.2.3.4", 5000);
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
    m.remove_fd(7);
    struct sockaddr_in got;
    SLS_CHECK_EQ(m.fd_for_remote(r, 10), -1);
    SLS_CHECK(!m.remote_for_fd(7, &got, 10));
    SLS_CHECK(!m.remote_for_local_port(40001, &got));
    SLS_CHECK_EQ(m.size(), 0);
    SLS_CHECK_EQ(m.count_for_ip(r.sin_addr.s_addr), 0);
    m.remove_fd(7);   // twice is harmless
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
}

SLS_TEST(peermap_evicts_ended_sessions_first)
{
    // a client retrying in a loop: each try is established, then refused and shut down by libsrt
    CSLSPeerMap m(16, 2);
    int ev = 0;
    SLS_CHECK_EQ(m.add(addr("2.2.2.2", 1), 10, 40010, 0), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("2.2.2.2", 2), 11, 40011, 100), SLS_OK);   // half-open, newer
    m.remote_for_fd(10, NULL, 50, true);
    m.fd_for_remote(addr("2.2.2.2", 1), 200);
    m.remote_for_fd(10, NULL, 300, false, true);                       // libsrt shut it down
    SLS_CHECK_EQ(m.add(addr("2.2.2.2", 3), 12, 40012, 400, &ev), SLS_OK);
    SLS_CHECK_EQ(ev, 10);                                              // not the half-open one
    SLS_CHECK_EQ(m.fd_for_remote(addr("2.2.2.2", 2), 500), 11);
}

SLS_TEST(peermap_random_victim_spreads)
{
    // a flood keeps evicting: a given half-open entry is not always the one hit
    CSLSPeerMap m(4, 0);
    int hits[4] = {0, 0, 0, 0};
    for (int round = 0; round < 400; round++) {
        CSLSPeerMap mm(4, 0);
        for (int i = 0; i < 4; i++) mm.add(addr("9.9.9.9", 100 + i), 10 + i, 40000 + i, i);
        int ev = -1;
        // a different instance each round still seeds the same way: vary the history
        for (int k = 0; k < round % 7; k++) {
            int e2 = -1;
            mm.add(addr("8.8.8.8", 1000 + k), 100 + k, 41000 + k, 10 + k, &e2);
        }
        if (SLS_OK == mm.add(addr("7.7.7.7", 1), 99, 42000, 50, &ev) && ev >= 10 && ev < 14) hits[ev - 10]++;
    }
    int spread = 0;
    for (int i = 0; i < 4; i++) if (hits[i] > 0) spread++;
    SLS_CHECK(spread >= 3);
    (void)m;
}

SLS_TEST(peermap_reconnect_after_shutdown)
{
    // a client on a fixed local port reconnects right after libsrt ended its session
    CSLSPeerMap m(16, 0);
    struct sockaddr_in r = addr("1.2.3.4", 5000);
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
    m.remote_for_fd(7, NULL, 50, true);
    m.remote_for_fd(7, NULL, 1000, false, true);   // libsrt shut the old session down
    SLS_CHECK_EQ(m.fd_for_remote(r, 1500, false, true), 7);   // a new handshake
    m.remote_for_fd(7, NULL, 1600, true);          // the new session runs
    m.fd_for_remote(r, 2500);
    SLS_CHECK_EQ(expire_n(m, 3500), 0);           // the old shutdown no longer ends it
    SLS_CHECK_EQ(m.size(), 1);
}
