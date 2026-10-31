#include <string.h>

#include "SLSTsParser.hpp"

/**
 * Bounds discipline: feed() works on one 188-byte packet. Every index into
 * it is checked against TS_PACK_LEN before the byte is read; PSI sections
 * are only used when the whole section (section_length + 3 bytes) lies in
 * the packet; the PES header is only used when all of it lies in the
 * packet. ES bytes are consumed one at a time, so the NAL scan never looks
 * outside the payload it was given, and kept bytes never exceed m_ps.
 */

#define TS_NULL_PID     0x1FFF
#define TS_MIN_PID      0x0010   // 0x0001-0x000F are reserved

static uint32_t crc32_mpeg(const uint8_t *d, int n)
{
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < n; i++) {
        crc ^= (uint32_t)d[i] << 24;
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
    }
    return crc;
}

// A complete, current, single-section PSI table of the given type whose
// CRC matches; len is section_length + 3 and already known to be in bounds.
static bool section_ok(const uint8_t *sec, int len, uint8_t table_id, int min_len)
{
    if (len < min_len || len > SLS_TS_SECTION_MAX)
        return false;
    if (sec[0] != table_id || !(sec[1] & 0x80))      // section_syntax_indicator
        return false;
    if (!(sec[5] & 0x01) || sec[6] != 0 || sec[7] != 0) // current, section 0 of 0
        return false;
    return crc32_mpeg(sec, len) == 0;
}

static bool pid_ok(int pid)
{
    return pid >= TS_MIN_PID && pid < TS_NULL_PID;
}

CSLSTsParser::CSLSTsParser()
{
    reset();
}

void CSLSTsParser::reset()
{
    m_pmt_pid  = -1;
    m_pat_len  = 0;
    m_pmt_len  = 0;
    m_boot_len = 0;
    m_video_pid  = -1;
    m_video_type = -1;
    m_pes_left    = -1;
    m_pes_scanned = 0;
    m_stream_id   = 0xE0;
    m_zeros       = 0;
    m_nal_type    = 0;
    m_nal_begin   = 0;
    clear_video();
}

bool CSLSTsParser::complete() const
{
    return m_boot_len > 0;
}

int CSLSTsParser::video_pid() const
{
    return m_video_pid;
}

int CSLSTsParser::video_stream_type() const
{
    return m_video_type;
}

int CSLSTsParser::build_bootstrap(uint8_t *out, int cap) const
{
    if (m_boot_len <= 0 || out == NULL || cap < m_boot_len)
        return 0;
    memcpy(out, m_boot, m_boot_len);
    return m_boot_len;
}

void CSLSTsParser::feed(const uint8_t *pkt)
{
    if (pkt == NULL || m_boot_len > 0)
        return;
    if (pkt[0] != TS_SYNC_BYTE || (pkt[1] & 0x80))  // sync, transport_error_indicator
        return;
    if (pkt[3] & 0xC0)                              // scrambled
        return;
    int  pid  = ((pkt[1] & 0x1F) << 8) | pkt[2];
    bool pusi = (pkt[1] & 0x40) != 0;
    int  afc  = (pkt[3] >> 4) & 0x03;
    if (!(afc & 0x01))                              // reserved or adaptation field only
        return;
    int off = 4;
    if (afc & 0x02) {
        off = 5 + pkt[4];                           // pkt[4]: adaptation_field_length
        if (off >= TS_PACK_LEN)                     // no payload byte left
            return;
    }
    // payload: pkt[off .. 187], at least one byte

    if (pid == PAT_PID || pid == m_pmt_pid) {
        if (!pusi)                                  // sections spanning packets aren't used
            return;
        int s = off + 1 + pkt[off];                 // skip pointer_field
        if (s + 3 > TS_PACK_LEN)
            return;
        int len = 3 + (((pkt[s + 1] & 0x0F) << 8) | pkt[s + 2]);
        if (len > TS_PACK_LEN - s)                  // section runs past the packet
            return;
        if (pid == PAT_PID)
            on_pat(pkt + s, len);
        else
            on_pmt(pkt + s, len);
        return;
    }
    if (pid == m_video_pid)
        on_video(pkt + off, TS_PACK_LEN - off, pusi, pkt[3] & 0x0F);
}

void CSLSTsParser::on_pat(const uint8_t *sec, int len)
{
    if (!section_ok(sec, len, 0x00, 9))
        return;
    // program loop: 4-byte entries between the 8-byte header and the CRC
    int pmt_pid = -1;
    for (int i = 8; i + 4 <= len - 4; i += 4) {
        int program = (sec[i] << 8) | sec[i + 1];
        if (program != 0) {                         // 0 is the network PID
            pmt_pid = ((sec[i + 2] & 0x1F) << 8) | sec[i + 3];
            break;
        }
    }
    if (!pid_ok(pmt_pid))
        return;
    if (pmt_pid != m_pmt_pid) {
        m_pmt_pid = pmt_pid;
        m_pmt_len = 0;
        m_video_pid  = -1;
        m_video_type = -1;
        clear_video();
    }
    memcpy(m_pat, sec, len);
    m_pat_len = len;
}

