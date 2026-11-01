
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
#include <string.h>
#include <stddef.h>
#include <strings.h>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "SLSLog.hpp"
#include "SLSLock.hpp"

CSLSLog* CSLSLog::m_pInstance = NULL;

CSLSLog::CSLSLog()
{
	m_level = SLS_LOG_INFO;
	m_log_file   = NULL;
    log_filename[0] = '\0';
}
CSLSLog::~CSLSLog()
{
    if (m_log_file) {
        fflush(m_log_file);
        fclose(m_log_file);
        m_log_file = NULL;
    }
}
int CSLSLog::create_instance()
{
    if (!m_pInstance) {
    	m_pInstance = new CSLSLog();
    	m_pInstance->m_level = SLS_LOG_ERROR;//default
		return 0;
    }
	return -1;
}
int CSLSLog::destory_instance()
{
	SAFE_DELETE(m_pInstance);
	return 0;
}

void CSLSLog::log(int level, const char *fmt, ...)
{
    if (!m_pInstance) {
        m_pInstance = new CSLSLog();
    }
    if (level > m_pInstance->m_level) {
        return;
    }

	va_list vl;
    va_start(vl, fmt);
	m_pInstance->print_log(level, fmt, vl);
	va_end (vl);
}


// sls_peer_log budget, process-wide: the time of the last peer line logged
// and the number held back since. Lock-free: callers include libsrt's
// receive thread, which must never wait on a worker.
static std::atomic<int64_t> s_peer_last_ms(0);
static std::atomic<int>     s_peer_held(0);

bool CSLSLog::peer_line_allowed(int level, int *held)
{
    *held = 0;
    if (!m_pInstance) {
        m_pInstance = new CSLSLog();
    }
    if (level > m_pInstance->m_level) {
        return false;            // not logged at this level anyway: no budget used
    }
    int64_t now  = sls_gettime_ms();
    int64_t last = s_peer_last_ms.load();
    if (now - last < 1000 || !s_peer_last_ms.compare_exchange_strong(last, now)) {
        s_peer_held++;
        return false;
    }
    *held = s_peer_held.exchange(0);
    return true;
}

// sls_session_log budget: a fixed one-second window, process-wide. The window
// start and the count in it are separate atomics; a race at a window edge can
// let a line or two more than the limit through, never unbounded.
static std::atomic<int64_t> s_sess_window_ms(0);
static std::atomic<int>     s_sess_count(0);
static std::atomic<int>     s_sess_held(0);

bool CSLSLog::session_line_allowed(int level, int *held)
{
    *held = 0;
    if (!m_pInstance) {
        m_pInstance = new CSLSLog();
    }
    if (level > m_pInstance->m_level) {
        return false;            // not logged at this level anyway: no budget used
    }
    int64_t now = sls_gettime_ms();
    int64_t w   = s_sess_window_ms.load();
    if (now - w >= 1000 && s_sess_window_ms.compare_exchange_strong(w, now)) {
        s_sess_count.store(0);
    }
    if (s_sess_count.fetch_add(1) >= SLS_SESSION_LINES_PER_SEC) {
        s_sess_held++;
        return false;
    }
    *held = s_sess_held.exchange(0);
    return true;
}

void CSLSLog::peer_flush()
{
    int64_t now = sls_gettime_ms();
    if (s_sess_held.load() > 0 && now - s_sess_window_ms.load() >= 1000) {
        // the window has run out: nothing more will carry the count
        int held = s_sess_held.exchange(0);
        if (held > 0) {
            log(SLS_LOG_INFO, "session lines: %d suppressed in the last second(s) (start/end lines over %d/s; log_level debug shows each).",
                held, SLS_SESSION_LINES_PER_SEC);
        }
    }
    if (0 == s_peer_held.load()) {
        return;
    }
    int64_t last = s_peer_last_ms.load();
    if (now - last < 1000 || !s_peer_last_ms.compare_exchange_strong(last, now)) {
        return;
    }
    int held = s_peer_held.exchange(0);
    if (held > 0) {
        log(SLS_LOG_INFO, "peer lines: %d held back in the last %.1f s (refusals of connections and requests, at most 1 line/s; log_level debug shows each).",
            held, (now - last) / 1000.0);
    }
}

void CSLSLog::print_log(int level, const char *fmt, va_list vl)
{
    CSLSLock lock(&m_mutex);
    char buf[4096] = {0};
    char buf_info[4096] = {0};
    char cur_time[STR_DATE_TIME_LEN] = {0};
    int64_t cur_time_msec = sls_gettime_ms();
    int64_t cur_time_sec = cur_time_msec/1000;
    cur_time_msec = cur_time_msec - cur_time_sec*1000;
    sls_gettime_fmt(cur_time, cur_time_sec, "%Y-%m-%d %H:%M:%S");
    vsnprintf (buf , 4095 , fmt , vl);
    //sprintf(buf_info, "%s %s: %s\n" , cur_time, LOG_LEVEL_NAME[level], buf);
    snprintf(buf_info, sizeof(buf_info), "%s:%03d %s %s: %s\n" , cur_time, (int)cur_time_msec, APP_NAME, LOG_LEVEL_NAME[level], buf);
    printf("%s", buf_info);

    if (m_log_file) {
        fwrite(buf_info, strlen(buf_info), 1, m_log_file);
        // the file is block-buffered: make sure errors survive a crash
        if (level <= SLS_LOG_ERROR) {
            fflush(m_log_file);
        }
    }
}

void CSLSLog::set_log_level(char * level)
{
    if (!m_pInstance)
    	m_pInstance = new CSLSLog();

    level = sls_strupper(level);//to upper
    int n = sizeof(LOG_LEVEL_NAME)/sizeof(char*);
    for (int i = 0; i < n; i ++) {
        if (strcmp(level, LOG_LEVEL_NAME[i]) == 0) {
            m_pInstance->m_level = i;
            printf("set log level='%s'.\n" , level);
            return ;
        }
    }
    printf("!!!wrong log level '%s', set default '%s'.\n" , level, LOG_LEVEL_NAME[m_pInstance->m_level]);
}

// The log file is opened while sls may still be root (the privilege drop comes
// later), so a path that an unprivileged user can swap for a symlink, or a file
// that belongs to someone else, must not be followed or appended to. Like the
// pid file: O_NOFOLLOW, then fstat on the open descriptor, and fail closed.
int CSLSLog::set_log_file(char * file_name)
{
    if (!m_pInstance)
        m_pInstance = new CSLSLog();

    if (strlen(m_pInstance->log_filename) != 0) {
        return 0;                // already open
    }
    int fd = open(file_name, O_WRONLY|O_APPEND|O_CREAT|O_NOFOLLOW|O_CLOEXEC, 0640);
    if (fd < 0) {
        printf("%s ERROR: cannot open log_file '%s' for appending: %s. sls does not follow a symlink for this file; "
               "set 'log_file' in the srt block to a writable path in an existing directory.\n",
               APP_NAME, file_name, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid()) {
        printf("%s ERROR: log_file '%s' is not a plain file owned by this user with a single link; refusing to use it. "
               "If it is a leftover, delete it and start again.\n", APP_NAME, file_name);
        close(fd);
        return -1;
    }
    FILE *fp = fdopen(fd, "a");
    if (!fp) {
        printf("%s ERROR: cannot use log_file '%s': %s.\n", APP_NAME, file_name, strerror(errno));
        close(fd);
        return -1;
    }
    snprintf(m_pInstance->log_filename, sizeof(m_pInstance->log_filename), "%s", file_name);
    m_pInstance->m_log_file = fp;
    return 0;
}

