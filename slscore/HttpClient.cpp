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
#include <poll.h>
#include <string.h>

#include "HttpClient.hpp"
#include "SLSLog.hpp"

//	 GET /sls?on_event=on_connect HTTP/1.1
#define HTTP_REQUEST_HEADER_ACCEPT         "Accept: text/html, */*\r\n"
#define HTTP_REQUEST_HEADER_USER_AGENT     "User-Agent: srt-live-server\r\n"
#define HTTP_REQUEST_HEADER_CONTENT_TYPE   "Content-Type: application/json\r\n"
// one request per connection: the response ends at its length, its last chunk, or the close
#define HTTP_REQUEST_HEADER_CONNECTION     "Connection: close\r\n"
#define HTTP_REQUEST_HEADER_CACHE_CONTROL  "Cache-Control: no-cache\r\n"

/**
 * CHttpClient class implementation
 */

CHttpClient::CHttpClient()
{
    memset(m_http_method, 0, 32);
    memset(&m_addr, 0, sizeof(m_addr));

    m_addr_len     = 0;
    m_http_port    = 80;
    m_out_pos      = 0;
    m_out_data_len = 0;
    m_begin_tm_ms  = 0;
    m_end_tm_ms    = 0;
    m_timeout      = 5000;//default 5 s, in ms
    m_interval     = 0; //unit s
    m_id           = SLS_HTTP_INVALID_CLIENT_ID;
    m_status       = 0;
    m_complete     = false;
    m_failed       = false;

    m_callback         = NULL;
    m_callback_context = NULL;

    m_response_info.m_response_content_length    = -1;
    sprintf(m_role_name, "http_client");
    sprintf(m_http_method, "GET");

}

CHttpClient::~CHttpClient()
{
    close();
}

void CHttpClient::reset_response()
{
    m_response_info.m_response_header.clear();
    m_response_info.m_response_code.clear();
    m_response_info.m_response_content.clear();
    m_response_info.m_response_content_length   = -1;
    m_resp_buf.clear();
    m_status    = 0;
    m_complete  = false;
    m_failed    = false;
    m_end_tm_ms = 0;
}

int  CHttpClient::open(const char *url, const struct sockaddr_storage *addr, socklen_t addr_len, const char *method, int interval)
{
    int ret = SLS_ERROR;

    m_begin_tm_ms = sls_gettime_ms();
    reset_response();
    m_out_array.clear();
    m_out_pos      = 0;
    m_out_data_len = 0;

    if (NULL == url || url[0] == '\0') {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::open, failed, url is empty.", this);
        goto FUNC_END;
    }
    m_url = url;
    if (NULL != method && strlen(method) > 0) {
        snprintf(m_http_method, sizeof(m_http_method), "%s", method);
    }
    m_interval = interval;

    if (SLS_OK != parse_url()) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::open, parse_url failed.", this);
        goto FUNC_END;
    }
    if (NULL == addr || addr_len == 0 || addr_len > sizeof(m_addr)) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::open, failed, no resolved address, remote_host='%s', remote_port=%d.",
                this, m_http_host.c_str(), m_http_port);
        goto FUNC_END;
    }
    if (addr != &m_addr) {
        memcpy(&m_addr, addr, addr_len);
    }
    m_addr_len = addr_len;

    ret = CTCPRole::open(&m_addr, m_addr_len);
    if (SLS_OK != ret) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::open, failed, remote_host='%s', remote_port=%d.",
                this, m_http_host.c_str(), m_http_port);
        goto FUNC_END;
    }

    ret = generate_http_request();
    if (SLS_OK != ret) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::open, generate_http_request failed, remote_host='%s', remote_port=%d.",
                this, m_http_host.c_str(), m_http_port);
        goto FUNC_END;
    }
    //send data
    handler();
FUNC_END:
    if (SLS_OK != ret) {
        // a client that could not even start has its (negative) result at once
        m_failed = true;
    }
    if (NULL != m_callback) {
        m_callback(this, HCT_OPEN, &ret, m_callback_context);
    }
    return ret;
}

