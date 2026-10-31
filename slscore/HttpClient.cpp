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

#define HTTP_HEADER_SIZE        1024

//	 POST /sls?method=stat HTTP/1.1
#define HTTP_REQUEST_HEADER_ACCEPT         "Accept: text/html, */*\r\n"
//   Accept: text/html, */*
#define HTTP_REQUEST_HEADER_USER_AGENT     "User-Agent: srt-live-server\r\n"
#define HTTP_REQUEST_HEADER_CONTENT_TYPE   "Content-Type: application/x-www-form-urlencoded\r\n"
//   Host: localhost:8080
//   Content-Length: 15
#define HTTP_REQUEST_HEADER_CONNECTION     "Connection: Keep-Alive\r\n"
//
#define HTTP_REQUEST_HEADER_CACHE_CONTROL  "Cache-Control: no-cache\r\n"
//   Cache-Control: no-cache

/**
 * CHttpClient class implementation
 */

CHttpClient::CHttpClient()
{
    memset(m_url, 0, URL_MAX_LEN);
    memset(m_uri, 0, URL_MAX_LEN);
    memset(m_remote_host, 0, 1024);
    memset(m_http_method, 0, 32);


    m_remote_port  = 80;
    m_out_pos      = 0;
    m_out_data_len = 0;
    m_begin_tm_ms  = 0;
    m_end_tm_ms    = 0;
    m_timeout      = 5000;//default 5 s, in ms
    memset(&m_addr, 0, sizeof(m_addr));
    m_addr_len     = 0;
    m_interval     = 0; //unit s
    m_id           = SLS_HTTP_INVALID_CLIENT_ID;

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

int  CHttpClient::open(const char *url, const struct sockaddr_storage *addr, socklen_t addr_len, const char *method, int interval)
{
    m_begin_tm_ms = sls_gettime_ms();
    int ret = SLS_OK;
	if (NULL == url) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::open, failed, url is NULL.", this);
		ret = SLS_ERROR;
		goto FUNC_END;
	}
	snprintf(m_url, sizeof(m_url), "%s", url);
	if (m_url[0] == '\0') {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::open, failed, m_url='%s'.", this, m_url);
		ret = SLS_ERROR;
		goto FUNC_END;
	}
	if (NULL != method && strlen(method) > 0) {
		snprintf(m_http_method, sizeof(m_http_method), "%s", method);
	}

	m_interval = interval;

    ret = parse_url();
    if (SLS_OK != ret) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::open, parse_url failed.", this);
		goto FUNC_END;
    }

    if (NULL == addr || addr_len == 0 || addr_len > sizeof(m_addr)) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::open, failed, no resolved address, remote_host='%s', remote_port=%d.",
        		this, m_remote_host, m_remote_port);
        ret = SLS_ERROR;
		goto FUNC_END;
    }
    if (addr != &m_addr) {
        memcpy(&m_addr, addr, addr_len);
    }
    m_addr_len = addr_len;
    ret = CTCPRole::open(&m_addr, m_addr_len);
    if (SLS_OK != ret) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::open, failed, remote_host='%s', remote_port=%d.",
        		this, m_remote_host, m_remote_port);
		goto FUNC_END;
    }
	//clear response
    m_response_info.m_response_header.clear();
    m_response_info.m_response_content.clear();
    m_response_info.m_response_content_length   = -1;
	m_end_tm_ms = 0;
	m_out_array.clear();

    ret = generate_http_request();
    if (SLS_OK != ret) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::open, generate_http_request failed, remote_host='%s', remote_port=%d.",
        		this, m_remote_host, m_remote_port);
		goto FUNC_END;
    }
    //send data
    handler();
FUNC_END:
    if (NULL != m_callback) {
		m_callback(this, HCT_OPEN, &ret, m_callback_context);
    }
    return ret;
}

int  CHttpClient::close()
{
	int ret = SLS_OK;
    sls_log(SLS_LOG_TRACE, "[%p]CHttpClient::close, m_url='%s', m_response_content_length=%d.",
    		this, m_url, m_response_info.m_response_content_length);
    ret = CTCPRole::close();
	if (NULL != m_callback) {
		m_callback(this, HCT_CLOSE, &ret, m_callback_context);
	}
    m_response_info.m_response_header.clear();
    m_response_info.m_response_content.clear();
    m_response_info.m_response_content_length   = -1;
	m_end_tm_ms = 0;
	m_out_array.clear();
	return ret;
}


