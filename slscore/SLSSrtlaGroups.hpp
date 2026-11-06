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


#ifndef _SLSSrtlaGroups_INCLUDE_
#define _SLSSrtlaGroups_INCLUDE_

#include <map>
#include <string>
#include <vector>
#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

#include "SLSSrtlaProto.hpp"

#define SLS_SRTLA_LINK_TIMEOUT_MS    10000   // a link silent this long is removed
#define SLS_SRTLA_GROUP_EMPTY_MS     10000   // a group without links this long is removed
#define SLS_SRTLA_HANDSHAKE_MS       10000   // a group without an SRT handshake this long is removed
#define SLS_SRTLA_CLEANUP_MS         3000    // how often the owner calls cleanup()
#define SLS_SRTLA_RATE_WINDOW_MS     1000    // registrations per IP are counted per window
#define SLS_SRTLA_RATE_SLOTS         4096    // the rate table: a keyed hash of the IP picks a slot
#define SLS_SRTLA_REG_VALID_MS       10000   // a REG1 answer (our id half) is good for this long
#define SLS_SRTLA_LINK_MOVE_QUIET_MS 3000    // a link moves to another group only after this much silence

struct sls_srtla_caps {
    int max_groups;            // srtla_max_groups (200)
    int max_groups_per_ip;     // srtla_max_groups_per_ip (4), by the registering (REG1) IP
    int max_links_per_group;   // srtla_max_links_per_group (8)
    int reg_per_ip_per_sec;    // srtla_reg_per_ip_per_sec (5), REG1 and REG2 together
};

// what to answer a registration with
enum sls_srtla_reg_result {
    SLS_SRTLA_REG_OK,        // REG1: answer REG2 with the full id; REG2: answer REG3
    SLS_SRTLA_REG_ERR,       // temporary refusal (a cap or the rate): answer REG_ERR
    SLS_SRTLA_REG_NGP,       // REG2 with an id we don't know: answer REG_NGP
};

enum sls_srtla_event_type {
    SLS_SRTLA_EV_GROUP_ADDED,
    SLS_SRTLA_EV_GROUP_REMOVED,
    SLS_SRTLA_EV_LINK_ADDED,
    SLS_SRTLA_EV_LINK_REMOVED,
};

struct sls_srtla_event {
    sls_srtla_event_type type;
    uint32_t           group;    // the group's handle
    struct sockaddr_in addr;     // the link (link events) or the registering address (group events)
    const char        *reason;   // static string: "registered", "timed out", "no links", "no handshake", "moved", "stopped"
    int                fd;       // GROUP_REMOVED: the group's SRT socket (or -1); the owner closes it
    int64_t            duration_ms;  // GROUP_REMOVED: how long it lived
    int                peak_links;   // GROUP_REMOVED
};

struct sls_srtla_link_stats {
    struct sockaddr_in addr;
    uint64_t packets;
    uint64_t bytes;
    int64_t  last_ms;
    int      kbitrate;    // over the last cleanup interval
};

/**
 * The SRTLA groups of one front door and their links (srtla2 registration).
 * Pure bookkeeping: no sockets, no locks (the front door thread owns it), no
 * logging; times are monotonic ms passed in; the random half of every group
 * id comes from the fill function given (getrandom() in production).
 *
 * A group is named by its full 256-byte id: the sender's 128 bytes from REG1
 * plus 128 bytes of ours. REG1 is stateless: our half carries the REG1
 * source, a timestamp, random bytes and a keyed MAC (SipHash) over all of
 * it and the sender's half. The group is created by the first REG2 whose id
 * verifies and is at most SLS_SRTLA_REG_VALID_MS old, so only a REG1 source
 * that could receive our answer ever costs state (a spoofed REG1 flood
 * costs nothing but the answers). The group's identity is that REG1 source.
 * A link is its source address and port; a source belongs to at most one
 * group (a REG2 for another group moves it, once it has been silent for
 * SLS_SRTLA_LINK_MOVE_QUIET_MS: a forged REG2 cannot steal a live link).
 * The registration rate per IP uses a fixed table indexed by a keyed hash
 * (O(1), no allocation; IPs that collide share a budget).
 */
class CSLSSrtlaGroups
{
public:
    typedef void (*rand_fill_fn)(uint8_t *buf, size_t len);

    CSLSSrtlaGroups(const sls_srtla_caps &caps, rand_fill_fn rand_fill);
    ~CSLSSrtlaGroups();

