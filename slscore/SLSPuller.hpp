
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


#ifndef _SLSPuller_INCLUDE_
#define _SLSPuller_INCLUDE_

#include "SLSRelay.hpp"

/**
 * CSLSPuller
 */
class CSLSPuller: public CSLSRelay
{
public :
	CSLSPuller();
    virtual ~CSLSPuller();

    virtual int  uninit();
    virtual int  handler();
    // closed because no player read it for idle_streams_timeout
    bool         closed_for_idle() const { return m_closed_for_idle; }
protected:
    bool         m_closed_for_idle;
    int64_t      m_last_reader_tm;    // ms, last time a player was attached
    int64_t      m_reader_check_tm;   // ms, last reader_count() look-up

};


#endif