int  CHttpClient::reopen()
{
	 // open() rewrites m_url / m_http_method: pass copies, not the members themselves
	 char url[URL_MAX_LEN];
	 char method[sizeof(m_http_method)];
	 snprintf(url, sizeof(url), "%s", m_url);
	 snprintf(method, sizeof(method), "%s", m_http_method);
	 close();
     return open(url, &m_addr, m_addr_len, method, m_interval);
}


int  CHttpClient::check_timeout(int64_t cur_tm_ms)
{
	if (0 == m_fd) {
		return SLS_OK;
	}

	if (m_end_tm_ms > 0) {
		if (m_response_info.m_response_content_length == m_response_info.m_response_content.length())
		    return SLS_ERROR;
		return SLS_OK;
	}
	if (cur_tm_ms == 0){
		cur_tm_ms = sls_gettime_ms();
	}

    int64_t d = cur_tm_ms - m_begin_tm_ms;
    if ( d < m_timeout) {
    	return SLS_ERROR;
    }
    m_end_tm_ms = cur_tm_ms;
	if (NULL != m_callback) {
		m_callback(this, HCT_RESPONSE_END, &m_response_info, m_callback_context);
	}
	sls_log(SLS_LOG_INFO, "[%p]CHttpClient::check_timeout, ok, url='%s', http_method='%s', content_len=%zu, m_response_content_length=%d.",
    		this, m_url, m_http_method, m_response_info.m_response_content.length(), m_response_info.m_response_content_length);

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

    int d = cur_tm_ms - m_begin_tm_ms;
    if ( d < m_interval*1000) {
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

int CHttpClient::check_finished()
{
	if (m_response_info.m_response_content_length == m_response_info.m_response_content.length()) {
		return SLS_OK;
	}
	if (0 == m_fd) {
		return SLS_OK;
	}
   return SLS_ERROR;
}

int CHttpClient::parse_http_response(std::string &response)
{
	if (m_response_info.m_response_header.size() > 0) {
		m_response_info.m_response_content += response;
		if (m_response_info.m_response_content_length == m_response_info.m_response_content.length()) {
        	sls_log(SLS_LOG_INFO, "[%p]CHttpClient::parse_http_response, finished, url='%s', http_method='%s', content_len=%zu.",
            		this, m_url, m_http_method, m_response_info.m_response_content.length());
			m_end_tm_ms = sls_gettime_ms();
			if (NULL != m_callback) {
				m_callback(this, HCT_RESPONSE_END, &m_response_info, m_callback_context);
			}
		}
	    return SLS_OK;
	}

    std::vector<std::string> response_parts;
    sls_split_string(response, "\r\n\r\n", response_parts, 1);
    if (response_parts.size() == 0) {
        sls_log(SLS_LOG_TRACE, "[%p]CHttpClient::parse_http_response, failed, sls_split_string, not found '\r\n\r\n', continue.",
        		this);
    	return SLS_ERROR;
    }

    sls_split_string(response_parts[0], "\r\n", m_response_info.m_response_header);
    if (m_response_info.m_response_header.size() > 0) {
        //HTTP/1.1 200 OK
    	std::vector<std::string> parts;
    	sls_split_string(m_response_info.m_response_header[0], " ", parts);
    	if (parts.size() == 3) {
    		m_response_info.m_response_code = parts[1];//
        	sls_log(SLS_LOG_INFO, "[%p]CHttpClient::parse_http_response, m_response_code:'%s', url='%s', http_method='%s'.",
            		this, m_response_info.m_response_code.c_str(), m_url, m_http_method);
    	}

    	//Content-Length
		std::string dst = std::string("Content-Length:");
		std::string content_length = sls_find_string(m_response_info.m_response_header, dst);
		if (content_length.length() > 0) {
	    	sls_split_string(content_length, ":", parts, 1);
		    if (parts.size() == 2) {
				m_response_info.m_response_content_length = atoi(parts[1].c_str());
		    	sls_log(SLS_LOG_INFO, "[%p]CHttpClient::parse_http_response, m_response_content_length=%d.",
		        		this, m_response_info.m_response_content_length);
		    }
		}

    }

    if (response_parts.size() == 2) {
    	m_response_info.m_response_content = response_parts[1];
    	if (m_response_info.m_response_content_length == m_response_info.m_response_content.length()) {
    		m_end_tm_ms = sls_gettime_ms();
    		if (NULL != m_callback) {
    			m_callback(this, HCT_RESPONSE_END, &m_response_info, m_callback_context);
    		}
        	sls_log(SLS_LOG_INFO, "[%p]CHttpClient::parse_http_response, finished, url='%s', http_method='%s', content_len=%zu.",
            		this, m_url, m_http_method, m_response_info.m_response_content.length());
    		return SLS_OK;
    	}
    }

    return SLS_OK;
}

int CHttpClient::recv()
{
	int ret = SLS_OK;
	//response data
	char data[HTTP_DATA_SIZE+1] = {0};
	int data_size = HTTP_DATA_SIZE;
	std::string http_response;
	while (true) {
		//just handle string type data
        int n = read(data, data_size);
        if (n <= 0) {
        	break;
        }
        data[n] = 0;
        http_response += std::string(data);
	}
	if (http_response.length() > 0) {
		//parse the response
        ret = parse_http_response(http_response);
        if (SLS_OK == check_finished()) {
            sls_log(SLS_LOG_INFO, "[%p]CHttpClient::recv, finished.",
            		this);
        }
	}
    return ret;
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
        } else if (n > m_out_data_len) {
            m_out_pos       = 0;
            m_out_data_len  = 0;
            sls_log(SLS_LOG_WARNING, "[%p]CHttpClient::send, write failed, m_url='%s', m_out_data_len=%d, but n=%d.",
            		this, m_url, m_out_data_len, n);
            break;
        } else {
        	//continue;
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
        return SLS_ERROR;
    }

    //write is ready?
    if (pfd.revents & POLLOUT) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (0 != getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &err, &len) || 0 != err) {
            sls_log(SLS_LOG_INFO, "[%p]CHttpClient::handler, connect failed, url='%s', err=%d.", this, m_url, err);
            m_valid = false;
            return SLS_ERROR;
        }
        send();
    }
    //read is ready? (also reports EOF / reset)
    if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
        recv();
    }
    return SLS_OK;
}



