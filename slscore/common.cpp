
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


#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <cstdarg>
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sys/file.h>
#include <limits.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <string>
#include <vector>


#include "common.hpp"
#include "SLSStreamId.hpp"
#include "SLSLog.hpp"


/**
* sls_format
 */
std::string sls_format(const char *pszFmt, ...)
{
    std::string str;
    /*
    va_list args;
    va_start(args, pszFmt);
    {
        int nLength = vscprintf(pszFmt, args);
        nLength += 1;  //include \0
        std::vector<char> vectorChars(nLength);
        vsnprintf(vectorChars.data(), nLength, pszFmt, args);
        str.assign(vectorChars.data());
    }
    va_end(args);
    */
    return str;
}

#define HAVE_GETTIMEOFDAY 1

int64_t sls_gettime_ms(void)//rturn millisecond
{
	return sls_gettime()/1000;
}

int64_t sls_gettime(void)//rturn micro-second
{
#if HAVE_GETTIMEOFDAY
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
#elif HAVE_GETSYSTEMTIMEASFILETIME
    FILETIME ft;
    int64_t t;
    GetSystemTimeAsFileTime(&ft);
    t = (int64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return t / 10 - 11644473600000000; /* Jan 1, 1601 */
#else
    return -1;
#endif
}

void sls_gettime_default_string(char *cur_time)
{
	if (NULL == cur_time) {
		return ;
	}
    int64_t cur_time_sec = sls_gettime()/1000000;
    sls_gettime_fmt(cur_time, cur_time_sec, "%Y-%m-%d %H:%M:%S");
}

void sls_gettime_fmt(char *dst, int64_t cur_time_sec, char *fmt)
{
    time_t rawtime;
    struct tm * timeinfo;
    char timef[32] = {0};

    time (&rawtime);
    rawtime = (time_t)cur_time_sec;
    timeinfo = localtime (&rawtime);
    strftime(timef, sizeof(timef), fmt, timeinfo);
    strcpy(dst, timef);
    return ;
}

char * sls_strupper(char * str)
{
    char *orign=str;
    for (; *str!='\0'; str++)
        *str = toupper(*str);
    return orign;
}

#define sls_hash(key, c)   ((uint32_t) key * 31 + c)
uint32_t sls_hash_key(const char *data, int len)
{
	//copy form ngx
    uint32_t  i, key;

    key = 0;

    for (i = 0; i < len; i++) {
        key = sls_hash(key, data[i]);
    }
    return key;
}

int sls_resolve(const char *host, int port, struct sockaddr_storage *out, socklen_t *out_len, int family)
{
    if (NULL == host || '\0' == host[0] || NULL == out || NULL == out_len || port < 1 || port > 65535) {
        return SLS_ERROR;
    }
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char service[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%d", port);

    int rc = getaddrinfo(host, service, &hints, &res);
    if (0 != rc || NULL == res) {
        sls_log(SLS_LOG_ERROR, "sls_resolve: cannot resolve host '%s': %s.", host, gai_strerror(rc));
        if (res) freeaddrinfo(res);
        return SLS_ERROR;
    }
    if (res->ai_addrlen > sizeof(struct sockaddr_storage)) {
        freeaddrinfo(res);
        return SLS_ERROR;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out, res->ai_addr, res->ai_addrlen);
    *out_len = res->ai_addrlen;
    freeaddrinfo(res);
    return SLS_OK;
}

int sls_parse_http_url(const char *url, std::string *host, int *port, std::string *uri)
{
    static const char scheme[] = "http://";
    if (NULL == url || NULL == host || NULL == port || NULL == uri) {
        return SLS_ERROR;
    }
    if (0 != strncasecmp(url, scheme, sizeof(scheme) - 1)) {
        return SLS_ERROR;                       // only plain http is supported
    }
    std::string rest(url + sizeof(scheme) - 1);
    size_t frag = rest.find('#');
    if (frag != std::string::npos) {
        rest.erase(frag);
    }
    size_t end = rest.find_first_of("/?");
    std::string authority = rest.substr(0, end);
    std::string path = (end == std::string::npos) ? std::string() : rest.substr(end);
    if (path.empty() || path[0] == '?') {
        path = "/" + path;                      // a base URL without a path gets '/'
    }

    size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        authority.erase(0, at + 1);
    }
    std::string h;
    std::string p;
    if (!authority.empty() && authority[0] == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos) {
            return SLS_ERROR;
        }
        h = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return SLS_ERROR;
            }
            p = authority.substr(close + 2);
        }
    } else {
        size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            h = authority;
        } else {
            h = authority.substr(0, colon);
            p = authority.substr(colon + 1);
        }
    }
    if (h.empty()) {
        return SLS_ERROR;
    }
    int port_num = 80;
    if (!p.empty()) {
        if (p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) {
            return SLS_ERROR;
        }
        port_num = atoi(p.c_str());
        if (port_num < 1 || port_num > 65535) {
            return SLS_ERROR;
        }
    }
    *host = h;
    *port = port_num;
    *uri  = path;
    return SLS_OK;
}

