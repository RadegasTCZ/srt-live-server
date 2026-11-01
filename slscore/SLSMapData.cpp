
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


#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "SLSMapData.hpp"
#include "SLSLog.hpp"


/**
 * CSLSMapData class implementation
 */

CSLSMapData::CSLSMapData()
{
}
CSLSMapData::~CSLSMapData()
{
    clear();
}

int CSLSMapData::add(char *key)
{
    int ret = SLS_OK;
    std::string strKey = std::string(key);

    CSLSLock lock(&m_rwclock, true);

    std::map<std::string, CSLSRecycleArray *>::iterator item;
    item = m_map_array.find(strKey);
    if (item != m_map_array.end()) {
        CSLSRecycleArray * array_data = item->second;
        if (array_data) {
        	sls_log(SLS_LOG_INFO, "[%p]CSLSMapData::add, failed, key=%s, array_data=%p, exist.",
                    this, key, array_data);
        	return ret;
        }
        //m_map_array.erase(item);
    }

    CSLSRecycleArray * data_array = new CSLSRecycleArray;
    //m_map_array.insert(make_pair(strKey, data_array));
    m_map_array[strKey] = data_array;

    // a new ring is a new publisher: its PSI bootstrap starts empty
    std::map<std::string, CSLSTsParser *>::iterator item_ti = m_map_ts_info.find(strKey);
    if (item_ti != m_map_ts_info.end() && item_ti->second) {
        item_ti->second->reset();
    } else {
        m_map_ts_info[strKey] = new CSLSTsParser;
    }
    sls_log(SLS_LOG_DEBUG, "[%p]CSLSMapData::add ok, key='%s'.",
            this, key);
    return ret;
}

int CSLSMapData::remove(char *key)
{
    int ret = SLS_ERROR;
    std::string strKey = std::string(key);

    CSLSLock lock(&m_rwclock, true);

    std::map<std::string, CSLSRecycleArray *>::iterator item;
    item = m_map_array.find(strKey);
    if (item != m_map_array.end()) {
        CSLSRecycleArray * array_data = item->second;
        sls_log(SLS_LOG_DEBUG, "[%p]CSLSMapData::remove, key='%s' delete array_data=%p.",
                this, key, array_data);
        if (array_data) {
            delete array_data;
        }
    	m_map_array.erase(item);
        ret = SLS_OK;
    }

    // Drop cached PSI (PAT/PMT/SPS/PPS) so the next publisher on the same
    // stream key seeds fresh metadata from its own packets. Without this,
    // pullers joining after a codec or track-layout change receive the
    // previous publisher's PMT and decode the wrong elementary stream.
    std::map<std::string, CSLSTsParser *>::iterator item_ti;
    item_ti = m_map_ts_info.find(strKey);
    if (item_ti != m_map_ts_info.end()) {
        delete item_ti->second;
        m_map_ts_info.erase(item_ti);
    }
    return ret;
}

bool CSLSMapData::is_exist(char *key)
{

    CSLSLock lock(&m_rwclock, true);
    std::string strKey = std::string(key);

    std::map<std::string, CSLSRecycleArray *>::iterator item;
    item = m_map_array.find(key);
    if (item != m_map_array.end()) {
    	CSLSRecycleArray * array_data = item->second;
    	if (array_data) {
            sls_log(SLS_LOG_TRACE, "[%p]CSLSMapData::is_exist, key=%s, exist.",
                this, key);
            return true;
    	} else {
            sls_log(SLS_LOG_TRACE, "[%p]CSLSMapData::is_exist, is_exist, key=%s, data_array is null.",
                this, key);
    	}
    } else {
        sls_log(SLS_LOG_TRACE, "[%p]CSLSMapData::add, is_exist, key=%s, not exist.",
            this, key);
    }
    return false;
}


int CSLSMapData::put(char *key, char *data, int len, int64_t *last_read_time)
{
    int ret = SLS_OK;

    CSLSLock lock(&m_rwclock, true);
    std::string strKey = std::string(key);

    std::map<std::string, CSLSRecycleArray *>::iterator item;
    item = m_map_array.find(strKey);
    if (item == m_map_array.end()) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSMapData::put, key=%s, not found data array.",
                this, key);
        return SLS_ERROR;
    }
    CSLSRecycleArray *array_data = item->second;
    if (NULL == array_data) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSMapData::get, key=%s, array_data is NULL.",
                this, key);
    }

    ret = array_data->put(data, len);
    if (ret != len) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSMapData::put, key=%s, array_data->put failed, len=%d, but ret=%d.",
                this, key, len, ret);
    }
    if (NULL != last_read_time) {
        *last_read_time = array_data->get_last_read_time();
    }

    // PSI bootstrap: parse only until it is complete, then no per-packet work
    CSLSTsParser *parser = NULL;
    std::map<std::string, CSLSTsParser *>::iterator item_ti;
    item_ti = m_map_ts_info.find(strKey);
    if (item_ti == m_map_ts_info.end() || NULL == item_ti->second) {
        parser = new CSLSTsParser;
        m_map_ts_info[strKey] = parser;
    } else {
        parser = item_ti->second;
    }

    if (!parser->complete() && SLS_OK == check_ts_info(data, len, parser)) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSMapData::put, PSI bootstrap ready, key=%s, video pid=%d, stream_type=0x%02x.",
                this, key, parser->video_pid(), parser->video_stream_type());
    }

    return ret;
}

