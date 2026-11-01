
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


#include <sys/stat.h>
#include <sys/types.h>
#include <cstdarg>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "TSFileTimeReader.hpp"
#include "SLSLog.hpp"

#define RTS_PACK_LEN (1316 + sizeof(int64_t))
#define RTS_BUF_SIZE (RTS_PACK_LEN * 100)

static int64_t ts_read_timestamp(const uint8_t *p)
{
    return ((int64_t)(p[0] & 0x0E) << 29) | ((int64_t)p[1] << 22) |
           ((int64_t)(p[2] & 0xFE) << 14) | ((int64_t)p[3] << 7) | (p[4] >> 1);
}

// PTS/DTS of a PES starting in this 188-byte packet (DTS = PTS when only
// PTS is present). False if the packet starts no PES with a timestamp, or
// the header doesn't fit the packet; never reads outside the packet.
static bool ts_pes_time(const uint8_t *pkt, int *pid, int64_t *dts, int64_t *pts)
{
    if (pkt[0] != TS_SYNC_BYTE || !(pkt[1] & 0x40))
        return false;
    int afc = (pkt[3] >> 4) & 0x03;
    if (!(afc & 0x01))
        return false;
    int off = 4;
    if (afc & 0x02)
        off = 5 + pkt[4];
    if (off + 14 > TS_PACK_LEN)                 // PES header up to the PTS
        return false;
    const uint8_t *p = pkt + off;
    if (p[0] != 0x00 || p[1] != 0x00 || p[2] != 0x01)
        return false;
    // Only stream ids that carry the optional PES header have a PTS/DTS:
    // not program_stream_map, padding, private_stream_2, ECM/EMM, DSMCC,
    // H.222.1 type E or the program stream directory.
    uint8_t stream_id = p[3];
    if (stream_id == 0xBC || stream_id == 0xBE || stream_id == 0xBF ||
        stream_id == 0xF0 || stream_id == 0xF1 || stream_id == 0xF2 ||
        stream_id == 0xF8 || stream_id == 0xFF)
        return false;
    if ((p[6] & 0xC0) != 0x80)                  // '10' marker bits
        return false;
    int flags = p[7] & 0xC0;
    if (flags == 0x80) {
        *pts = *dts = ts_read_timestamp(p + 9);
    } else if (flags == 0xC0 && off + 19 <= TS_PACK_LEN) {
        *pts = ts_read_timestamp(p + 9);
        *dts = ts_read_timestamp(p + 14);
    } else {
        return false;
    }
    *pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    return true;
}


CTSFileTimeReader::CTSFileTimeReader()
{
	m_rts_fd       = -1;
	m_loop         = true;
	m_dts_pid      = INVALID_PID;
    m_dts          = INVALID_DTS_PTS;
    m_pts          = INVALID_DTS_PTS;
    m_readed_count = 0;
    m_udp_duration = 0;

    memset(m_file_name, 0, URL_MAX_LEN);
}

CTSFileTimeReader::~CTSFileTimeReader()
{
}

int CTSFileTimeReader::open(const char *ts_file_name, bool loop)
{
    if (NULL == ts_file_name || strlen(ts_file_name) == 0) {
        sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::open, wrong file_name.", this);
    	return SLS_ERROR;
    }

    close();
    int ret = generate_rts_file(ts_file_name);
    if (SLS_OK != ret) {
    	sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::open, generate_rts_file failed, '%s'.\n",
    			this, ts_file_name);
    	return SLS_ERROR;
    }

    m_rts_fd = ::open(m_file_name, O_RDONLY);
    if (m_rts_fd < 0) {
    	sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::open, open file='%s' failed, '%s'.\n",
    			this, m_file_name, strerror(errno));
    	return SLS_ERROR;
    }
    m_array_data.setSize(RTS_BUF_SIZE);

    m_loop = loop;
    m_readed_count = 0;
	sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::open, ok, fd=%d, file_name='%s', loop=%d.\n",
			this, m_rts_fd, m_file_name, m_loop);
	return SLS_OK;
}

int CTSFileTimeReader::close()
{
    if (m_rts_fd >= 0) {
        sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::close, ok, fd=%d, file_name='%s'.",
                this, m_rts_fd, m_file_name);
    	::close(m_rts_fd);
        m_rts_fd = -1;
    }
    return SLS_OK;
}