static void av_str_replace(char *buf, const char dst, const char ch)
{
    char *p = NULL;
    while(1) {
        p = strchr(buf, dst);
        if (p == NULL)
            break;
        *p++ = ch;
    }
}
static size_t max_alloc_size= 1024000;// max 1M

static void *av_malloc(size_t size)
{
    void *ptr = NULL;

    /* let's disallow possibly ambiguous cases */
    if (size > (max_alloc_size - 32))
        return NULL;

#if HAVE_POSIX_MEMALIGN
    if (size) //OS X on SDK 10.6 has a broken posix_memalign implementation
    if (posix_memalign(&ptr, ALIGN, size))
        ptr = NULL;
#elif HAVE_ALIGNED_MALLOC
    ptr = _aligned_malloc(size, ALIGN);
#elif HAVE_MEMALIGN
#ifndef __DJGPP__
    ptr = memalign(ALIGN, size);
#else
    ptr = memalign(size, ALIGN);
#endif
    /* Why 64?
     * Indeed, we should align it:
     *   on  4 for 386
     *   on 16 for 486
     *   on 32 for 586, PPro - K6-III
     *   on 64 for K7 (maybe for P3 too).
     * Because L1 and L2 caches are aligned on those values.
     * But I don't want to code such logic here!
     */
    /* Why 32?
     * For AVX ASM. SSE / NEON needs only 16.
     * Why not larger? Because I did not see a difference in benchmarks ...
     */
    /* benchmarks with P3
     * memalign(64) + 1          3071, 3051, 3032
     * memalign(64) + 2          3051, 3032, 3041
     * memalign(64) + 4          2911, 2896, 2915
     * memalign(64) + 8          2545, 2554, 2550
     * memalign(64) + 16         2543, 2572, 2563
     * memalign(64) + 32         2546, 2545, 2571
     * memalign(64) + 64         2570, 2533, 2558
     *
     * BTW, malloc seems to do 8-byte alignment by default here.
     */
#else
    ptr = malloc(size);
#endif

    if (ptr)
        memset(ptr, 0, size);
    return ptr;
}
static void av_free(void *arg)
{
    //memcpy(arg, &(void *){ NULL }, sizeof(arg));
    free(arg);
}

static char *av_strdup(const char *s)
{
    char *ptr = NULL;
    if (s) {
        size_t len = strlen(s) + 1;
        ptr = (char *)av_malloc(len);
        if (ptr)
            memcpy(ptr, s, len);
    }
    return ptr;
}

/**
 * Locale-independent conversion of ASCII characters to lowercase.
 */
static inline int av_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        c ^= 0x20;
    return c;
}

static int av_strncasecmp(const char *a, const char *b, size_t n)
{
    uint8_t c1, c2;
    if (n <= 0)
        return 0;
    do {
        c1 = av_tolower(*a++);
        c2 = av_tolower(*b++);
    } while (--n && c1 && c1 == c2);
    return c1 - c2;
}

int sls_mkdir_p(const char *path)
{

    int ret = 0;
    char *temp = av_strdup(path);
    char *pos = temp;
    char tmp_ch = '\0';

    if (!path || !temp) {
        return -1;
    }

    if (!av_strncasecmp(temp, "/", 1) || !av_strncasecmp(temp, "\\", 1)) {
        pos++;
    } else if (!av_strncasecmp(temp, "./", 2) || !av_strncasecmp(temp, ".\\", 2)) {
        pos += 2;
    }

    for ( ; *pos != '\0'; ++pos) {
        if (*pos == '/' || *pos == '\\') {
            tmp_ch = *pos;
            *pos = '\0';
            ret = mkdir(temp, 0755);
            *pos = tmp_ch;
        }
    }

    if ((*(pos - 1) != '/') || (*(pos - 1) != '\\')) {
        ret = mkdir(temp, 0755);
    }

    av_free(temp);
    return ret;
}