    // REG1 from src with the sender's id (256 bytes). On OK, full_id_out (256
    // bytes) gets the id to answer REG2 with. Creates nothing.
    sls_srtla_reg_result reg1(const struct sockaddr_in &src, const uint8_t *sender_id,
                              int64_t now_ms, uint8_t *full_id_out);
    // REG2 from src with a full id. On OK the source is a link of that group.
    sls_srtla_reg_result reg2(const struct sockaddr_in &src, const uint8_t *full_id, int64_t now_ms);

    // the group src is a link of (0 if none); counts the packet for that link.
    // data: an SRT data packet (counts towards the link's recent data).
    uint32_t on_packet(const struct sockaddr_in &src, int len, bool data, int64_t now_ms);
    uint32_t group_of(const struct sockaddr_in &src) const;
    // collects a data sequence number that arrived on link src; when
    // SLS_SRTLA_ACK_SEQS are collected, copies them to ack_out and returns
    // true (send that link an SRTLA ACK)
    bool record_ack_seq(const struct sockaddr_in &src, uint32_t seq, uint32_t *ack_out);

    // the group's SRT session has been seen (an SRT handshake through it)
    void mark_handshake(uint32_t group);
    // the group's SRT socket (to the loopback listener), -1 for none; returns the previous one
    int  set_fd(uint32_t group, int fd);
    int  fd_of(uint32_t group) const;
    bool registering_addr(uint32_t group, struct sockaddr_in *addr) const;

    // the links of a group, most recently active first
    void links(uint32_t group, std::vector<sls_srtla_link_stats> *out) const;
    // where an SRT ACK goes: the most recently active link always, plus every
    // link that delivered data within window_ms AND since the previous call
    // (a forged link gets at most one ACK per forged packet); clears the marks
    void fanout_links(uint32_t group, int64_t now_ms, int64_t window_ms,
                      std::vector<struct sockaddr_in> *out);
    // the most recently active link of the group
    bool latest_link(uint32_t group, struct sockaddr_in *out) const;

    // removes timed-out links and groups; call every SLS_SRTLA_CLEANUP_MS
    void cleanup(int64_t now_ms);
    // removes everything (reason "stopped")
    void clear(int64_t now_ms);
    // events since the last call (the owner logs them and closes fds)
    void take_events(std::vector<sls_srtla_event> *out);

    size_t group_count() const { return m_groups.size(); }
    size_t link_count() const { return m_links.size(); }
    int    groups_for_ip(uint32_t ip_be) const;

private:
    struct link {
        struct sockaddr_in addr;
        uint64_t packets;
        uint64_t bytes;
        int64_t  last_ms;
        int64_t  last_data_ms;
        uint64_t rate_bytes;     // bytes at the last rate sample
        int64_t  rate_ms;
        int      kbitrate;
        uint32_t ack_seqs[SLS_SRTLA_ACK_SEQS];
        int      ack_n;
        bool     data_since_fanout;
    };
    struct group {
        uint8_t  id[SLS_SRTLA_ID_LEN];
        struct sockaddr_in reg_addr;
        int64_t  created_ms;
        int64_t  empty_since_ms;   // when it last had no links
        bool     handshake;
        int      fd;
        int      peak_links;
        std::vector<link> links;
    };
    typedef std::map<uint32_t, group> groups_t;

    static uint64_t key_of(const struct sockaddr_in &a);
    bool rate_ok(uint32_t ip_be, int64_t now_ms);
    void remove_group(groups_t::iterator it, const char *reason, int64_t now_ms);
    void id_mac(const uint8_t *id, uint8_t *mac16) const;
    void remove_link(group &g, uint32_t handle, size_t i, const char *reason);
    void event(sls_srtla_event_type type, uint32_t handle, const struct sockaddr_in &addr,
               const char *reason);
    link *find_link(const struct sockaddr_in &src, uint32_t *handle);

    sls_srtla_caps m_caps;
    rand_fill_fn   m_rand_fill;
    uint8_t        m_key[32];      // two SipHash keys: the id MAC, the rate table
    uint32_t       m_next_handle;
    groups_t       m_groups;
    std::map<std::string, uint32_t> m_by_id;
    std::map<uint64_t, uint32_t>    m_links;      // link source -> group
    std::map<uint32_t, int>         m_per_ip;     // groups per registering IP
    struct rate { int64_t window_ms; int count; };
    rate                            m_rate[SLS_SRTLA_RATE_SLOTS];
    std::vector<sls_srtla_event>    m_events;
};

#endif