int CHttpClient::parse_url()
{
    //http://hostname:port/sls?on_event=on_connect&srt_url=...
    std::string host;
    std::string uri;
    int         port = 0;
    if (SLS_OK != sls_parse_http_url(m_url, &host, &port, &uri)) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::parse_url, not a usable http url='%s'.", this, m_url);
        return SLS_ERROR;
    }
    snprintf(m_remote_host, sizeof(m_remote_host), "%s", host.c_str());
    snprintf(m_uri, sizeof(m_uri), "%s", uri.c_str());
    m_remote_port = port;
    return SLS_OK;
}

int CHttpClient::write_http_header(int data_len)
{
	std::string http_header;
	char data[HTTP_HEADER_SIZE] = {0};

	if (strcmp(m_http_method, "GET") != 0 && strcmp(m_http_method, "POST") != 0) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::write_http_header, wrong method='%s'.",
            this, m_http_method);
		return SLS_ERROR;
	}
	snprintf(data, sizeof(data), "%s %s HTTP/1.1\r\n", m_http_method, m_uri);
	http_header = std::string(data);

	http_header += std::string(HTTP_REQUEST_HEADER_ACCEPT);
	http_header += std::string(HTTP_REQUEST_HEADER_USER_AGENT);
	http_header += std::string(HTTP_REQUEST_HEADER_CONTENT_TYPE);

	snprintf(data, sizeof(data), "Host: %s\r\n", m_remote_host);
	http_header += std::string(data);

	if (data_len > 0) {
	    snprintf(data, sizeof(data), "Content-Length: %d\r\n", data_len);
	    http_header += std::string(data);
	}

	http_header += std::string(HTTP_REQUEST_HEADER_CONNECTION);
	http_header += std::string(HTTP_REQUEST_HEADER_CACHE_CONTROL);
	http_header += std::string("\r\n");

	//int ret = write(http_header.c_str(), http_header.length());
	int len = http_header.length();
	int ret = m_out_array.put((const uint8_t*)(http_header.c_str()), len);
	if (len != ret) {
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::write_http_header, failed, m_out_array.put len=%d, ret=%d, url='%s', http_method='%s'.",
            this, len, ret, m_url, m_http_method);
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
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::generate_http_request, write_get_header failed, remote_host='%s', remote_port=%d.",
        		this, m_remote_host, m_remote_port);
    	return ret;
    }

    if (post_data_len > 0) {
        //post content
        ret = write_string(&m_request_data);
        sls_log(SLS_LOG_INFO, "[%p]CHttpClient::generate_http_request, write_post_content, ret=%d, remote_host='%s', remote_port=%d.",
        		this, ret, m_remote_host, m_remote_port);
    }
    sls_log(SLS_LOG_INFO, "[%p]CHttpClient::generate_http_request, ok, m_url='%s', content len=%d.",
    		this, m_url, post_data_len);
    return SLS_OK;
}