void sls_remove_marks(char * s) {
    int len = strlen(s);
    if (len < 2)//pair
        return;

    if ((s[0] == '\'' && s[len-1] == '\'')
     || (s[0] == '"' && s[len-1] == '"')) {
        for(int i=0; i < len-2; i ++) {
            s[i] = s[i+1];
        }
        s[len-2] = 0x0;
    }
}

// Legacy wrapper: a streamid component is safe iff it passes the shared
// streamid rule (SLSStreamId.cpp). Kept so there is only one rule.
bool sls_is_safe_name(const char *s)
{
    return s && sls_sid_part_ok(std::string(s));
}

// SRT caps SRTO_PASSPHRASE at 10-79 printable bytes. A zero-length value
// means "no encryption for this direction" and is accepted as such.
// Anything between 1 and 9 bytes (or >79) is silently truncated or rejected
// by libsrt at handshake time -- validate up-front so operators get a clear
// error instead of mysterious handshake failures.
bool sls_is_valid_passphrase_len(size_t len)
{
    return len == 0 || (len >= 10 && len <= 79);
}

// SRTO_PBKEYLEN accepts 0 (libsrt default), 16, 24, or 32.
bool sls_is_valid_pbkeylen(int pbkeylen)
{
    return pbkeylen == 0 || pbkeylen == 16 || pbkeylen == 24 || pbkeylen == 32;
}

// ---- pid file -------------------------------------------------------------
// Design (see doc/Directives.md "pid_file"):
//  * The kernel's lock table is the source of truth, never the file's contents.
//    A running server holds an fcntl write lock on the pid file; `sls -s` asks
//    F_GETLK who holds it and signals THAT pid (after checking it is an sls
//    binary). No holder => stale file => nothing is signalled. The pid written
//    inside is informational only, so a stale, garbage or tampered file cannot
//    redirect a signal.
//  * The file is 0600 and must live in a directory only root / the service user
//    can write to. sls never creates directories and never follows a symlink at
//    the final component (O_NOFOLLOW).
//  * Startup has no check-then-act step: open (never unlink, never O_EXCL), then
//    one atomic F_SETLK. A leftover file with no lock holder is simply reused.
//  * The server never unlinks the file at exit: removing a path that another
//    starting instance may already have opened would let two instances each
//    hold "the" lock on different inodes. The lock dies with the process.

// Returns a read-only fd holding an fcntl read lock on the pid file (>= 0), or
// -1. The caller keeps it open until exit and must never open the path again in
// this process: closing ANY fd of the file drops the process's locks on it.
// The writable fd is closed here, before the caller drops privileges, so the
// unprivileged daemon cannot rewrite the file.
//
// Stale-handle reasoning: someone may still hold an fd on an old inode. The
// fstat check refuses any pre-existing file with group/other bits, and `-s`
// only signals a lock holder whose /proc/<pid>/exe is this sls binary, so such
// a handle can at worst block startup or `-s`; both fail closed.
int sls_write_pid(const char *path, int pid)
{
    if (NULL == path || '\0' == path[0]) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: no pid file path.");
        return -1;
    }
    int fd = open(path, O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC, 0600);
    if (fd < 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: cannot open pid file '%s' for writing: %s. "
                "sls does not create directories or follow symlinks for this file; "
                "set 'pid_file' in the srt block of the conf to a writable path, or create its directory.",
                path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: pid file '%s' is not a private (mode 0600) plain file owned by this user "
                "with a single link; refusing to use it. If it is a stale leftover, delete it and start again.", path);
        close(fd);
        return -1;
    }

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;   // l_start = l_len = 0: whole file
    if (fcntl(fd, F_SETLK, &fl) != 0) {
        if (errno == EACCES || errno == EAGAIN) {
            struct flock who;
            memset(&who, 0, sizeof(who));
            who.l_type = F_WRLCK; who.l_whence = SEEK_SET;
            int holder = (fcntl(fd, F_GETLK, &who) == 0 && who.l_type != F_UNLCK) ? (int)who.l_pid : 0;
            sls_log(SLS_LOG_ERROR, "sls_write_pid: another instance is running (lock holder pid %d) with pid file '%s'.", holder, path);
        } else {
            sls_log(SLS_LOG_ERROR, "sls_write_pid: cannot lock pid file '%s': %s.", path, strerror(errno));
        }
        close(fd);
        return -1;
    }

    if (ftruncate(fd, 0) != 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: cannot truncate pid file '%s': %s.", path, strerror(errno));
        close(fd);
        return -1;
    }
    char buf[32];
    int  len = snprintf(buf, sizeof(buf), "%d\n", pid);
    int  off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            sls_log(SLS_LOG_ERROR, "sls_write_pid: write to pid file '%s' failed: %s.", path, strerror(errno));
            close(fd);
            return -1;
        }
        off += (int)n;
    }
    if (fsync(fd) != 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: fsync of pid file '%s' failed: %s.", path, strerror(errno));
        close(fd);
        return -1;
    }

    // Swap to a read-only fd: open it, check it is the same inode, close the
    // writable one (which releases our lock), then re-lock with a read lock.
    // Between the close and the F_SETLK another instance B may take the write
    // lock, write ITS pid, swap to its own read lock and run. Read locks do not
    // conflict, so our F_RDLCK can succeed too. We therefore read the file back
    // through the locked fd and require our own pid: if A's read lock succeeded,
    // either B's write already finished (B's pid is in the file, A exits) or
    // B's F_SETLK comes later and conflicts with A's read lock (B exits). Exactly
    // one instance survives.
    int fd2 = open(path, O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd2 < 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: cannot reopen pid file '%s' read-only: %s.", path, strerror(errno));
        close(fd);
        return -1;
    }
    struct stat st2;
    if (fstat(fd2, &st2) != 0 || st2.st_dev != st.st_dev || st2.st_ino != st.st_ino) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: pid file '%s' was replaced while starting; refusing.", path);
        close(fd2);
        close(fd);
        return -1;
    }
    close(fd);