int  CHttpClient::close()
{
    int ret = SLS_OK;
    sls_log(SLS_LOG_TRACE, "[%p]CHttpClient::close, m_url='%s', status=%d.",
            this, m_url.c_str(), m_status);
    ret = CTCPRole::close();
    if (NULL != m_callback) {
        m_callback(this, HCT_CLOSE, &ret, m_callback_context);
    }
    reset_response();
    m_out_array.clear();
    return ret;
}


int  CHttpClient::reopen()
{
    // open() rewrites m_url / m_http_method: pass copies, not the members themselves
    std::string url = m_url;
    char method[sizeof(m_http_method)];
    snprintf(method, sizeof(method), "%s", m_http_method);
    close();
    return open(url.c_str(), &m_addr, m_addr_len, method, m_interval);
}

// SLS_OK once the request is over: it finished, or it ran out of time (then it
// has failed unless a status line had already arrived).
int  CHttpClient::check_timeout(int64_t cur_tm_ms)
{
    if (m_fd <= 0) {
        return SLS_OK;
    }
    if (m_end_tm_ms > 0) {
        return SLS_OK;
    }
    if (cur_tm_ms == 0){
        cur_tm_ms = sls_gettime_ms();
    }
    if (cur_tm_ms - m_begin_tm_ms < m_timeout) {
        return SLS_ERROR;
    }
    if (0 == m_status) {
        m_failed = true;
    }
    sls_log(SLS_LOG_INFO, "[%p]CHttpClient::check_timeout, timed out after %dms, url='%s', http_method='%s', status=%d.",
            this, m_timeout, m_url.c_str(), m_http_method, m_status);
    finish();
    return SLS_OK;
}

int  CHttpClient::check_repeat(int64_t cur_tm_ms)
{
    if (m_interval <= 0) {
        //no repeat
        return SLS_ERROR;
    }
    if (cur_tm_ms == 0){
        cur_tm_ms = sls_gettime_ms();
    }

    int64_t d = cur_tm_ms - m_begin_tm_ms;
    if ( d < (int64_t)m_interval*1000) {
        return SLS_ERROR;
    }
    return SLS_OK;
}

void  CHttpClient::set_stage_callback(HTTP_CALLBACK callback, void *context)
{
    m_callback         = callback;
    m_callback_context = context;
}

void CHttpClient::set_timeout(int ms)
{
    m_timeout = ms;
}

void CHttpClient::set_id(uint32_t client_id)
{
    m_id  = client_id;
}

uint32_t CHttpClient::get_id()
{
    return m_id;
}

HTTP_RESPONSE_INFO *CHttpClient::get_response_info()
{
    return &m_response_info;
}

// SLS_OK: nothing more will arrive (response complete, or the connection is gone)
int CHttpClient::check_finished()
{
    if (m_complete || m_fd <= 0 || !m_valid) {
        return SLS_OK;
    }
    return SLS_ERROR;
}

int CHttpClient::get_status()
{
    return m_status;
}

// the outcome is known: a status line arrived, or the request failed (connect
// error, EOF before a response, malformed response, timeout)
bool CHttpClient::has_result()
{
    return m_status > 0 || m_failed;
}

// the request is over, whatever the outcome; fires HCT_RESPONSE_END once
void CHttpClient::finish()
{
    if (m_end_tm_ms > 0) {
        return;
    }
    m_end_tm_ms = sls_gettime_ms();
    if (NULL != m_callback) {
        m_callback(this, HCT_RESPONSE_END, &m_response_info, m_callback_context);
    }
}

