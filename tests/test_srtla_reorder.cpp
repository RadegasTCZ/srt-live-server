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
#include <vector>
#include "sls_test.hpp"
#include "SLSSrtlaReorder.hpp"

// collects the sequence numbers released
static void collect(void *ctx, const uint8_t *pkt, int len)
{
    std::vector<uint32_t> *out = (std::vector<uint32_t> *)ctx;
    out->push_back(len >= 16 ? sls_srt_data_seq(pkt) : 0xFFFFFFFFu);
}

static void push(CSLSSrtlaReorder &r, uint32_t seq, int64_t now, int len = 1332)
{
    uint8_t p[1500];
    memset(p, 0x47, sizeof(p));
    p[0] = (uint8_t)((seq >> 24) & 0x7F); p[1] = (uint8_t)(seq >> 16); p[2] = (uint8_t)(seq >> 8); p[3] = (uint8_t)seq;
    r.push(p, len, now);
}

static bool seqs_are(const std::vector<uint32_t> &got, const uint32_t *want, size_t n)
{
    if (got.size() != n) return false;
    for (size_t i = 0; i < n; i++) if (got[i] != want[i]) return false;
    return true;
}

SLS_TEST(reorder_in_order_no_delay)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(100, collect, &out);
    for (uint32_t s = 1000; s < 1100; s++) push(r, s, 0);
    SLS_CHECK_EQ(out.size(), 100u);
    SLS_CHECK_EQ(r.held_count(), 0u);
    SLS_CHECK_EQ(r.next_deadline(0), -1);
    SLS_CHECK_EQ(r.stats().passed, 100u);
    SLS_CHECK_EQ(r.stats().held, 0u);
}

SLS_TEST(reorder_gap_filled_late)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(100, collect, &out);
    push(r, 10, 0);
    push(r, 12, 1);
    push(r, 13, 2);
    SLS_CHECK_EQ(out.size(), 1u);          // 12 and 13 wait for 11
    SLS_CHECK_EQ(r.held_count(), 2u);
    SLS_CHECK_EQ(r.next_deadline(50), 51); // 12 arrived at 1, hold 100
    push(r, 11, 30);
    uint32_t want[] = {10, 11, 12, 13};
    SLS_CHECK(seqs_are(out, want, 4));
    SLS_CHECK_EQ(r.stats().reordered, 1u);
    SLS_CHECK_EQ(r.stats().held, 2u);
    SLS_CHECK_EQ(r.stats().held_max_ms, 29);
    SLS_CHECK_EQ(r.stats().gap_released, 0u);
}

SLS_TEST(reorder_gap_released_after_hold)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(100, collect, &out);
    push(r, 10, 0);
    push(r, 12, 10);
    push(r, 13, 20);
    push(r, 15, 30);       // a second gap
    r.tick(109);
    SLS_CHECK_EQ(out.size(), 1u);   // 12 has waited 99 ms
    r.tick(110);
    uint32_t want[] = {10, 12, 13};
    SLS_CHECK(seqs_are(out, want, 3));   // released up to the next gap
    SLS_CHECK_EQ(r.stats().gap_released, 1u);
    SLS_CHECK_EQ(r.next_deadline(110), 20);   // 15 arrived at 30
    r.tick(130);
    uint32_t want2[] = {10, 12, 13, 15};
    SLS_CHECK(seqs_are(out, want2, 4));
    // the late 11 and 14 go straight through
    push(r, 11, 140);
    push(r, 14, 140);
    SLS_CHECK_EQ(out.size(), 6u);
    SLS_CHECK_EQ(r.stats().late, 2u);
    push(r, 16, 150);
    SLS_CHECK_EQ(out.size(), 7u);
    SLS_CHECK_EQ(r.held_count(), 0u);
}

SLS_TEST(reorder_oldest_held_decides)
{
    // 20 arrives first (fast link), 15 later: the hold runs from 20's arrival
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(100, collect, &out);
    push(r, 10, 0);
    push(r, 20, 0);
    push(r, 15, 50);
    r.tick(100);
    uint32_t want[] = {10, 15, 20};
    SLS_CHECK(seqs_are(out, want, 3));
    SLS_CHECK_EQ(r.stats().gap_released, 2u);
}

SLS_TEST(reorder_duplicates)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(100, collect, &out);
    push(r, 10, 0);
    push(r, 12, 0);
    push(r, 12, 1);        // a copy of a held packet: dropped
    SLS_CHECK_EQ(r.stats().duplicates, 1u);
    push(r, 10, 2);        // a copy of a released one: late, forwarded
    SLS_CHECK_EQ(r.stats().late, 1u);
    push(r, 11, 3);
    uint32_t want[] = {10, 10, 11, 12};
    SLS_CHECK(seqs_are(out, want, 4));
}