#ifdef SLS_TEST_PID_GAP
    // test builds only (never compiled in release): widen the gap so the test can
    // start a second instance inside it deterministically
    { const char *g = getenv("SLS_TEST_PID_GAP_MS"); if (g) usleep((useconds_t)atoi(g) * 1000); }
#endif
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_RDLCK; fl.l_whence = SEEK_SET;
    if (fcntl(fd2, F_SETLK, &fl) != 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: another instance is running (could not re-lock pid file '%s': %s).", path, strerror(errno));
        close(fd2);
        return -1;
    }
    {
        char chk[32];
        ssize_t cn = pread(fd2, chk, sizeof(chk) - 1, 0);
        long    cv = 0;
        if (cn > 0) {
            chk[cn] = '\0';
            cv = strtol(chk, NULL, 10);
        }
        if (cn <= 0 || cv != (long)pid) {
            sls_log(SLS_LOG_ERROR, "sls_write_pid: another instance is running (pid file '%s' holds pid %ld, not ours).", path, cv);
            close(fd2);
            return -1;
        }
    }
    sls_log(SLS_LOG_INFO, "sls_write_pid ok, file='%s', pid=%d.", path, pid);
    return fd2;
}

// Command-line side of 'sls -c conf -s reload|stop'. Output goes to stdout.
int sls_send_cmd(const char *pid_path, const char *cmd)
{
    int sig = 0;
    if (NULL == cmd) {
        printf("sls_send_cmd failed, cmd is null.\n");
        return SLS_ERROR;
    }
    if (strcmp(cmd, "reload") == 0) {
        sig = SIGHUP;
    } else if (strcmp(cmd, "stop") == 0) {
        sig = SIGINT;
    } else {
        printf("sls_send_cmd failed, unknown command '%s' (use reload or stop).\n", cmd);
        return SLS_ERROR;
    }

    int fd = open(pid_path, O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd < 0) {
        printf("sls_send_cmd failed, cannot open pid file '%s': %s (is sls running with this conf?).\n", pid_path, strerror(errno));
        return SLS_ERROR;
    }
    struct flock who;
    memset(&who, 0, sizeof(who));
    who.l_type = F_WRLCK; who.l_whence = SEEK_SET;
    int r = fcntl(fd, F_GETLK, &who);
    close(fd);
    if (r != 0) {
        printf("sls_send_cmd failed, cannot query the lock on '%s': %s.\n", pid_path, strerror(errno));
        return SLS_ERROR;
    }
    // No lock holder: stale file, nothing is running (whatever pid it names).
    if (who.l_type == F_UNLCK) {
        printf("sls is not running (stale pid file '%s'); nothing sent.\n", pid_path);
        return SLS_ERROR;
    }

    // The target is the kernel's answer. Only signal it if it runs this sls binary.
    int target = (int)who.l_pid;
    char exe_self[PATH_MAX], exe_target[PATH_MAX], link[64];
    ssize_t ns = readlink("/proc/self/exe", exe_self, sizeof(exe_self) - 1);
    snprintf(link, sizeof(link), "/proc/%d/exe", target);
    ssize_t nt = readlink(link, exe_target, sizeof(exe_target) - 1);
    if (ns > 0) exe_self[ns] = '\0';
    if (nt > 0) exe_target[nt] = '\0';
    if (target <= 0 || ns <= 0 || nt <= 0 || strcmp(exe_self, exe_target) != 0) {
        printf("lock holder pid %d is not sls; nothing sent.\n", target);
        return SLS_ERROR;
    }
    if (kill(target, sig) != 0) {
        printf("sls_send_cmd failed, cannot signal pid %d: %s.\n", target, strerror(errno));
        return SLS_ERROR;
    }
    if (sig == SIGHUP) {
        printf("reload signal sent; result in log\n");
    } else {
        printf("stop signal sent\n");
    }
    return SLS_OK;
}