int  CTSFileTimeReader::get(uint8_t *data, int size, int64_t &tm_ms, bool& jitter)
{
	jitter = false;
    if (m_rts_fd < 0) {
        sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::get, failed, fd=%d, file_name='%s'.",
                this, m_rts_fd, m_file_name);
    	return SLS_ERROR;
    }

    if (m_array_data.count() == 0) {
    	uint8_t rts_data[RTS_BUF_SIZE];
    	int n = read_records(rts_data);
    	if (n > 0){
    		m_array_data.put(rts_data, n);
    	} else {
        	if (!m_loop) {
    	    	sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::get, file end, file='%s'.\n",
    	    			this, m_file_name);
    	    	return SLS_ERROR;
        	}
			sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::get, loop=1, reopen file='%s'.\n",
					this, m_file_name);
			//1. read data from header again.
			::close(m_rts_fd);
			m_rts_fd = ::open(m_file_name, O_RDONLY);
			if (m_rts_fd < 0) {
				sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::get, open file='%s' failed, '%s'.\n",
						this, m_file_name, strerror(errno));
				return SLS_ERROR;
			}
			m_readed_count = 0;
			n = read_records(rts_data);
			if (n > 0){
				m_array_data.put(rts_data, n);
			} else {
	        	sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::get, read data failed, '%s'.\n",
	        			this, strerror(errno));
				return SLS_ERROR;
			}
    	}
    }

    if (m_array_data.count() == 0) {
        sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::get, failed, m_array.count() is zero, file_name='%s'.",
                this, m_file_name);
    	return SLS_ERROR;
    }

    int64_t rts = INVALID_DTS_PTS;
    int ret = m_array_data.get((uint8_t*)&rts, sizeof(rts));
    if (ret != sizeof(rts)) {
        sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::get, failed, m_array_data.get rts, ret=%d.",
                this, ret);
    	return SLS_ERROR;
    }
    tm_ms = rts/90;//rts is 90K clock

    ret = m_array_data.get(data, size);
    if (ret > 0) {
    	m_readed_count += ret;
    }
    if (ret != size) {
        sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::get, failed, m_array_data. get data, ret=%d, not %d.",
                this, ret, size);
    	return SLS_ERROR;
    }
    return SLS_OK;
}

// Read whole <rts, TS_UDP_LEN> records from the .rts file; a torn tail is dropped.
// Returns the byte count (a multiple of RTS_PACK_LEN), 0 at end of file, <0 on error.
int CTSFileTimeReader::read_records(uint8_t *buf)
{
    int n = ::read(m_rts_fd, buf, RTS_BUF_SIZE);
    if (n <= 0)
        return n;
    return n - (int)(n % RTS_PACK_LEN);
}

// One rts record: the 8-byte time stamp, then one TS_UDP_LEN unit. False on a
// failed or short write (disk full): the rts file is then incomplete.
static bool write_rts_record(int fd, int64_t rts, const uint8_t *data)
{
    errno = 0;               // stays 0 for a short write
    if (write(fd, &rts, sizeof(rts)) != (ssize_t)sizeof(rts)) {
        return false;
    }
    return write(fd, data, TS_UDP_LEN) == (ssize_t)TS_UDP_LEN;
}