void CHttpClient::parse_response()
{
    int    status = 0;
    size_t header_len = 0;
    long   content_length = -1;
    bool   chunked = false;

    int r = sls_http_parse_response(m_resp_buf, &status, &header_len, &content_length, &chunked);
    if (r < 0) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::parse_response, malformed or oversized response, url='%s'.",
                this, m_url.c_str());
        m_failed = true;
        finish();
        return;
    }
    if (status > 0) {
        if (0 == m_status) {
            m_status = status;
            char code[16];
            snprintf(code, sizeof(code), "%d", status);
            m_response_info.m_response_code = code;
            sls_split_string(m_resp_buf.substr(0, header_len - 4), "\r\n", m_response_info.m_response_header);
            sls_log(SLS_LOG_INFO, "[%p]CHttpClient::parse_response, status=%d, url='%s', http_method='%s'.",
                    this, status, m_url.c_str(), m_http_method);
        }
        m_response_info.m_response_content_length = (int)content_length;
        m_response_info.m_response_content        = m_resp_buf.substr(header_len);   // raw (still chunk-framed if chunked)
    }
    if (1 == r) {
        m_complete = true;
        finish();
    }
}

int CHttpClient::recv()
{
    char data[HTTP_DATA_SIZE];
    bool got = false;
    // one byte past the parser's cap is enough for it to reject an oversized response
    while (m_resp_buf.size() <= HTTP_MAX_RESPONSE) {
        int n = read(data, sizeof(data));
        if (n <= 0) {
            break;
        }
        m_resp_buf.append(data, n);
        got = true;
    }
    if (got && m_end_tm_ms == 0) {
        parse_response();
    }
    if (!m_valid && m_end_tm_ms == 0) {
        // the peer closed: that ends a close-delimited body; with no status line it is a failure
        if (0 == m_status) {
            m_failed = true;
        } else {
            m_complete = true;
        }
        finish();
    }
    return SLS_OK;
}

int CHttpClient::send()
{
    int ret = SLS_OK;
    while (true)
    {
        if (m_out_data_len <= 0) {
            if (m_out_array.count() == 0) {
                break;
            }
            m_out_data_len = m_out_array.get(m_out_data, HTTP_DATA_SIZE);
            if (m_out_data_len <= 0){
                m_out_pos       = 0;
                m_out_data_len  = 0;
                break;
            }
            m_out_pos = 0;
        }

        int n = write((char*)m_out_data + m_out_pos, m_out_data_len);
        if (n <= 0) {
            break;
        }
        ret += n;
        if (n < m_out_data_len) {
            //remainder, maybe net is busy, break
            m_out_pos      += n;
            m_out_data_len -= n;
            break;
        } else {
            m_out_pos       = 0;
            m_out_data_len  = 0;
            continue;
        }
    }
    return ret;
}

int CHttpClient::handler(DATA_PARAM *p)
{
    int ret = SLS_OK;
    if (p->readable) {
        ret = recv();
    } else if (p->writable) {
        ret = send();
    }
    return ret;
}

// Called from the worker's loop: never blocks (poll with a zero timeout).
int CHttpClient::handler()
{
    if (!m_valid || m_fd <= 0)
        return SLS_OK;

    struct pollfd pfd;
    pfd.fd      = m_fd;
    pfd.events  = POLLIN | POLLOUT;
    pfd.revents = 0;
    int ret = poll(&pfd, 1, 0);
    if (ret == 0) {
        return SLS_OK;
    }
    if (ret < 0) {
        if (errno == EINTR) {
            return SLS_OK;
        }
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::handler, poll ret=%d, errno=%d, err='%s'",
                this, ret, errno, strerror(errno));
        m_valid = false;
        m_failed = true;
        finish();
        return SLS_ERROR;
    }

    //write is ready? (also tells whether the connect worked)
    if (pfd.revents & POLLOUT) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (0 != getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &err, &len) || 0 != err) {
            sls_log(SLS_LOG_INFO, "[%p]CHttpClient::handler, connect failed, url='%s', err=%d.", this, m_url.c_str(), err);
            m_valid = false;
            m_failed = true;
            finish();
            return SLS_ERROR;
        }
        send();
    }
    //read is ready? (also reports EOF / reset)
    if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
        recv();
    }
    if (!m_valid && !has_result()) {
        m_failed = true;        // a write error left the request half sent
        finish();
    }
    return SLS_OK;
}