int sls_drop_privileges(const char *user, const char *group)
{
    bool want_user  = (user  != NULL && user[0]  != '\0');
    bool want_group = (group != NULL && group[0] != '\0');

    if (!want_user && !want_group) {
        return SLS_OK;
    }

    if (want_group && !want_user) {
        sls_log(SLS_LOG_FATAL, "sls_drop_privileges: 'group' set without 'user'. Configure both or neither.");
        return SLS_ERROR;
    }

    if (geteuid() != 0) {
        sls_log(SLS_LOG_WARNING, "sls_drop_privileges: not running as root, ignoring user='%s' group='%s'.",
                user ? user : "", group ? group : "");
        return SLS_OK;
    }

    errno = 0;
    struct passwd *pw = getpwnam(user);
    if (!pw) {
        sls_log(SLS_LOG_FATAL, "sls_drop_privileges: user '%s' not found (errno=%d).", user, errno);
        return SLS_ERROR;
    }
    uid_t target_uid = pw->pw_uid;
    gid_t target_gid = pw->pw_gid;

    if (want_group) {
        errno = 0;
        struct group *gr = getgrnam(group);
        if (!gr) {
            sls_log(SLS_LOG_FATAL, "sls_drop_privileges: group '%s' not found (errno=%d).", group, errno);
            return SLS_ERROR;
        }
        target_gid = gr->gr_gid;
    }

    if (initgroups(user, target_gid) != 0) {
        sls_log(SLS_LOG_FATAL, "sls_drop_privileges: initgroups('%s', %d) failed: %s.",
                user, (int)target_gid, strerror(errno));
        return SLS_ERROR;
    }

    if (setgid(target_gid) != 0) {
        sls_log(SLS_LOG_FATAL, "sls_drop_privileges: setgid(%d) failed: %s.",
                (int)target_gid, strerror(errno));
        return SLS_ERROR;
    }

    if (setuid(target_uid) != 0) {
        sls_log(SLS_LOG_FATAL, "sls_drop_privileges: setuid(%d) failed: %s.",
                (int)target_uid, strerror(errno));
        return SLS_ERROR;
    }

    // Paranoia: make sure the drop was irreversible.
    if (target_uid != 0 && setuid(0) == 0) {
        sls_log(SLS_LOG_FATAL, "sls_drop_privileges: privilege drop did not stick; setuid(0) succeeded.");
        return SLS_ERROR;
    }

    sls_log(SLS_LOG_INFO, "sls_drop_privileges: dropped to uid=%d gid=%d (user='%s', group='%s').",
            (int)target_uid, (int)target_gid, user, want_group ? group : "(from passwd)");
    return SLS_OK;
}

#define ADD_VECTOR_END(v,i) (v).push_back((i))

void sls_split_string(std::string str, std::string separator, std::vector<std::string> &result, int count)
{
	result.clear();
	string::size_type position = str.find(separator);
	string::size_type lastPosition = 0;
	uint32_t separatorLength = (uint32_t) separator.length();

	int i = 0;
	while (position != str.npos) {
		ADD_VECTOR_END(result, str.substr(lastPosition, position - lastPosition));
		lastPosition = position + separatorLength;
		position = str.find(separator, lastPosition);
		i ++;
		if (i == count)
			break;
	}
	ADD_VECTOR_END(result, str.substr(lastPosition, string::npos));
}

std::string sls_find_string(std::vector<std::string> &src, std::string &dst)
{
	std::string ret = std::string("");
	std::vector<std::string>::iterator it;
    for(it=src.begin(); it!=src.end();) {
    	std::string str = *it;
    	it ++;
    	string::size_type pos = str.find(dst);
    	if (pos != std::string::npos)
    	{
    		ret = str;
    	    break;
    	}
    }
    return ret;
}