SLS_TEST(reorder_wraparound)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(100, collect, &out);
    push(r, 0x7FFFFFFEu, 0);
    push(r, 0, 1);
    push(r, 1, 2);
    SLS_CHECK_EQ(out.size(), 1u);
    push(r, 0x7FFFFFFFu, 3);
    uint32_t want[] = {0x7FFFFFFEu, 0x7FFFFFFFu, 0, 1};
    SLS_CHECK(seqs_are(out, want, 4));
    SLS_CHECK_EQ(r.held_count(), 0u);
}

SLS_TEST(reorder_cap_releases_early)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(10, collect, &out);   // the minimum window
    int n = r.slots();
    SLS_CHECK_EQ(n, SLS_SRTLA_REORDER_MIN_SLOTS);
    push(r, 0, 0);
    for (int s = 2; s < n; s++) push(r, (uint32_t)s, 0);   // 1 missing, the window full
    SLS_CHECK_EQ(out.size(), 1u);
    push(r, (uint32_t)n + 5, 0);       // beyond the window: release early
    SLS_CHECK_EQ(r.stats().cap_released, 1u);
    SLS_CHECK_EQ(out.size(), (size_t)n - 1);   // 0 and 2..n-1
    SLS_CHECK_EQ(r.held_count(), 1u);          // n+5 waits for n..n+4
    for (size_t i = 1; i < out.size(); i++) SLS_CHECK(out[i] > out[i - 1]);
}

SLS_TEST(reorder_big_jump_resets)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(10, collect, &out);
    int n = r.slots();
    push(r, 100, 0);
    push(r, 102, 0);
    // a confirmed jump far ahead (an outage): forwarded unbuffered until
    // confirmed, then the window moves there
    uint32_t far = 100 + 3 * (uint32_t)n;
    for (int k = 0; k < SLS_SRTLA_REORDER_JUMP_CONFIRM - 1; k++) push(r, far + k, 1);
    SLS_CHECK_EQ(r.stats().resets, 0u);
    SLS_CHECK_EQ(r.held_count(), 1u);      // 102 still waits for 101
    push(r, far + SLS_SRTLA_REORDER_JUMP_CONFIRM - 1, 1);
    SLS_CHECK_EQ(r.stats().resets, 1u);
    SLS_CHECK_EQ(r.held_count(), 0u);
    SLS_CHECK_EQ(out.size(), (size_t)SLS_SRTLA_REORDER_JUMP_CONFIRM + 2);
    push(r, far + SLS_SRTLA_REORDER_JUMP_CONFIRM, 2);
    SLS_CHECK_EQ(out.back(), far + SLS_SRTLA_REORDER_JUMP_CONFIRM);
}

SLS_TEST(reorder_single_forged_far_ahead)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(100, collect, &out);
    push(r, 1000, 0);
    push(r, 1002, 0);
    push(r, 1000 + 10 * (uint32_t)r.slots(), 1);   // one forged packet
    SLS_CHECK_EQ(r.stats().resets, 0u);
    SLS_CHECK_EQ(r.held_count(), 1u);               // the buffer is untouched
    push(r, 1001, 2);
    push(r, 1003, 3);
    SLS_CHECK_EQ(r.held_count(), 0u);
    SLS_CHECK_EQ(out.size(), 5u);
    SLS_CHECK_EQ(out[2], 1001u);
    SLS_CHECK_EQ(out[3], 1002u);
    SLS_CHECK_EQ(out[4], 1003u);
}

SLS_TEST(reorder_old_retransmission_is_late)
{
    // the reviewer's case: hold 50, a retransmission ~400 packets late must
    // not reset the buffer or disturb the order of what is held
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(50, collect, &out);
    r.start_at(1000);
    for (uint32_t s = 1000; s < 1400; s++) if (s != 1100) push(r, s, 0);
    r.tick(60);                    // 1101.. waited; the gap at 1100 is given up
    push(r, 1402, 61);             // 1400, 1401 missing: held
    push(r, 1100, 70);             // the retransmission: late, forwarded
    push(r, 1400, 71);
    push(r, 1401, 72);
    SLS_CHECK_EQ(r.stats().resets, 0u);
    SLS_CHECK_EQ(r.held_count(), 0u);
    size_t k = out.size();
    SLS_CHECK(k >= 4 && out[k - 4] == 1100 && out[k - 3] == 1400 && out[k - 2] == 1401 && out[k - 1] == 1402);
    // a retransmission from far back (more than 2x the window) as well
    push(r, 1001, 80);
    SLS_CHECK_EQ(r.stats().resets, 0u);
    SLS_CHECK_EQ(out.back(), 1001u);
}