int64_t CTSFileTimeReader::generate_rts_file(const char  *ts_file_name)
{
    int ret = SLS_ERROR;
    if (NULL == ts_file_name || strlen(ts_file_name) == 0) {
        sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::generate_rts_file, failed, no ts_file_name.", this);
    	return ret;
    }
    char rts_file_name[URL_MAX_LEN] = {0};
    int name_len = snprintf(rts_file_name, sizeof(rts_file_name), "%s.rts", ts_file_name);
    if (name_len < 0 || (size_t)name_len >= sizeof(rts_file_name)) {
        sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::generate_rts_file, failed, ts_file_name is too long.", this);
    	return ret;
    }
    memcpy(m_file_name, rts_file_name, name_len + 1);

    struct stat rts_file;
    if (0 == stat(rts_file_name, &rts_file)) {
        sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::generate_rts_file, ts_file_name='%s' exist.",
                this, rts_file_name);
        return SLS_OK;
    }

    int ts_fd = ::open(ts_file_name, O_RDONLY);
    if (ts_fd < 0) {
    	sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::generate_rts_file, open file='%s' failed, '%s'.\n",
    			this, ts_file_name, strerror(errno));
    	return ret;
    }

    m_rts_fd = ::open(rts_file_name, O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (m_rts_fd < 0) {
    	sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::generate_rts_file, create file='%s' failed, '%s'.\n",
    			this, rts_file_name, strerror(errno));
    	::close(ts_fd);
    	return ret;
    }

    m_array_data.setSize(TS_UDP_LEN * 1000);
    uint8_t ts_pack[TS_PACK_LEN] = {0};
    int ts_index = 0;
    int dts_index = 0;
    struct { int es_pid; int64_t dts; int64_t pts; } ti = {INVALID_PID, INVALID_DTS_PTS, INVALID_DTS_PTS};
    bool write_ok = true;
    int  write_errno = 0;
    while(true) {
    	int n = read(ts_fd, ts_pack, TS_PACK_LEN);
    	if (n < TS_PACK_LEN)
    		break;
        // timestamps of the first timestamped PID only
        int pid;
        int64_t dts, pts;
        if (ts_pes_time(ts_pack, &pid, &dts, &pts) && (INVALID_PID == ti.es_pid || pid == ti.es_pid)) {
            ti.es_pid = pid;
            ti.dts    = dts;
            ti.pts    = pts;
        }
        if (INVALID_DTS_PTS == ti.dts ) {
            m_array_data.put(ts_pack, TS_PACK_LEN);
            ts_index ++;
            continue;
        }
        if (INVALID_DTS_PTS == m_dts) {
            //the 1st dts
            sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::generate_rts_file, ts_index=%d, dts_index=%d, ti.es_pid=%d, dts=%lld.",
                    this, ts_index, dts_index, ti.es_pid, (long long)ti.dts);
            m_dts = ti.dts;
            m_pts = ti.pts;
            ti.dts = INVALID_DTS_PTS;
            ti.pts = INVALID_DTS_PTS;
            m_dts_pid = ti.es_pid;

            m_array_data.put(ts_pack, TS_PACK_LEN);
            ts_index ++;
            dts_index ++;
            continue;
        }
        //write the rts packet
        int ts_count = m_array_data.count();
        int ts_udp_count = ts_count/TS_UDP_LEN;
        if (ts_udp_count > 0) {
            m_udp_duration = (ti.dts - m_dts)/ts_udp_count;
            int64_t rts = m_dts;
            uint8_t rts_data[TS_UDP_LEN];
            for (int i = 0; i < ts_udp_count; i++) {
                int n = m_array_data.get(rts_data, TS_UDP_LEN);
                if (n != TS_UDP_LEN) {
                    printf("ts2es: m_array_data.get, wrong n=%d, not %d.\n", n, TS_UDP_LEN);
                    break;
                }
                if (write_ok && !write_rts_record(m_rts_fd, rts, rts_data)) {
                    write_ok = false;
                    write_errno = errno;
                }
                rts += m_udp_duration;
            }
            m_dts = ti.dts;
            m_pts = ti.pts;
        }
        sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::generate_rts_file, ts_index=%d, dts_index=%d, m_dts_pid=%d, dts=%lld.",
                this, ts_index, dts_index, m_dts_pid, (long long)ti.dts);
        ti.dts = INVALID_DTS_PTS;
        ti.pts = INVALID_DTS_PTS;

        m_array_data.put(ts_pack, TS_PACK_LEN);
        ts_index ++;
        dts_index ++;
    }
    //last data in m_array_data
	int64_t rts = m_dts;
	int ts_count = m_array_data.count();
	int ts_udp_count = ts_count/TS_UDP_LEN;
	int n = 0;
	uint8_t udp_data[TS_UDP_LEN];
	if (ts_count > 0){
		for (int i = 0; i < ts_udp_count+1; i++) {
			n = m_array_data.get(udp_data, TS_UDP_LEN);
			if (n != TS_UDP_LEN) {
				break;
			}
			if (write_ok && !write_rts_record(m_rts_fd, rts, udp_data)) {
				write_ok = false;
				write_errno = errno;
			}
			rts += m_udp_duration;
		}
	}
	//remainder
	if (n > 0) {
		//fill the remainder with null packets
		memset(udp_data + n, 0xFF, TS_UDP_LEN - n);
		for (int i = n; i < TS_UDP_LEN;) {
		    udp_data[i] = 0x47;
		    udp_data[i+1] = 0x1F;
		    udp_data[i+2] = 0xFF;
			i += TS_PACK_LEN;
		}
		if (write_ok && !write_rts_record(m_rts_fd, rts, udp_data)) {
			write_ok = false;
			write_errno = errno;
		}
	}

    ::close(ts_fd);
    ::close(m_rts_fd);
    m_rts_fd = -1;
    if (!write_ok) {
        // an existing rts file is reused as it is: never leave a truncated one
        sls_log(SLS_LOG_ERROR, "[%p]CTSFileTimeReader::generate_rts_file, write to file='%s' failed, '%s'; removed.",
                this, rts_file_name, write_errno ? strerror(write_errno) : "short write");
        ::unlink(rts_file_name);
        return SLS_ERROR;
    }
	sls_log(SLS_LOG_INFO, "[%p]CTSFileTimeReader::generate_rts_file, ok, file='%s'.\n",
			this, rts_file_name);

    return SLS_OK;
}



