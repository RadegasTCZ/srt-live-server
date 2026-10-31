
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


#include <stdio.h>


#include "SLSRecycleArray.hpp"
#include "SLSLog.hpp"

const int DEFAULT_MAX_DATA_SIZE = 1024*1316;//about 5mbps*2sec

// Generation 0 is never handed out, so a zeroed SLSRecycleArrayID never
// matches any array.
std::atomic<uint64_t> CSLSRecycleArray::s_next_generation(1);

CSLSRecycleArray::CSLSRecycleArray()
{
    m_nDataSize     = DEFAULT_MAX_DATA_SIZE;
    m_nWritePos     = 0;
    m_nDataCount    = 0;
    m_generation    = s_next_generation.fetch_add(1);

    m_last_read_time = sls_gettime_ms();

    // zeroed: no reader may ever see another allocation's heap contents
    m_arrayData      = new char[m_nDataSize]();

}

CSLSRecycleArray::~CSLSRecycleArray()
{
    CSLSLock lock(&m_rwclock, true);
    if (m_arrayData != NULL) {
        delete[] m_arrayData;
        m_arrayData = NULL;
    }
}

int CSLSRecycleArray::count()
{
    CSLSLock lock(&m_rwclock, false);
    return (int)m_nDataCount;
}

//please call this function before get and put.
//Existing readers are resynced (new generation) on their next get.
void CSLSRecycleArray::setSize(int n)
{
    CSLSLock lock(&m_rwclock, true);
    delete[] m_arrayData ;
    m_nDataSize      = n;
    m_nWritePos      = 0;
    m_nDataCount     = 0;
    m_generation     = s_next_generation.fetch_add(1);
    m_arrayData      = new char[m_nDataSize]();
}

int CSLSRecycleArray::put(char * data, int len)
{
    if (!data || len <= 0) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSRecycleArray::put, failed, data=%p, len=%d.",
                this, data, len);
        return SLS_ERROR;
    }

    if (len > m_nDataSize) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSRecycleArray::put, failed, len=%d is bigger than m_nDataSize=%d.",
                this, len, m_nDataSize);
        return SLS_ERROR;
    }

    int     write_pos;
    int64_t data_count;
    {
        CSLSLock lock(&m_rwclock, true);
        if (m_nDataSize - m_nWritePos >= len) {
            //copy directly
            memcpy(m_arrayData + m_nWritePos, data, len);
            m_nWritePos += len;
        } else {
            int first_len = m_nDataSize - m_nWritePos;
            memcpy(m_arrayData + m_nWritePos, data, first_len);
            memcpy(m_arrayData, data + first_len, len - first_len);
            m_nWritePos = (len - first_len);
        }

        if (m_nWritePos==m_nDataSize)
        	m_nWritePos = 0;
        // under the lock: readers compare it with their cursor for laps
        m_nDataCount += len;
        write_pos  = m_nWritePos;
        data_count = m_nDataCount;
    }
    sls_log(SLS_LOG_TRACE, "[%p]CSLSRecycleArray::put, len=%d, m_nWritePos=%d, m_nDataCount=%lld, m_nDataSize=%d.",
            this, len, write_pos, (long long)data_count, m_nDataSize);
    return len;
}

int CSLSRecycleArray::get(char *data, int size, SLSRecycleArrayID *read_id, int aligned, bool *resynced)
{
    if (NULL != resynced) {
        *resynced = false;
    }
    if (NULL == read_id) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSRecycleArray::get, failed, read_id is NULL.", this);
        return SLS_ERROR;
    }

    // Everything below reads the writer's state, so it happens under the
    // read lock -- including the first-time positioning.
    CSLSLock lock(&m_rwclock, false);
    if (NULL == m_arrayData) {
        sls_log(SLS_LOG_INFO, "[%p]CSLSRecycleArray::get, failed, m_arrayData is NULL.", this);
        return SLS_ERROR;
    }

    int64_t ready = m_nDataCount - read_id->nDataCount;
    const char *why = NULL;
    if (read_id->bFirst) {
        why = "first read";
    } else if (read_id->nGeneration != m_generation) {
        why = "ring replaced (new publisher)";
    } else if (ready < 0 || ready > (int64_t)(m_nDataSize - TS_UDP_LEN)) {
        why = "reader lapped";
    } else if (read_id->nReadPos < 0 || read_id->nReadPos >= m_nDataSize) {
        why = "bad read position";
    }
    if (NULL != why) {
        sls_log(read_id->bFirst ? SLS_LOG_TRACE : SLS_LOG_INFO,
                "[%p]CSLSRecycleArray::get, resync reader to write pos=%d: %s (reader was %lld bytes behind).",
                this, m_nWritePos, why, read_id->bFirst || read_id->nGeneration != m_generation ? 0LL : (long long)ready);
        read_id->nReadPos    = m_nWritePos;
        read_id->nDataCount  = m_nDataCount;
        read_id->nGeneration = m_generation;
        read_id->bFirst      = false;
        if (NULL != resynced) {
            *resynced = true;
        }
        return 0;
    }

    if (0 == ready) {
        sls_log(SLS_LOG_TRACE, "[%p]CSLSRecycleArray::get, no new data.", this);
        return SLS_OK;
    }
    sls_log(SLS_LOG_TRACE, "[%p]CSLSRecycleArray::get, read_id->nReadPos=%d, m_nWritePos=%d, ready=%lld, m_nDataSize=%d.",
            this, read_id->nReadPos, m_nWritePos, (long long)ready, m_nDataSize);

    //update the last read time
    m_last_read_time = sls_gettime_ms();

    int copy_data_len = ready <= (int64_t)size ? (int)ready : size;
    if (aligned > 0) {
        copy_data_len = copy_data_len/aligned * aligned;
    }
    if (copy_data_len > 0) {
        int tail = m_nDataSize - read_id->nReadPos;
        if (tail >= copy_data_len) {
            //no wrap round
            memcpy(data, m_arrayData + read_id->nReadPos, copy_data_len);
            read_id->nReadPos += copy_data_len;
        } else {
            //wrap around
            memcpy(data, m_arrayData + read_id->nReadPos, tail);
            memcpy(data + tail, m_arrayData, copy_data_len - tail);
            read_id->nReadPos = copy_data_len - tail;
        }
        if (read_id->nReadPos == m_nDataSize)
            read_id->nReadPos = 0;
        read_id->nDataCount += copy_data_len;
    }
    sls_log(SLS_LOG_TRACE, "[%p]CSLSRecycleArray::get, copy_data_lens=%d.",
    		this, copy_data_len);
    return copy_data_len;
}

int64_t CSLSRecycleArray::get_last_read_time()
{
	return m_last_read_time;
}



