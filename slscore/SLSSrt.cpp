
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
#include <string.h>
#include <map>
#include <string>
#include <memory.h>
#include <syslog.h>
#include <stdlib.h>
#include <atomic>

#include "SLSSrt.hpp"
#include "SLSLog.hpp"
#include "SLSLock.hpp"

/**
 * CSLSSrt class implementation
 */

bool CSLSSrt::m_inited = false;


CSLSSrt::CSLSSrt()
{
    memset(&m_sc, 0x0, sizeof(m_sc));
    m_sc.port = 8000;//for test
    m_sc.fd   = 0;
    m_sc.eid  = 0;
    m_sc.latency = 20;

    m_sc.backlog = 1024;
    memset(m_peer_name, 0, sizeof(m_peer_name));
    m_peer_port = 0;

    m_passphrase[0] = '\0';
    m_pbkeylen = 0;

}
CSLSSrt::~CSLSSrt()
{
}

// libsrt's own log. Its default logger writes every line to stderr, one or
// more per rejected handshake (1200 for 300 bad passphrases), outside every
// sls budget and straight into the journal under systemd. Lines at its
// warning level and above come here instead and go to the sls log, prefixed
// "libsrt:", through the peer-line budget: they are mostly caused by peers
// (a rejected handshake), and with sls's own refusal line they make one
// line a second, the rest at DEBUG and counted. Called on libsrt's threads:
// no libsrt call and no lock but the log's own.
static std::atomic<sls_bad_secret_observer_t> sls_bad_secret_observer(NULL);

void CSLSSrt::set_bad_secret_observer(sls_bad_secret_observer_t fn)
{
    sls_bad_secret_observer.store(fn);
}

// libsrt 1.5.4 (core.cpp, CUDT::processConnectRequest -> newConnection)
// logs "@<socket>:newConnection: connection rejected due to: <reason>"
// with "ERROR:BADSECRET" in the reason when the passphrase was wrong. The
// only signal there is: this depends on that text (and on a libsrt built
// with logging); without it the handshake rate limit is what remains.
static bool sls_parse_bad_secret(const char *line, int *ns)
{
    static const char tag[] = ":newConnection: connection rejected due to:";
    if (line[0] != '@' || line[1] < '0' || line[1] > '9') {
        return false;
    }
    char *end = NULL;
    long id = strtol(line + 1, &end, 10);
    if (NULL == end || 0 != strncmp(end, tag, sizeof(tag) - 1) || NULL == strstr(end, "BADSECRET")) {
        return false;
    }
    *ns = (int)id;
    return true;
}

static void sls_libsrt_log_handler(void *opaque, int level, const char *file, int line,
                                   const char *area, const char *message)
{
    (void)opaque; (void)file; (void)line; (void)area;
    // At most WARNING: these lines go through the peer budget because peers
    // cause them, and a remote peer must not be able to raise ERROR lines.
    int sls_level = level <= LOG_WARNING ? SLS_LOG_WARNING :
                    level == LOG_NOTICE ? SLS_LOG_INFO : SLS_LOG_DEBUG;
    // one line of printable ASCII (the text can quote what a peer sent):
    // other bytes as \xNN, as elsewhere in the sls log
    const char *p = message ? message : "";
    while (*p == ':' || *p == ' ') {
        p++;                 // what is left of the prefix the flags turned off
    }
    char buf[1024];
    size_t n = 0;
    for (; *p && n + 5 < sizeof(buf); p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\r' || c == '\t') {
            buf[n++] = ' ';
        } else if (c < 0x20 || c >= 0x7f) {
            n += snprintf(buf + n, sizeof(buf) - n, "\\x%02x", c);
        } else {
            buf[n++] = (char)c;
        }
    }
    while (n > 0 && buf[n - 1] == ' ') {
        n--;
    }
    buf[n] = 0;
    // The listener is non-blocking, so an accept with nothing queued is
    // normal (it ends every drain, see CSLSListener::on_added_to_epoll), but
    // libsrt logs it as an error: DEBUG, outside the peer budget.
    if (NULL != strstr(buf, "srt_accept: no pending connection available")) {
        sls_log(SLS_LOG_DEBUG, "libsrt: %s", buf);
        return;
    }
    int ns = 0;
    sls_bad_secret_observer_t obs = sls_bad_secret_observer.load();
    if (NULL != obs && sls_parse_bad_secret(buf, &ns)) {
        obs(ns);
    }
    sls_peer_log(sls_level, "libsrt: %s", buf);
}