void CSLSTsParser::on_pmt(const uint8_t *sec, int len)
{
    if (!section_ok(sec, len, 0x02, 13))
        return;
    int end = len - 4;                              // ES loop stops before the CRC
    int i = 12 + (((sec[10] & 0x0F) << 8) | sec[11]); // skip program_info
    if (i > end)
        return;
    int vpid = -1, vtype = -1;
    while (i + 5 <= end) {
        int type = sec[i];
        int pid  = ((sec[i + 1] & 0x1F) << 8) | sec[i + 2];
        int info = ((sec[i + 3] & 0x0F) << 8) | sec[i + 4];
        if (i + 5 + info > end)
            return;                                 // malformed ES loop
        if (type == SLS_TS_TYPE_H264 || type == SLS_TS_TYPE_HEVC) {
            if (pid_ok(pid) && pid != m_pmt_pid) {
                vpid  = pid;
                vtype = type;
            }
            break;
        }
        i += 5 + info;
    }
    memcpy(m_pmt, sec, len);
    m_pmt_len = len;
    if (vpid != m_video_pid || vtype != m_video_type) {
        m_video_pid  = vpid;
        m_video_type = vtype;
        clear_video();
    }
}

void CSLSTsParser::clear_video()
{
    pes_drop();
    m_video_cc = -1;
}

void CSLSTsParser::on_video(const uint8_t *payload, int len, bool pusi, int cc)
{
    if (m_video_cc >= 0) {
        if (cc == m_video_cc)                       // duplicate packet
            return;
        if (cc != ((m_video_cc + 1) & 0x0F))        // lost packet: this PES is unusable
            pes_drop();
    }
    m_video_cc = cc;
    if (pusi) {
        if (m_pes_active)
            pes_end();                              // an unbounded PES ends at the next start
        if (m_boot_len == 0)
            pes_start(payload, len);
    } else if (m_pes_active) {
        es_feed(payload, len);
    }
}

void CSLSTsParser::pes_start(const uint8_t *p, int len)
{
    // 00 00 01 stream_id PES_packet_length(2) flags(2) PES_header_data_length
    if (len < 9)
        return;
    if (p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01)
        return;
    if ((p[3] & 0xF0) != 0xE0 || (p[6] & 0xC0) != 0x80) // video stream_id, '10' marker
        return;
    int hdr = 9 + p[8];
    if (hdr > len)                                  // header continues past this packet
        return;
    int plen = (p[4] << 8) | p[5];
    int left = -1;
    if (plen != 0) {
        left = plen - (hdr - 6);
        if (left < 0)
            return;
    }
    m_pes_active   = true;
    m_pes_left     = left;
    m_pes_scanned  = 0;
    m_stream_id    = p[3];
    m_zeros        = 0;
    m_nal_hdr_next = false;
    m_nal_keep     = false;
    m_ps_len       = 0;
    m_ps_seen      = 0;
    es_feed(p + hdr, len - hdr);
}

void CSLSTsParser::es_feed(const uint8_t *p, int len)
{
    if (m_pes_left >= 0 && len > m_pes_left)
        len = m_pes_left;
    if (len > SLS_TS_PES_SCAN_MAX - m_pes_scanned)
        len = SLS_TS_PES_SCAN_MAX - m_pes_scanned;
    for (int i = 0; i < len && m_pes_active; i++)
        es_byte(p[i]);
    if (!m_pes_active)
        return;
    m_pes_scanned += len;
    if (m_pes_left >= 0) {
        m_pes_left -= len;
        if (m_pes_left == 0) {
            pes_end();
            return;
        }
    }
    if (m_pes_scanned >= SLS_TS_PES_SCAN_MAX)       // parameter sets come first; give up
        pes_drop();
}

// Annex-B scan, one byte at a time. Parameter-set NALs are copied to m_ps
// with a 4-byte start code; the first slice NAL ends the search.
void CSLSTsParser::es_byte(uint8_t b)
{
    if (m_nal_hdr_next) {
        m_nal_hdr_next = false;
        bool hevc = m_video_type == SLS_TS_TYPE_HEVC;
        int  t    = hevc ? (b >> 1) & 0x3F : b & 0x1F;
        bool vcl  = hevc ? t < 32 : (t >= 1 && t <= 5);
        if (vcl) {
            pes_end();
            return;
        }
        m_nal_keep = hevc ? (t >= 32 && t <= 34) : (t == 7 || t == 8);
        if (m_nal_keep) {
            if (m_ps_len + 5 > SLS_TS_PARAMSETS_MAX) {
                pes_drop();
                return;
            }
            m_nal_type  = t;
            m_nal_begin = m_ps_len;
            m_ps[m_ps_len++] = 0x00;
            m_ps[m_ps_len++] = 0x00;
            m_ps[m_ps_len++] = 0x00;
            m_ps[m_ps_len++] = 0x01;
            m_ps[m_ps_len++] = b;
        }
        m_zeros = (b == 0x00) ? 1 : 0;
        return;
    }
    if (b == 0x01 && m_zeros >= 2) {                // start code: the current NAL ends
        nal_close();
        m_nal_hdr_next = true;
        m_zeros = 0;
        return;
    }
    if (b == 0x00) {
        if (m_zeros < 3)
            m_zeros++;
    } else {
        m_zeros = 0;
    }
    if (m_nal_keep) {
        if (m_ps_len >= SLS_TS_PARAMSETS_MAX) {
            pes_drop();
            return;
        }
        m_ps[m_ps_len++] = b;
    }
}