SLS_TEST(reorder_cap_empty_buffer_not_counted)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(10, collect, &out);
    push(r, 0, 0);
    push(r, (uint32_t)r.slots() + 10, 0);   // a loss within 2x the window, nothing held
    SLS_CHECK_EQ(r.stats().cap_released, 0u);
    SLS_CHECK_EQ(r.held_count(), 1u);
}

SLS_TEST(reorder_set_hold_keeps_order)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(100, collect, &out);
    push(r, 10, 0);
    push(r, 14, 0);
    push(r, 12, 0);
    push(r, 13, 0);
    r.set_hold(200, 5);
    uint32_t want[] = {10, 12, 13, 14};
    SLS_CHECK(seqs_are(out, want, 4));
    SLS_CHECK_EQ(r.stats().held, 3u);
}

SLS_TEST(reorder_deadline_across_wrap)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(100, collect, &out);
    push(r, 0x7FFFFFFDu, 0);
    push(r, 0x7FFFFFFFu, 10);
    push(r, 1, 20);
    SLS_CHECK_EQ(r.next_deadline(50), 60);
    r.tick(110);
    uint32_t want[] = {0x7FFFFFFDu, 0x7FFFFFFFu};
    SLS_CHECK(seqs_are(out, want, 2));
    SLS_CHECK_EQ(r.next_deadline(110), 10);
    r.tick(120);
    SLS_CHECK_EQ(out.size(), 3u);
    SLS_CHECK_EQ(r.next_deadline(120), -1);
}

SLS_TEST(reorder_hold_zero_passthrough)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(0, collect, &out);
    push(r, 5, 0);
    push(r, 7, 0);
    push(r, 6, 0);
    uint32_t want[] = {5, 7, 6};
    SLS_CHECK(seqs_are(out, want, 3));
    SLS_CHECK_EQ(r.held_count(), 0u);
}

SLS_TEST(reorder_start_at_and_flush)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(100, collect, &out);
    r.start_at(50);
    push(r, 51, 0);        // 50 (the first) arrives second: held, not late
    push(r, 50, 1);
    uint32_t want[] = {50, 51};
    SLS_CHECK(seqs_are(out, want, 2));
    push(r, 53, 2);
    push(r, 55, 2);
    r.flush();             // a new session: everything held goes out in order
    uint32_t want2[] = {50, 51, 53, 55};
    SLS_CHECK(seqs_are(out, want2, 4));
    SLS_CHECK_EQ(r.held_count(), 0u);
    push(r, 9, 3);         // and the sequence starts over
    SLS_CHECK_EQ(out.size(), 5u);
    SLS_CHECK_EQ(r.stats().late, 0u);
    // reset drops what is held
    push(r, 11, 4);
    r.reset();
    SLS_CHECK_EQ(r.held_count(), 0u);
    SLS_CHECK_EQ(out.size(), 5u);
}

SLS_TEST(reorder_set_hold)
{
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(500, collect, &out);
    SLS_CHECK_EQ(r.slots(), 1900 * 500 / 1000 * 3 / 2);
    push(r, 1, 0);
    push(r, 3, 0);
    r.set_hold(20, 0);     // held packets are released first
    SLS_CHECK_EQ(out.size(), 2u);
    SLS_CHECK_EQ(r.hold_ms(), 20);
    SLS_CHECK_EQ(r.slots(), SLS_SRTLA_REORDER_MIN_SLOTS);
}

SLS_TEST(reorder_long_run_bounded)
{
    // steady two-link interleave with jitter: everything comes out, in
    // order, and the arrival list stays bounded
    std::vector<uint32_t> out;
    CSLSSrtlaReorder r(100, collect, &out);
    uint32_t s = 0x7FFFF000u;
    r.start_at(s);         // from the handshake: the first packet arrives second
    int64_t t = 0;
    for (int i = 0; i < 200000; i += 2) {
        push(r, sls_srt_seq_add(s, i + 1), t);   // the faster link first
        push(r, sls_srt_seq_add(s, i), t + 3);
        t += 1;
        if (r.next_deadline(t) == 0) r.tick(t);
    }
    SLS_CHECK_EQ(out.size(), 200000u);
    bool ordered = true;
    for (size_t i = 1; i < out.size(); i++) if (sls_srt_seq_diff(out[i - 1], out[i]) != 1) ordered = false;
    SLS_CHECK(ordered);
    SLS_CHECK_EQ(r.stats().gap_released, 0u);
    SLS_CHECK_EQ(r.held_count(), 0u);
}