int CSLSSrt::libsrt_init()
{
    if (m_inited)
        return SLS_OK;

    srt_setloglevel(LOG_WARNING);
    srt_setlogflags(SRT_LOGF_DISABLE_TIME | SRT_LOGF_DISABLE_THREADNAME |
                    SRT_LOGF_DISABLE_SEVERITY | SRT_LOGF_DISABLE_EOL);
    srt_setloghandler(NULL, sls_libsrt_log_handler);

    if (srt_startup() < 0) {
        return SLSERROR_UNKNOWN;
    }
    m_inited = true;
	return SLS_OK;
}

int CSLSSrt::libsrt_uninit()
{
    if (!m_inited)
        return SLS_OK;
    srt_cleanup();
	return SLS_OK;
}

int CSLSSrt::libsrt_epoll_create()
{
	return srt_epoll_create();
}

void CSLSSrt::libsrt_epoll_release(int eid)
{
	srt_epoll_release(eid);
}

void CSLSSrt::libsrt_print_error_info()
{
    /**
SRTS_BROKEN: The socket was connected, but the connection was broken
SRTS_CLOSING: The socket may still be open and active, but closing is requested, so no further operations will be accepted (active operations will be completed before closing)
SRTS_CLOSED: The socket has been closed, but not yet removed by the GC thread
SRTS_NONEXIST:
     */

#define set_error_map(k)\
        map_error[k] = std::string(#k);

    char szBuf[1024] = {0};
    std::map<int, std::string> map_error;

    set_error_map(SRTS_INIT);
    set_error_map(SRTS_OPENED);
    set_error_map(SRTS_LISTENING);
    set_error_map(SRTS_CONNECTING);
    set_error_map(SRTS_CONNECTED);
    set_error_map(SRTS_BROKEN);
    set_error_map(SRTS_CLOSING);
    set_error_map(SRTS_CLOSED);
    set_error_map(SRTS_NONEXIST);

    set_error_map(SRT_ENOCONN);
    set_error_map(SRT_ECONNLOST);
    set_error_map(SRT_EINVALMSGAPI);
    set_error_map(SRT_EINVALBUFFERAPI);
    set_error_map(SRT_EASYNCSND);
    set_error_map(SRT_ETIMEOUT);
    set_error_map(SRT_EPEERERR);

    printf("--------srt error--------\n");
    std::map<int, std::string>::iterator it;
    for(it=map_error.begin(); it!=map_error.end(); ++it) {
        snprintf(szBuf, sizeof(szBuf), "%d: %s\n", it->first, it->second.c_str());
        printf("%s", szBuf);
    }
    printf("----------end------------\n");
    map_error.clear();


}

int CSLSSrt::libsrt_lasterrno()
{
    return srt_getlasterror(NULL);
}

int CSLSSrt::libsrt_neterrno(bool peer)
{
    int err = srt_getlasterror(NULL);
    const char *msg = srt_getlasterror_str();

    switch (err) {
        case SRT_ECONNLOST:
        case SRT_ENOCONN:
            sls_log(SLS_LOG_DEBUG,
                    "CSLSSrt: peer disconnected (err=%d, %s).", err, msg);
            break;
        case SRT_ETIMEOUT:
            sls_log(SLS_LOG_DEBUG,
                    "CSLSSrt: peer idle timeout, no graceful close (err=%d, %s).",
                    err, msg);
            break;
        default:
            if (peer) {
                sls_peer_log(SLS_LOG_INFO,
                        "CSLSSrt::libsrt_neterrno, err=%d, %s.", err, msg);
            } else {
                sls_log(SLS_LOG_ERROR,
                        "CSLSSrt::libsrt_neterrno, err=%d, %s.", err, msg);
            }
            break;
    }
    return err;
}

void CSLSSrt::libsrt_set_context(SRTContext *sc)
{
    m_sc = *sc;
}

void CSLSSrt::libsrt_set_latency(int latency)
{
    m_sc.latency = latency;
}

void CSLSSrt::libsrt_set_passphrase(const char *passphrase, int pbkeylen)
{
    if (passphrase && *passphrase) {
        snprintf(m_passphrase, sizeof(m_passphrase), "%s", passphrase);
    } else {
        m_passphrase[0] = '\0';
    }
    m_pbkeylen = pbkeylen;
}

int CSLSSrt::libsrt_setup(int port)
{
    struct addrinfo hints = { 0 }, *ai;
    int fd = -1;
    int ret;
    char portstr[10];
    int timeout = 10;//ms
    int eid;
    SRTContext *s = &m_sc;

    m_sc.port = port;

    hints.ai_family = AF_INET;//AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(portstr, sizeof(portstr), "%d", s->port);
    hints.ai_flags |= AI_PASSIVE;
    ret = getaddrinfo(s->hostname[0] ? s->hostname : NULL, portstr, &hints, &ai);
    if (ret) {
        sls_log(SLS_LOG_ERROR,
               "[%p]CSLSSrt::libsrt_setup, Failed to resolve hostname %s: %s.",
               this, s->hostname, gai_strerror(ret));
        return ret;
    }

    fd = srt_create_socket();
    if (fd < 0) {
        ret = libsrt_neterrno();
        freeaddrinfo(ai);
        return ret;
    }

/*
    if (libsrt_setsockopt(h, fd, SRTO_STREAMID, "SRTO_STREAMID", sc->streamid, strlen(s->streamid)) < 0) {
        ret = libsrt_neterrno();
        freeaddrinfo(ai);
        return ret;
    }
*/

    /* Set the socket's send or receive buffer sizes, if specified.
       If unspecified or setting fails, system default is used. */
    if (s->latency > 0) {
        srt_setsockopt(fd, SOL_SOCKET, SRTO_LATENCY, &s->latency, sizeof (s->latency));
    }
    if (s->recv_buffer_size > 0) {
        srt_setsockopt(fd, SOL_SOCKET, SRTO_UDP_RCVBUF, &s->recv_buffer_size, sizeof (s->recv_buffer_size));
    }
    if (s->send_buffer_size > 0) {
        srt_setsockopt(fd, SOL_SOCKET, SRTO_UDP_SNDBUF, &s->send_buffer_size, sizeof (s->send_buffer_size));
    }
    if (s->reuse) {
        if (srt_setsockopt(fd, SOL_SOCKET, SRTO_REUSEADDR, &s->reuse, sizeof(s->reuse)))
            sls_log(SLS_LOG_WARNING, "[%p]CSLSSrt::libsrt_setup, setsockopt(SRTO_REUSEADDR) failed.", this);
    }

    if (m_passphrase[0] != '\0') {
        if (m_pbkeylen > 0) {
            if (srt_setsockopt(fd, 0, SRTO_PBKEYLEN, &m_pbkeylen, sizeof(m_pbkeylen)) < 0) {
                sls_log(SLS_LOG_ERROR, "[%p]CSLSSrt::libsrt_setup, setsockopt(SRTO_PBKEYLEN=%d) failed: %s.",
                        this, m_pbkeylen, srt_getlasterror_str());
                srt_close(fd);
                freeaddrinfo(ai);
                return libsrt_neterrno();
            }
        }
        if (srt_setsockopt(fd, 0, SRTO_PASSPHRASE, m_passphrase, strlen(m_passphrase)) < 0) {
            sls_log(SLS_LOG_ERROR, "[%p]CSLSSrt::libsrt_setup, setsockopt(SRTO_PASSPHRASE) failed: %s.",
                    this, srt_getlasterror_str());
            srt_close(fd);
            freeaddrinfo(ai);
            return libsrt_neterrno();
        }
        sls_log(SLS_LOG_INFO, "[%p]CSLSSrt::libsrt_setup, SRT encryption enabled (pbkeylen=%d).",
                this, m_pbkeylen);
    }

    ret = srt_bind(fd, ai->ai_addr, ai->ai_addrlen);
    if (ret) {
        srt_close(fd);
        freeaddrinfo(ai);
        return libsrt_neterrno();
    }

    s->fd = fd;

    freeaddrinfo(ai);
    sls_log(SLS_LOG_INFO, "[%p]CSLSSrt::libsrt_setup, fd=%d.", this, fd);

    return SLS_OK;
}

int CSLSSrt::libsrt_listen(int backlog)
{
    m_sc.backlog = backlog;
    int ret = srt_listen(m_sc.fd, backlog);
    if (ret)
        return libsrt_neterrno();

    // the INFO "libsrt_listen, ok" line is the listener's, once a worker polls it
    sls_log(SLS_LOG_DEBUG, "[%p]CSLSSrt::libsrt_listen, listening, fd=%d, port=%d, backlog=%d.", this, m_sc.fd, m_sc.port, backlog);
    return SLS_OK;
}

int CSLSSrt::libsrt_accept()
{
    struct sockaddr_in scl;
    int sclen = sizeof(scl);
    char ip[30] = {0};
    struct sockaddr_in * addrtmp;

    int new_sock = srt_accept(m_sc.fd, (struct sockaddr*)&scl, &sclen);//NULL, NULL);//(sockaddr*)&scl, &sclen);
    if (new_sock == SRT_INVALID_SOCK) {
        int err_no = libsrt_lasterrno();
        if (SRT_EASYNCRCV == err_no) {
            return SLS_ACCEPT_EMPTY;     // non-blocking listener, nothing queued
        }
        sls_peer_log(SLS_LOG_INFO, "[%p]CSLSSrt::libsrt_accept failed, sock=%d, error_no=%d (%s).",
                   this, m_sc.fd, err_no, srt_getlasterror_str());
        return SLS_ACCEPT_FAILED;
    }
    addrtmp = (struct sockaddr_in *)&scl;
    inet_ntop(AF_INET, &addrtmp->sin_addr, ip, sizeof(ip));
    // DEBUG: once per connection attempt; the listener logs the admitted ones
    sls_log(SLS_LOG_DEBUG, "[%p]CSLSSrt::libsrt_accept ok, new sock=%d, %s:%d.",
               this, new_sock, ip, ntohs(addrtmp->sin_port));

    return new_sock;
}

int CSLSSrt::libsrt_close()
{
    if (m_sc.fd) {
        // DEBUG: once per socket, including every refused attempt
        sls_log(SLS_LOG_DEBUG, "[%p]CSLSSrt::libsrt_close, fd=%d.", this, m_sc.fd);
      srt_close(m_sc.fd);
      m_sc.fd = 0;
    }
    return SLS_OK;
}

int CSLSSrt::libsrt_set_fd(int fd)
{
    libsrt_close();
    m_sc.fd = fd;
    return SLS_OK;
}


int CSLSSrt::libsrt_get_fd()
{
    return m_sc.fd;
}

int CSLSSrt::libsrt_set_eid(int eid)
{
    m_sc.eid = eid;
    return SLS_OK;
}


int CSLSSrt::libsrt_getsockopt(SRT_SOCKOPT optname, const char * optnamestr, void * optval, int * optlen, bool quiet)
{
    if (srt_getsockopt(m_sc.fd, 0, optname, optval, optlen) < 0) {
        if (quiet) {
            // the caller logs it, once, with the peer's address
        } else {
            sls_log(SLS_LOG_ERROR, "[%p]CSLSSrt::libsrt_getsockopt, failed to get option %s on socket: %s.", this, optnamestr, srt_getlasterror_str());
        }
        return SLSERROR(EIO);
    }
    return 0;
}

int CSLSSrt::libsrt_setsockopt(SRT_SOCKOPT optname, const char * optnamestr, const void * optval, int optlen)
{
    if (srt_setsockopt(m_sc.fd, 0, optname, optval, optlen) < 0) {
        sls_log(SLS_LOG_ERROR, "[%p]CSLSSrt::libsrt_setsockopt, failed to set option %s on socket: %s\n", this, optnamestr, srt_getlasterror_str());
        return SLSERROR(EIO);
    }
    return 0;
}


int CSLSSrt::libsrt_socket_nonblock(int enable)
{
    int ret = srt_setsockopt(m_sc.fd, 0, SRTO_SNDSYN, &enable, sizeof(enable));
    if (ret < 0)
        return ret;
    return srt_setsockopt(m_sc.fd, 0, SRTO_RCVSYN, &enable, sizeof(enable));
}

int CSLSSrt::libsrt_read(char *buf, int size)
{
    int ret;
    ret = srt_recvmsg(m_sc.fd, buf, size);
    if (ret < 0) {
        libsrt_neterrno(true);
    }
    return ret;
}

int CSLSSrt::libsrt_write(const char *buf, int size)
{
    int ret;
    ret = srt_sendmsg(m_sc.fd, buf, size, -1, 0);
    if (ret < 0) {//SRTS_BROKEN
        int err_no = libsrt_neterrno(true);
        // a player that left: the session end line carries the reason
        sls_log(SLS_LOG_DEBUG, "[%p]CSLSSrt::libsrt_write failed, sock=%d, ret=%d, errno=%d.",
                this, m_sc.fd, ret, err_no);
    }
    return ret;
}

int CSLSSrt::libsrt_add_to_epoll(int eid, bool write)
{
    int ret = SLS_OK;
    int fd = m_sc.fd;
    int modes = write ? SRT_EPOLL_OUT : SRT_EPOLL_IN;

    if (!eid) {
        sls_log(SLS_LOG_ERROR, "CSLSSrt::libsrt_add_to_epoll failed, m_eid=%d.", eid);
        return SLS_ERROR;
    }

    modes |= SRT_EPOLL_ERR;
    ret = srt_epoll_add_usock(eid, fd, &modes);
    if (ret < 0) {
        sls_log(SLS_LOG_ERROR, "CSLSSrt::libsrt_add_to_epoll, srt_epoll_add_usock failed, m_eid=%d, fd=%d, modes=%d.",
                eid, fd, modes);
        return libsrt_neterrno();
    }
    return ret;
}

int CSLSSrt::libsrt_remove_from_epoll()
{
    int ret = SLS_OK;
    int fd = m_sc.fd;
    int eid = m_sc.eid;

    if (!eid) {
        return SLS_ERROR;      // never added to an epoll: nothing to remove
    }

    ret = srt_epoll_remove_usock(eid, fd);
    if (ret < 0) {
        sls_log(SLS_LOG_DEBUG, "CSLSSrt::remove_from_epoll, srt_epoll_remove_usock failed, m_eid=%d, fd=%d, err=%d.",
                eid, fd, srt_getlasterror(NULL));
        return SLS_ERROR;
    }
    return ret;
}

int  CSLSSrt::libsrt_getsockstate()
{
    return srt_getsockstate(m_sc.fd);
}


int CSLSSrt::libsrt_getpeeraddr(char * peer_name, int& port)
{
    int ret = SLS_ERROR;
    struct sockaddr_in peer_addr;
    int peer_addr_len = sizeof(peer_addr);

    if (strlen(m_peer_name) == 0 || m_peer_port == 0) {
        ret = srt_getpeername(m_sc.fd, (struct sockaddr *)&peer_addr, &peer_addr_len);
        if (0 == ret) {
            inet_ntop(AF_INET, &peer_addr.sin_addr, m_peer_name, sizeof(m_peer_name));
            m_peer_port = ntohs(peer_addr.sin_port);

            snprintf(peer_name, IP_MAX_LEN, "%s", m_peer_name);
            port = m_peer_port;
            ret = SLS_OK;
        }
    } else {
        snprintf(peer_name, IP_MAX_LEN, "%s", m_peer_name);
        port = m_peer_port;
        ret = SLS_OK;
    }
    return ret;
}

