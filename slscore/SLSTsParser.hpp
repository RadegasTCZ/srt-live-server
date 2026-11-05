#ifndef _SLSTsParser_INCLUDE_
#define _SLSTsParser_INCLUDE_

#include <stdint.h>

#include "common.hpp"

#define SLS_TS_PARAMSETS_MAX 1024          // total VPS+SPS+PPS bytes kept
#define SLS_TS_BOOTSTRAP_MAX TS_UDP_LEN    // bootstrap <= 7 packets
#define SLS_TS_SECTION_MAX   181           // PAT/PMT section bytes that fit one bootstrap packet
#define SLS_TS_PES_SCAN_MAX  16384         // ES bytes of one PES searched for parameter sets

#define SLS_TS_TYPE_H264     0x1B
#define SLS_TS_TYPE_HEVC     0x24

/**
 * Late-join PSI bootstrap for one published stream.
 *
 * Fed every packet of the publisher's stream: keeps the PAT, the PMT it
 * points to, picks the video PID from the PMT (first ES with stream_type
 * 0x1B H.264 or 0x24 HEVC) and collects that codec's parameter sets (H.264
 * SPS+PPS, HEVC VPS+SPS+PPS) from the start of one video PES. Then it builds
 * the bootstrap once: PAT packet, PMT packet, and one PES on the video PID
 * holding the parameter sets (Annex-B), each PID starting at CC 0 with the
 * discontinuity_indicator set on its first packet.
 *
 * The parameter-set PES carries a PTS: a PTS-less one makes GStreamer's
 * tsdemux abort when the next PES comes before any PCR. The parser keeps
 * reading the timestamp of every video PES start, also once the bootstrap
 * is built, and build_bootstrap() writes the latest one into each copy, so
 * the bootstrap sits just before the first frame a joining player gets.
 * Without a video timestamp seen yet there is no bootstrap.
 *
 * Every read is bounded by the 188-byte packet or the stored section;
 * anything malformed, truncated, scrambled or spanning packets where we
 * don't reassemble is ignored. Streams without H.264/HEVC video never
 * complete, so players get live data without a bootstrap.
 */
class CSLSTsParser
{
public:
    CSLSTsParser();

    void reset();
    void feed(const uint8_t *pkt);               // exactly 188 bytes; never reads outside them
    bool complete() const;                       // PAT && PMT && video PID && all param sets for its codec && a video timestamp
    int  build_bootstrap(uint8_t *out, int cap) const; // multiple of 188, PES PTS = latest video timestamp; 0 if !complete()
    int  video_pid() const;
    int  video_stream_type() const;              // 0x1B H.264, 0x24 HEVC, -1 unknown
    // > 0 once, the first time the parameter sets were collected but the
    // bootstrap did not fit SLS_TS_BOOTSTRAP_MAX: their size in bytes. The
    // caller logs it; later failures (every GOP) return 0.
    int  take_oversize();

private:
    void on_pat(const uint8_t *sec, int len);
    void on_pmt(const uint8_t *sec, int len);
    void on_video(const uint8_t *payload, int len, bool pusi, int cc);
    void note_timestamp(const uint8_t *p, int len);
    void pes_start(const uint8_t *p, int len);
    void es_feed(const uint8_t *p, int len);
    void es_byte(uint8_t b);
    void nal_close();
    void pes_end();
    void pes_drop();
    void clear_video();
    void make_bootstrap();

    int      m_pmt_pid;
    int      m_pat_len;
    uint8_t  m_pat[SLS_TS_SECTION_MAX];
    int      m_pmt_len;
    uint8_t  m_pmt[SLS_TS_SECTION_MAX];
    int      m_video_pid;
    int      m_video_type;
    int      m_video_cc;          // last CC seen on the video PID, -1 none
    int64_t  m_video_ts;          // latest video PES DTS (PTS if none), 33 bits; -1 none

    // the video PES being searched for parameter sets
    bool     m_pes_active;
    int      m_pes_left;          // ES bytes left in a bounded PES, -1 unbounded
    int      m_pes_scanned;       // ES bytes searched so far (<= SLS_TS_PES_SCAN_MAX)
    uint8_t  m_stream_id;
    int      m_zeros;             // consecutive 0x00 bytes just seen
    bool     m_nal_hdr_next;      // next byte is a NAL header
    bool     m_nal_keep;          // current NAL is a parameter set being copied
    int      m_nal_type;
    int      m_nal_begin;         // offset of the current kept NAL (its start code) in m_ps
    uint64_t m_ps_seen;           // bit n: a NAL of type n was kept
    int      m_ps_len;
    uint8_t  m_ps[SLS_TS_PARAMSETS_MAX];

    int      m_boot_len;          // > 0 once built (complete() also needs m_video_ts)
    int      m_boot_pts;          // offset of the PES PTS field in m_boot
    int      m_oversize;          // parameter-set bytes of a failed build, until taken
    bool     m_oversize_seen;     // a failed build was already reported
    uint8_t  m_boot[SLS_TS_BOOTSTRAP_MAX];
};

#endif
