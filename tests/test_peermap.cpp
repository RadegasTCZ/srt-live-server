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
    SLS_CHECK_EQ(m.add(addr("1.2.3.4", 5000), 7, 40001, 0), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("1.2.3.4", 5000), 8, 40002, 0), SLS_ERROR);   // same remote
    SLS_CHECK_EQ(m.add(addr("1.2.3.4", 5001), 7, 40002, 0), SLS_ERROR);   // same fd
    SLS_CHECK_EQ(m.add(addr("1.2.3.4", 5001), 8, 40001, 0), SLS_ERROR);   // same local port
    SLS_CHECK_EQ(m.size(), 1);
    SLS_CHECK_EQ(m.count_for_ip(addr("1.2.3.4", 0).sin_addr.s_addr), 1);
}

SLS_TEST(peermap_global_cap)
{
    CSLSPeerMap m(3, 0);
    SLS_CHECK_EQ(m.add(addr("1.1.1.1", 1), 10, 40010, 0), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("1.1.1.2", 1), 11, 40011, 0), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("1.1.1.3", 1), 12, 40012, 0), SLS_OK);
    SLS_CHECK_EQ(m.add(addr("1.1.1.4", 1), 13, 40013, 0), SLS_ERROR);
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
    SLS_CHECK_EQ(m.count_for_ip(addr("2.2.2.2", 0).sin_addr.s_addr), 2);
    SLS_CHECK_EQ(m.count_for_ip(addr("3.3.3.3", 0).sin_addr.s_addr), 1);
    SLS_CHECK_EQ(m.count_for_ip(addr("4.4.4.4", 0).sin_addr.s_addr), 0);
}

SLS_TEST(peermap_halfopen_expiry)
{
    CSLSPeerMap m(16, 0);
    SLS_CHECK_EQ(m.add(addr("1.2.3.4", 5000), 7, 40001, 0), SLS_OK);
    std::vector<int> fds;
    m.expire(4999, SLS_FD_HALFOPEN_MS, SLS_FD_IDLE_MS, &fds);
    SLS_CHECK_EQ(fds.size(), 0);
    m.expire(5001, SLS_FD_HALFOPEN_MS, SLS_FD_IDLE_MS, &fds);
    SLS_CHECK_EQ(fds.size(), 1);
    if (fds.size() == 1) SLS_CHECK_EQ(fds[0], 7);
    SLS_CHECK_EQ(m.size(), 0);
    SLS_CHECK_EQ(m.count_for_ip(addr("1.2.3.4", 0).sin_addr.s_addr), 0);
    struct sockaddr_in got;
    SLS_CHECK(!m.remote_for_local_port(40001, &got));
}

SLS_TEST(peermap_halfopen_reply_is_not_enough)
{
    // replies from the listener do not establish: only a second packet from the remote does
    CSLSPeerMap m(16, 0);
    struct sockaddr_in r = addr("1.2.3.4", 5000);
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
    struct sockaddr_in got;
    SLS_CHECK(m.remote_for_fd(7, &got, 1000));
    std::vector<int> fds;
    m.expire(5001, SLS_FD_HALFOPEN_MS, SLS_FD_IDLE_MS, &fds);
    SLS_CHECK_EQ(fds.size(), 1);
}

SLS_TEST(peermap_established_survives)
{
    CSLSPeerMap m(16, 0);
    struct sockaddr_in r = addr("1.2.3.4", 5000);
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
    SLS_CHECK_EQ(m.fd_for_remote(r, 100), 7);   // the handshake that created it is not counted here
    SLS_CHECK_EQ(m.fd_for_remote(r, 200), 7);
    std::vector<int> fds;
    m.expire(6000, SLS_FD_HALFOPEN_MS, SLS_FD_IDLE_MS, &fds);
    SLS_CHECK_EQ(fds.size(), 0);
    SLS_CHECK_EQ(m.size(), 1);
    m.expire(30300, SLS_FD_HALFOPEN_MS, SLS_FD_IDLE_MS, &fds);
    SLS_CHECK_EQ(fds.size(), 1);
    SLS_CHECK_EQ(m.size(), 0);
}

SLS_TEST(peermap_outbound_keeps_alive)
{
    // traffic in either direction counts as activity
    CSLSPeerMap m(16, 0);
    struct sockaddr_in r = addr("1.2.3.4", 5000);
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
    m.fd_for_remote(r, 100);
    m.fd_for_remote(r, 200);
    struct sockaddr_in got;
    m.remote_for_fd(7, &got, 25000);
    std::vector<int> fds;
    m.expire(30300, SLS_FD_HALFOPEN_MS, SLS_FD_IDLE_MS, &fds);
    SLS_CHECK_EQ(fds.size(), 0);
    m.expire(55001, SLS_FD_HALFOPEN_MS, SLS_FD_IDLE_MS, &fds);
    SLS_CHECK_EQ(fds.size(), 1);
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

SLS_TEST(peermap_closing_expires_soon)
{
    CSLSPeerMap m(16, 0);
    struct sockaddr_in r = addr("1.2.3.4", 5000);
    SLS_CHECK_EQ(m.add(r, 7, 40001, 0), SLS_OK);
    m.fd_for_remote(r, 100);
    m.mark_closing(7, 1000);
    m.fd_for_remote(r, 1500);           // a retransmitted shutdown does not revive it
    m.mark_closing(7, 1800);            // the first mark counts
    std::vector<int> fds;
    m.expire(2900, SLS_FD_HALFOPEN_MS, SLS_FD_IDLE_MS, &fds);
    SLS_CHECK_EQ(fds.size(), 0);
    m.expire(3001, SLS_FD_HALFOPEN_MS, SLS_FD_IDLE_MS, &fds);
    SLS_CHECK_EQ(fds.size(), 1);
    m.mark_closing(99, 1000);           // unknown fd: harmless
}