void CSLSTsParser::nal_close()
{
    if (!m_nal_keep)
        return;
    m_nal_keep = false;
    // the zeros of the start code that ended it (a NAL never ends in 0x00)
    while (m_ps_len > m_nal_begin + 5 && m_ps[m_ps_len - 1] == 0x00)
        m_ps_len--;
    if (m_ps_len - m_nal_begin < 4 + 3) {           // start code + header + payload
        m_ps_len = m_nal_begin;
        return;
    }
    m_ps_seen |= (uint64_t)1 << m_nal_type;
}

void CSLSTsParser::pes_end()
{
    if (!m_pes_active)
        return;
    nal_close();
    m_pes_active = false;
    uint64_t need = (m_video_type == SLS_TS_TYPE_HEVC)
        ? ((uint64_t)1 << 32) | ((uint64_t)1 << 33) | ((uint64_t)1 << 34)
        : ((uint64_t)1 << 7) | ((uint64_t)1 << 8);
    if ((m_ps_seen & need) == need && m_pat_len > 0 && m_pmt_len > 0)
        make_bootstrap();
    if (m_boot_len == 0) {
        m_ps_len  = 0;
        m_ps_seen = 0;
    }
}

void CSLSTsParser::pes_drop()
{
    m_pes_active   = false;
    m_nal_keep     = false;
    m_nal_hdr_next = false;
    m_ps_len       = 0;
    m_ps_seen      = 0;
}

// One PSI section in one packet: CC 0, adaptation field carrying only the
// discontinuity_indicator, pointer_field 0, 0xFF stuffing.
static int psi_packet(uint8_t *p, int pid, const uint8_t *sec, int len)
{
    p[0] = TS_SYNC_BYTE;
    p[1] = 0x40 | ((pid >> 8) & 0x1F);
    p[2] = pid & 0xFF;
    p[3] = 0x30;                                    // AF + payload, CC 0
    p[4] = 1;                                       // adaptation_field_length
    p[5] = 0x80;                                    // discontinuity_indicator
    p[6] = 0;                                       // pointer_field
    memcpy(p + 7, sec, len);                        // len <= SLS_TS_SECTION_MAX = 188 - 7
    memset(p + 7 + len, 0xFF, TS_PACK_LEN - 7 - len);
    return TS_PACK_LEN;
}

void CSLSTsParser::make_bootstrap()
{
    uint8_t *o = m_boot;
    int pos = psi_packet(o, PAT_PID, m_pat, m_pat_len);
    pos += psi_packet(o + pos, m_pmt_pid, m_pmt, m_pmt_len);

    // PES with exact length, data_alignment_indicator, no timestamps
    uint8_t hdr[9];
    int body = 3 + m_ps_len;                        // flags(2) + header_data_length + ES
    hdr[0] = 0x00; hdr[1] = 0x00; hdr[2] = 0x01;
    hdr[3] = m_stream_id;
    hdr[4] = (body >> 8) & 0xFF;
    hdr[5] = body & 0xFF;
    hdr[6] = 0x84;
    hdr[7] = 0x00;
    hdr[8] = 0x00;
    int total = 9 + m_ps_len;

    int done = 0, cc = 0;
    while (done < total) {
        if (pos + TS_PACK_LEN > SLS_TS_BOOTSTRAP_MAX) { // doesn't fit 7 packets
            m_boot_len = 0;
            return;
        }
        uint8_t *p = o + pos;
        bool first = cc == 0;
        int room = TS_PACK_LEN - 4 - (first ? 2 : 0);   // first packet: AF with the flag
        int take = total - done < room ? total - done : room;
        int af   = TS_PACK_LEN - 4 - take;          // adaptation field bytes incl. length
        p[0] = TS_SYNC_BYTE;
        p[1] = (first ? 0x40 : 0x00) | ((m_video_pid >> 8) & 0x1F);
        p[2] = m_video_pid & 0xFF;
        p[3] = (af > 0 ? 0x30 : 0x10) | (cc & 0x0F);
        int q = 4;
        if (af > 0) {
            p[q++] = af - 1;
            if (af > 1) {
                p[q++] = first ? 0x80 : 0x00;
                memset(p + q, 0xFF, af - 2);
                q += af - 2;
            }
        }
        for (int k = 0; k < take; k++, done++)
            p[q + k] = done < 9 ? hdr[done] : m_ps[done - 9];
        pos += TS_PACK_LEN;
        cc++;
    }
    m_boot_len = pos;
}
