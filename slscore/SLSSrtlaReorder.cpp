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

#include "SLSSrtlaReorder.hpp"

CSLSSrtlaReorder::CSLSSrtlaReorder(int hold_ms, emit_fn emit, void *ctx)
{
    m_hold_ms = hold_ms < 0 ? 0 : hold_ms;
    m_slots   = slots_for(m_hold_ms);
    m_emit    = emit;
    m_ctx     = ctx;
    m_started = false;
    m_next    = 0;
    m_max_seen = 0;
    m_head    = 0;
    m_held    = 0;
    m_far_ahead = 0;
    m_slot.assign(m_slots, slot());
    for (size_t i = 0; i < m_slot.size(); i++) {
        m_slot[i].used = false;
    }
    memset(&m_stats, 0, sizeof(m_stats));
}

CSLSSrtlaReorder::~CSLSSrtlaReorder()
{
}

int CSLSSrtlaReorder::slots_for(int hold_ms)
{
    // the sequence numbers ~20 Mbit/s covers in the hold, with 50 % to spare
    int64_t n = (int64_t)SLS_SRTLA_REORDER_RATE_PPS * hold_ms / 1000 * 3 / 2;
    return n < SLS_SRTLA_REORDER_MIN_SLOTS ? SLS_SRTLA_REORDER_MIN_SLOTS : (int)n;
}

void CSLSSrtlaReorder::set_hold(int hold_ms, int64_t now_ms)
{
    (void)now_ms;
    if (hold_ms < 0) {
        hold_ms = 0;
    }
    if (hold_ms == m_hold_ms) {
        return;
    }
    flush();
    m_hold_ms = hold_ms;
    m_slots   = slots_for(hold_ms);
    m_slot.assign(m_slots, slot());
    for (size_t i = 0; i < m_slot.size(); i++) {
        m_slot[i].used = false;
    }
    std::vector<uint8_t>().swap(m_data);
}

void CSLSSrtlaReorder::start_at(uint32_t seq)
{
    if (m_started) {
        return;
    }
    m_started  = true;
    m_next     = seq & SLS_SRT_SEQ_MASK;
    m_max_seen = sls_srt_seq_add(m_next, -1);
    m_head     = 0;
}

void CSLSSrtlaReorder::advance()
{
    m_head = (m_head + 1) % (size_t)m_slots;
    m_next = sls_srt_seq_add(m_next, 1);
}

void CSLSSrtlaReorder::emit_slot(size_t i, int64_t now_ms)
{
    slot &s = m_slot[i];
    m_emit(m_ctx, &m_data[i * SLS_SRTLA_REORDER_SLOT_LEN], s.len);
    s.used = false;
    m_held--;
    m_stats.held++;
    int waited = (int)(now_ms - s.arrival_ms);
    if (waited > m_stats.held_max_ms) {
        m_stats.held_max_ms = waited;
    }
}

bool CSLSSrtlaReorder::still_held(uint32_t seq) const
{
    int32_t off = sls_srt_seq_diff(m_next, seq);
    return off >= 0 && off < m_slots && m_slot[index_of(off)].used;
}

void CSLSSrtlaReorder::prune_arrivals()
{
    while (!m_arrivals.empty() && !still_held(m_arrivals.front().second)) {
        m_arrivals.pop_front();
    }
    if (0 == m_held) {
        m_arrivals.clear();
    } else if (m_arrivals.size() > 4 * (size_t)m_slots) {
        // released packets behind a long-held one: rebuild from the slots
        std::deque<std::pair<int64_t, uint32_t> > keep;
        for (size_t k = 0; k < m_arrivals.size(); k++) {
            if (still_held(m_arrivals[k].second)) {
                keep.push_back(m_arrivals[k]);
            }
        }
        m_arrivals.swap(keep);
    }
}

void CSLSSrtlaReorder::release_run(int64_t now_ms)
{
    while (m_held > 0 && m_slot[m_head].used) {
        emit_slot(m_head, now_ms);
        advance();
    }
    prune_arrivals();
}

void CSLSSrtlaReorder::skip_to_next_held(int64_t now_ms)
{
    (void)now_ms;
    while (m_held > 0 && !m_slot[m_head].used) {
        advance();
    }
}