int CHttpClient::parse_url()
{
    //http://hostname:port/sls?on_event=on_connect&srt_url=...
    if (SLS_OK != sls_parse_http_url(m_url.c_str(), &m_http_host, &m_http_port, &m_uri)) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::parse_url, not a usable http url='%s'.", this, m_url.c_str());
        return SLS_ERROR;
    }
    return SLS_OK;
}

int CHttpClient::write_http_header(int data_len)
{
    if (strcmp(m_http_method, "GET") != 0 && strcmp(m_http_method, "POST") != 0) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::write_http_header, wrong method='%s'.",
            this, m_http_method);
        return SLS_ERROR;
    }
    std::string http_header = std::string(m_http_method) + " " + m_uri + " HTTP/1.1\r\n";

    http_header += HTTP_REQUEST_HEADER_ACCEPT;
    http_header += HTTP_REQUEST_HEADER_USER_AGENT;
    if (data_len > 0) {
        http_header += HTTP_REQUEST_HEADER_CONTENT_TYPE;
    }

    std::string host = m_http_host;
    if (host.find(':') != std::string::npos) {
        host = "[" + host + "]";                  // IPv6 literal
    }
    if (m_http_port != 80) {
        host += ":" + std::to_string(m_http_port);
    }
    http_header += "Host: " + host + "\r\n";

    if (data_len > 0) {
        http_header += "Content-Length: " + std::to_string(data_len) + "\r\n";
    }

    http_header += HTTP_REQUEST_HEADER_CONNECTION;
    http_header += HTTP_REQUEST_HEADER_CACHE_CONTROL;
    http_header += "\r\n";

    int len = http_header.length();
    int ret = m_out_array.put((const uint8_t*)(http_header.c_str()), len);
    if (len != ret) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::write_http_header, failed, m_out_array.put len=%d, ret=%d, url='%s', http_method='%s'.",
            this, len, ret, m_url.c_str(), m_http_method);
        return SLS_ERROR;
    }
    return ret;
}

int CHttpClient::write_string(std::string *str)
{
    if (!m_valid) {
        return SLS_ERROR;
    }
    if (!str) {
        return SLS_ERROR;
    }

    int len = str->length();
    if (len > 0) {
        int ret = m_out_array.put(((const uint8_t*)str->c_str()), len);
        if (len != ret) {
            sls_log(SLS_LOG_INFO, "[%p]CHttpClient::write_string, failed, m_out_array.put len=%d, ret=%d.",
                this, len, ret);
            return SLS_ERROR;
        }
    }
    return SLS_OK;
}

void CHttpClient::get_request_data()
{
    m_request_data.clear();
    if (m_callback) {
        m_callback(this, HCT_REQUEST_CONTENT, &m_request_data, m_callback_context);
    }
}

int CHttpClient::generate_http_request()
{
    int ret = SLS_ERROR;
    get_request_data();
    int post_data_len = m_request_data.length();

    ret = write_http_header(post_data_len);
    if (0 >= ret) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::generate_http_request, write_http_header failed, remote_host='%s', remote_port=%d.",
                this, m_http_host.c_str(), m_http_port);
        return SLS_ERROR;
    }

    if (post_data_len > 0) {
        //post content
        ret = write_string(&m_request_data);
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::generate_http_request, write_post_content, ret=%d, remote_host='%s', remote_port=%d.",
                this, ret, m_http_host.c_str(), m_http_port);
        if (SLS_OK != ret) {
            return SLS_ERROR;
        }
    }
    sls_log(SLS_LOG_INFO, "[%p]CHttpClient::generate_http_request, ok, m_url='%s', content len=%d.",
            this, m_url.c_str(), post_data_len);
    return SLS_OK;
}
