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


#ifndef _SLSSrtlaReorder_INCLUDE_
#define _SLSSrtlaReorder_INCLUDE_

#include <deque>
#include <vector>
#include <stddef.h>
#include <stdint.h>

#include "SLSSrtlaProto.hpp"

#define SLS_SRTLA_REORDER_RATE_PPS   1900    // ~20 Mbit/s of 1316-byte payloads
#define SLS_SRTLA_REORDER_MIN_SLOTS  128
#define SLS_SRTLA_REORDER_SLOT_LEN   SLS_UDP_MTU
#define SLS_SRTLA_REORDER_JUMP_CONFIRM 16    // far-ahead packets in a row that move the window

struct sls_srtla_reorder_stats {
    uint64_t passed;          // released in order without waiting
    uint64_t held;            // waited for an earlier packet (incl. released by flush)
    uint64_t reordered;       // arrived after a later sequence number (filled a gap)
    uint64_t gap_released;    // gaps given up on after the hold (SRT will NAK them)
    uint64_t late;            // behind the release point, or far ahead (unconfirmed jump): forwarded at once
    uint64_t duplicates;      // a copy of a packet already held: dropped
    uint64_t cap_released;    // released early because the window was full
    uint64_t resets;          // confirmed jumps of more than twice the window
    int      held_max_ms;     // the longest any packet waited
};

/**
 * The reorder buffer of one SRTLA group: SRT data packets from several links
 * arrive out of order; this releases them in sequence order so libsrt only
 * sees (and NAKs) real losses.
 *
 * - In-order packets are released at once (no added latency).
 * - A packet after a gap is held until the gap fills, or until the oldest
 *   held packet has waited hold_ms; then the gap is skipped and packets are
 *   released up to the next gap.
 * - Packets behind the release point (however old: retransmissions) are
 *   forwarded at once (libsrt treats them as late or duplicate); a second
 *   copy of a held packet is dropped (forwarding it would let libsrt see a
 *   gap and NAK it).
 * - Bounded: the window is slots() sequence numbers. A packet just beyond it
 *   releases early to make room. Packets more than twice the window ahead
 *   are forwarded unbuffered; SLS_SRTLA_REORDER_JUMP_CONFIRM of them in a
 *   row flush and restart there (a single forged one cannot).
 * - hold_ms 0 is pass-through.
 *
 * Pure: no sockets, no locks, time passed in. Released packets go to the
 * emit callback, in order, during push() and tick().
 */
class CSLSSrtlaReorder
{
public:
    typedef void (*emit_fn)(void *ctx, const uint8_t *pkt, int len);

    CSLSSrtlaReorder(int hold_ms, emit_fn emit, void *ctx);
    ~CSLSSrtlaReorder();

    // changes the hold (and the window); held packets are released first
    void set_hold(int hold_ms, int64_t now_ms);
    int  hold_ms() const { return m_hold_ms; }
    int  slots() const { return m_slots; }

    // the first sequence number to expect (from the SRT handshake's initial
    // sequence number); ignored once packets have been seen. Without it the
    // first data packet sets it.
    void start_at(uint32_t seq);
    // one SRT data packet (the caller checked sls_srt_is_data)
    void push(const uint8_t *pkt, int len, int64_t now_ms);
    // releases what has waited long enough; call when next_deadline() passes
    void tick(int64_t now_ms);
    // ms until tick() has work, or -1 when nothing is held
    int  next_deadline(int64_t now_ms) const;
    // releases everything held, in order, then forgets the sequence (a new SRT session)
    void flush();
    void reset();

    size_t held_count() const { return m_held; }
    const sls_srtla_reorder_stats &stats() const { return m_stats; }

private:
    struct slot {
        bool     used;
        int      len;
        int64_t  arrival_ms;
    };
    static int slots_for(int hold_ms);
    size_t index_of(int32_t offset) const { return (m_head + (size_t)offset) % (size_t)m_slots; }
    void release_run(int64_t now_ms);           // releases from m_next while slots are filled
    void skip_to_next_held(int64_t now_ms);     // skips the gap at m_next
    void emit_slot(size_t i, int64_t now_ms);
    void advance();
    bool still_held(uint32_t seq) const;
    void prune_arrivals();

    int      m_hold_ms;
    int      m_slots;
    emit_fn  m_emit;
    void    *m_ctx;
    bool     m_started;
    uint32_t m_next;        // the next sequence number to release
    uint32_t m_max_seen;
    size_t   m_head;        // slot of m_next
    size_t   m_held;
    int      m_far_ahead;   // far-ahead packets in a row
    std::vector<slot>    m_slot;
    std::vector<uint8_t> m_data;     // m_slots * SLS_SRTLA_REORDER_SLOT_LEN, allocated on first hold
    std::deque<std::pair<int64_t, uint32_t> > m_arrivals;   // (arrival, seq) of held packets, oldest first
    sls_srtla_reorder_stats m_stats;
};

#endif