void CSLSSrtlaReorder::push(const uint8_t *pkt, int len, int64_t now_ms)
{
    if (0 == m_hold_ms || len > SLS_SRTLA_REORDER_SLOT_LEN || len < SLS_SRT_HDR_LEN) {
        m_stats.passed++;
        m_emit(m_ctx, pkt, len);
        return;
    }
    uint32_t seq = sls_srt_data_seq(pkt);
    if (!m_started) {
        start_at(seq);
    }
    int32_t d = sls_srt_seq_diff(m_next, seq);
    if (d < 0) {
        // behind the release point, however far (an SRT retransmission can be
        // up to the latency old): forwarded at once, never a reset. A new SRT
        // session gets flush() from the caller.
        m_stats.late++;
        m_emit(m_ctx, pkt, len);
        return;
    }
    if (d >= 2 * m_slots) {
        // far ahead: a long outage, or a forged sequence number. Forward it
        // unbuffered; only SLS_SRTLA_REORDER_JUMP_CONFIRM in a row move the
        // window (one packet cannot flush the buffer).
        if (++m_far_ahead < SLS_SRTLA_REORDER_JUMP_CONFIRM) {
            m_stats.late++;
            m_emit(m_ctx, pkt, len);
            return;
        }
        flush();
        m_stats.resets++;
        start_at(seq);
        d = 0;
    }
    m_far_ahead = 0;
    if (sls_srt_seq_diff(m_max_seen, seq) < 0) {
        m_stats.reordered++;
    } else {
        m_max_seen = seq;
    }
    if (d >= m_slots) {
        // just beyond the window: release (and skip gaps) until it fits
        bool released = false;
        while (d >= m_slots) {
            if (m_slot[m_head].used) {
                emit_slot(m_head, now_ms);
                released = true;
            }
            advance();
            d--;
        }
        if (released) {
            m_stats.cap_released++;
        }
        release_run(now_ms);
        d = sls_srt_seq_diff(m_next, seq);
    }
    if (0 == d) {
        m_stats.passed++;
        m_emit(m_ctx, pkt, len);
        advance();
        release_run(now_ms);
        return;
    }
    size_t i = index_of(d);
    if (m_slot[i].used) {
        m_stats.duplicates++;
        return;
    }
    if (m_data.empty()) {
        m_data.resize((size_t)m_slots * SLS_SRTLA_REORDER_SLOT_LEN);
    }
    memcpy(&m_data[i * SLS_SRTLA_REORDER_SLOT_LEN], pkt, len);
    m_slot[i].used       = true;
    m_slot[i].len        = len;
    m_slot[i].arrival_ms = now_ms;
    m_held++;
    m_arrivals.push_back(std::make_pair(now_ms, seq));
}

int CSLSSrtlaReorder::next_deadline(int64_t now_ms) const
{
    if (0 == m_held) {
        return -1;
    }
    for (size_t k = 0; k < m_arrivals.size(); k++) {
        if (still_held(m_arrivals[k].second)) {
            int64_t left = m_arrivals[k].first + m_hold_ms - now_ms;
            return left < 0 ? 0 : (int)left;
        }
    }
    return 0;
}

void CSLSSrtlaReorder::tick(int64_t now_ms)
{
    while (m_held > 0) {
        prune_arrivals();
        if (m_arrivals.empty() || m_arrivals.front().first + m_hold_ms > now_ms) {
            break;
        }
        // the oldest held packet has waited long enough: give up on the gap
        skip_to_next_held(now_ms);
        m_stats.gap_released++;
        release_run(now_ms);
    }
}

void CSLSSrtlaReorder::flush()
{
    // release everything held, in order, without counting gaps
    while (m_held > 0) {
        while (!m_slot[m_head].used) {
            advance();
        }
        while (m_held > 0 && m_slot[m_head].used) {
            slot &s = m_slot[m_head];
            m_emit(m_ctx, &m_data[m_head * SLS_SRTLA_REORDER_SLOT_LEN], s.len);
            s.used = false;
            m_held--;
            m_stats.held++;
            advance();
        }
    }
    m_arrivals.clear();
    m_started = false;
    m_head    = 0;
    m_far_ahead = 0;
}

void CSLSSrtlaReorder::reset()
{
    for (size_t i = 0; i < m_slot.size(); i++) {
        m_slot[i].used = false;
    }
    m_held = 0;
    m_arrivals.clear();
    m_started = false;
    m_head    = 0;
    m_far_ahead = 0;
}
