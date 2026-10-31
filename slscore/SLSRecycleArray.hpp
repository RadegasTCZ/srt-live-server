
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


#ifndef _SLSRecycleArray_INCLUDE_
#define _SLSRecycleArray_INCLUDE_

#include <list>
#include <atomic>
#include <stdint.h>
#include <string.h>

#include "common.hpp"
#include "SLSLock.hpp"

// Per-reader cursor into a CSLSRecycleArray. Owned and touched only by the
// reading role; the array validates it under its own read lock.
//   nReadPos    - byte offset of the next read in the ring
//   nDataCount  - total bytes the reader has consumed (compared with the
//                 array's m_nDataCount to detect "no new data" and laps)
//   nGeneration - generation of the array the cursor belongs to; a cursor
//                 from a replaced array never matches the new one
//   bFirst      - not positioned yet
struct SLSRecycleArrayID
{
    int       nReadPos;
    int64_t   nDataCount;
    uint64_t  nGeneration;
    bool      bFirst;
};


/**
 * CSLSRecycleArray
 */
class CSLSRecycleArray
{
public :
    CSLSRecycleArray();
    ~CSLSRecycleArray();

public :
    int  put(char *data, int len);
    // Copies up to size bytes (rounded down to a multiple of aligned) of data
    // the reader hasn't seen. If the cursor is unpositioned (bFirst), belongs
    // to another array (generation), or was lapped by the writer, the reader
    // is moved to the write position, *resynced is set to true and 0 is
    // returned; otherwise *resynced is false.
    int  get(char *dst, int size, SLSRecycleArrayID *read_id, int aligned, bool *resynced);

    void setSize(int n);
    int  count();

    int64_t get_last_read_time();
private:
    char     *m_arrayData;
    int       m_nDataSize;
    int64_t   m_nDataCount;     // total bytes ever put (this generation)
    int       m_nWritePos;
    uint64_t  m_generation;     // process-wide unique, see s_next_generation
    std::atomic<int64_t> m_last_read_time;  // written by concurrent readers

    static std::atomic<uint64_t> s_next_generation;


    CSLSRWLock m_rwclock;


};



#endif