int CSLSMapData::get(char *key, char *data, int len, SLSRecycleArrayID *read_id, int aligned)
{
    int ret = SLS_OK;

    CSLSLock lock(&m_rwclock, false);
    std::string strKey = std::string(key);

    std::map<std::string, CSLSRecycleArray *>::iterator item;
    item = m_map_array.find(strKey);
    if (item == m_map_array.end()) {
        sls_log(SLS_LOG_TRACE, "[%p]CSLSMapData::get, key=%s, not found data array,",
                this, key);
        return SLS_ERROR;
    }
    CSLSRecycleArray *array_data = item->second;
    if (NULL == array_data) {
        sls_log(SLS_LOG_WARNING, "[%p]CSLSMapData::get, key=%s, array_data is NULL.",
                this, key);
        return SLS_ERROR;
    }

    // The array resyncs the reader (first read, new publisher's ring, or
    // lapped) and then returns no data; the reader restarts at the write
    // position, so it gets the PSI bootstrap first.
    bool resynced = false;
    ret = array_data->get(data, len, read_id, aligned, &resynced);
    if (resynced) {
        ret = get_ts_info(key, data, len);
        sls_log(SLS_LOG_INFO, "[%p]CSLSMapData::get, reader resynced, key=%s, bootstrap len=%d.",
                    this, key, ret);
    }
    return ret;
}

// The bootstrap, padded with null packets to one TS_UDP_LEN unit: players
// send whole TS_UDP_LEN units and refill their buffer below that.
// 0 (players get live data) while the bootstrap is incomplete.
int CSLSMapData::get_ts_info(char *key, char *data, int len)
{
    std::string strKey = std::string(key);
    std::map<std::string, CSLSTsParser *>::iterator item_ti;
    item_ti = m_map_ts_info.find(strKey);
    if (item_ti == m_map_ts_info.end() || NULL == item_ti->second || len < TS_UDP_LEN)
        return 0;
    uint8_t *out = (uint8_t *)data;
    int n = item_ti->second->build_bootstrap(out, TS_UDP_LEN);
    if (n <= 0)
        return 0;
    for (; n < TS_UDP_LEN; n += TS_PACK_LEN) {
        out[n]     = TS_SYNC_BYTE;
        out[n + 1] = 0x1F;          // PID 0x1FFF
        out[n + 2] = 0xFF;
        out[n + 3] = 0x10;          // payload only, CC 0
        memset(out + n + 4, 0xFF, TS_PACK_LEN - 4);
    }
    return TS_UDP_LEN;
}

void CSLSMapData::add_reader(const char *key)
{
    CSLSLock lock(&m_readers_lock);
    m_map_readers[std::string(key)]++;
}

void CSLSMapData::remove_reader(const char *key)
{
    CSLSLock lock(&m_readers_lock);
    std::map<std::string, int>::iterator it = m_map_readers.find(std::string(key));
    if (it != m_map_readers.end() && --it->second <= 0) {
        m_map_readers.erase(it);
    }
}

int CSLSMapData::reader_count(const char *key)
{
    CSLSLock lock(&m_readers_lock);
    std::map<std::string, int>::iterator it = m_map_readers.find(std::string(key));
    return it == m_map_readers.end() ? 0 : it->second;
}

void CSLSMapData::clear()
{
    CSLSLock lock(&m_rwclock, true);
    std::map<std::string, CSLSRecycleArray *>::iterator it;
    for(it=m_map_array.begin(); it!=m_map_array.end(); ) {
        CSLSRecycleArray * array_data = it->second;
        if (array_data) {
            delete array_data;
        }
        it ++;
    }
    m_map_array.clear();

    std::map<std::string, CSLSTsParser *>::iterator it_ti;
    for (it_ti = m_map_ts_info.begin(); it_ti != m_map_ts_info.end(); ++it_ti) {
        delete it_ti->second;
    }
    m_map_ts_info.clear();
}

// data holds whole 188-byte packets (the publisher role only puts those);
// a trailing partial packet would be skipped. SLS_OK once complete.
int CSLSMapData::check_ts_info(char *data, int len, CSLSTsParser *parser)
{
    for (int i = 0; i + TS_PACK_LEN <= len && !parser->complete(); i += TS_PACK_LEN) {
        parser->feed((const uint8_t *)data + i);
    }
    return parser->complete() ? SLS_OK : SLS_ERROR;
}


