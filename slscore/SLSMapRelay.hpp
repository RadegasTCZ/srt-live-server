
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


#ifndef _SLSMapRelay_INCLUDE_
#define _SLSMapRelay_INCLUDE_

#include <map>
#include <string>

#include "SLSRelayManager.hpp"
#include "SLSLock.hpp"

class CSLSMapRelay
{
public:
	CSLSMapRelay();
    virtual ~CSLSMapRelay();

    // Find or create the manager for app_uplive/stream_name. The result
    // carries a reference for the caller (release() it when done); NULL if
    // the app has no relay conf or max_pull_streams / max_push_streams managers
    // already exist for it. *created (optional) tells whether this call made it.
    CSLSRelayManager *add_relay_manager(const char *app_uplive, const char *stream_name,
                                        bool *created = NULL);
    // Drop the map's entry (and its reference) for app_uplive/stream_name;
    // with expected set, only if the entry is that manager. SLS_OK if removed.
    int  remove_relay_manager(const char *app_uplive, const char *stream_name,
                              CSLSRelayManager *expected = NULL);
    void clear();

    int add_relay_conf(std::string app_uplive, sls_conf_relay_t * cr);
    // Main thread only (blocking DNS): re-resolve the upstreams of every relay
    // block a failed open marked stale. The relay blocks are fixed between
    // start-up and stop(), both on the main thread, so no lock is needed.
    // With worker_threads 0 the main thread is also the only serving thread,
    // so this DNS blocks serving for its duration.
    void refresh_stale_upstreams();
    SLS_RELAY_INFO *get_relay_conf(std::string app_uplive);

private:
    CSLSRWLock          m_rwclock;
    std::map<std::string, CSLSRelayManager *>   m_map_relay_manager;        //stream_name: relay_manager

    std::map<std::string, SLS_RELAY_INFO *>     m_map_relay_info;        //uplive: relay_conf_info
};


#endif
