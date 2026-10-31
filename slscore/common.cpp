
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

int sls_gethostbyname(const char *hostname, char *ip)
{
    char   *ptr, **pptr;
    struct hostent *hptr;
    char   str[32];
    ptr = (char *)hostname;
    int ret = SLS_ERROR;

    if((hptr = gethostbyname(ptr)) == NULL)
     {
         printf("sls_gethostbyname: gethostbyname error for host:%s\n", ptr);
         return ret;
     }

/*
    printf("official hostname:%s\n",hptr->h_name);
     for(pptr = hptr->h_aliases; *pptr != NULL; pptr++)
         printf(" alias:%s\n",*pptr);
*/

     switch(hptr->h_addrtype)
     {
         case AF_INET:
         case AF_INET6:
             //pptr=hptr->h_addr_list;
             //for(; *pptr!=NULL; pptr++)
             //    printf(" address:%s\n",
             //           inet_ntop(hptr->h_addrtype, *pptr, str, sizeof(str)));

        	 //copy the 1st ip
             strcpy(ip, inet_ntop(hptr->h_addrtype, hptr->h_addr, str, sizeof(str)));
             ret = SLS_OK;
         break;
         default:
             printf("sls_gethostbyname: unknown address type\n");
         break;
     }

     return ret;
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
//  * The file is 0600 and must live in a directory only root / the service user
//    can write to, so no other user can open it, lock it, or replace it.
//  * sls never creates directories and never follows a symlink at the final path
//    component (O_NOFOLLOW).
//  * A running server holds a process-associated fcntl READ lock on the file
//    (taken on a read-only fd that stays open until exit). `sls -s` asks
//    F_GETLK who holds it: no holder => stale file, never signalled; holder pid
//    must equal the pid in the file before anything is signalled.
//  * The writable fd is closed before privileges are dropped, so the unprivileged
//    daemon cannot rewrite the file.

// Returns the locked read-only fd (>= 0, keep it open until exit) or -1.
int sls_write_pid(const char *path, int pid)
{
    if (NULL == path || '\0' == path[0]) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: no pid file path.");
        return -1;
    }
    struct stat st;
    struct flock fl;

    // 1. An existing file may be a leftover from an older build or planted by
    //    someone else, and somebody may already hold an fd on it that no chmod
    //    can revoke. Never reuse it: refuse if a lock holder exists (a running
    //    instance), otherwise unlink it and create a fresh inode below.
    int ofd = open(path, O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if (ofd >= 0) {
        if (fstat(ofd, &st) != 0 || !S_ISREG(st.st_mode)) {
            sls_log(SLS_LOG_ERROR, "sls_write_pid: '%s' exists and is not a regular file; refusing to touch it.", path);
            close(ofd);
            return -1;
        }
        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
        if (fcntl(ofd, F_GETLK, &fl) != 0) {
            sls_log(SLS_LOG_ERROR, "sls_write_pid: cannot query the lock on existing pid file '%s': %s.", path, strerror(errno));
            close(ofd);
            return -1;
        }
        close(ofd);
        if (fl.l_type != F_UNLCK) {
            sls_log(SLS_LOG_ERROR, "sls_write_pid: another instance (pid %d) is running with pid file '%s'.", (int)fl.l_pid, path);
            return -1;
        }
        if (unlink(path) != 0 && errno != ENOENT) {
            sls_log(SLS_LOG_ERROR, "sls_write_pid: cannot remove stale pid file '%s': %s.", path, strerror(errno));
            return -1;
        }
    } else if (errno != ENOENT) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: cannot open pid file '%s': %s. "
                "sls does not create directories or follow symlinks for this file; "
                "set 'pid_file' in the srt block of the conf to a writable path, or create its directory.",
                path, strerror(errno));
        return -1;
    }

    // 2. Always a fresh file: O_EXCL, so if anyone raced us in, refuse (no retry).
    int wfd = open(path, O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC, 0600);
    if (wfd < 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: cannot create pid file '%s': %s. "
                "sls does not create directories or follow symlinks for this file; "
                "set 'pid_file' in the srt block of the conf to a writable path, or create its directory.",
                path, strerror(errno));
        return -1;
    }

    // 3. Fail closed unless it is exactly what we think we created.
    if (fstat(wfd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: new pid file '%s' is not a private plain file owned by this user (mode %o); refusing to use it.",
                path, (unsigned)(st.st_mode & 07777));
        close(wfd);
        unlink(path);
        return -1;
    }

    // Take the write lock now so two simultaneous starts cannot both pass.
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK; fl.l_whence = SEEK_SET;
    if (fcntl(wfd, F_SETLK, &fl) != 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: cannot lock pid file '%s': %s.", path, strerror(errno));
        close(wfd);
        return -1;
    }

    char buf[32];
    int  len = snprintf(buf, sizeof(buf), "%d\n", pid);
    int  off = 0;
    while (off < len) {
        ssize_t n = write(wfd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            sls_log(SLS_LOG_ERROR, "sls_write_pid: write to pid file '%s' failed: %s.", path, strerror(errno));
            close(wfd);
            return -1;
        }
        off += (int)n;
    }
    if (fsync(wfd) != 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: fsync of pid file '%s' failed: %s.", path, strerror(errno));
        close(wfd);
        return -1;
    }

    // Process locks belong to (process, file): closing ANY fd of this file
    // drops them all. So close the writable fd first, then lock a read-only one.
    close(wfd);
    int rfd = open(path, O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (rfd < 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: cannot reopen pid file '%s' read-only: %s.", path, strerror(errno));
        return -1;
    }
    struct stat rst;
    if (fstat(rfd, &rst) != 0 || rst.st_ino != st.st_ino || rst.st_dev != st.st_dev) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: pid file '%s' was replaced while starting; refusing.", path);
        close(rfd);
        return -1;
    }
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_RDLCK; fl.l_whence = SEEK_SET;
    if (fcntl(rfd, F_SETLK, &fl) != 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid: cannot lock pid file '%s': %s.", path, strerror(errno));
        close(rfd);
        return -1;
    }
    sls_log(SLS_LOG_INFO, "sls_write_pid ok, file='%s', pid=%d.", path, pid);
    return rfd;
}

// Returns the pid in the file, or 0 if the file is missing/unreadable/invalid.
int sls_read_pid(const char *path)
{
    if (NULL == path || '\0' == path[0]) {
        return 0;
    }
    int fd = open(path, O_RDONLY|O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    char buf[32];
    ssize_t n;
    do {
        n = read(fd, buf, sizeof(buf) - 1);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    buf[n] = '\0';

    char *end = NULL;
    errno = 0;
    long v = strtol(buf, &end, 10);
    if (errno != 0 || end == buf || v <= 0 || v > INT_MAX) {
        return 0;
    }
    while (*end == '\n' || *end == '\r' || *end == ' ') end++;
    if (*end != '\0') {
        return 0;
    }
    return (int)v;
}

// Exit: remove the file if we still can (usually not after the privilege drop;
// tolerated). A leftover file is harmless: nothing holds a lock on it once the
// fd is closed, so `-s` reports it stale. There is no writable fd to truncate.
int sls_remove_pid(const char *path, int fd)
{
    if (path && path[0] != '\0' && unlink(path) != 0 && errno != ENOENT && errno != EACCES) {
        sls_log(SLS_LOG_WARNING, "sls_remove_pid: cannot remove '%s': %s.", path, strerror(errno));
    }
    if (fd >= 0) {
        close(fd);
    }
    return 0;
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

    int pid = sls_read_pid(pid_path);

    int fd = open(pid_path, O_RDONLY|O_CLOEXEC);
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
    // A running server holds a lock for its whole life. No holder: stale file,
    // its pid may belong to an unrelated process now, never signal it.
    if (who.l_type == F_UNLCK) {
        printf("sls is not running (stale pid file '%s'); nothing sent.\n", pid_path);
        return SLS_ERROR;
    }
    if (pid <= 0 || (int)who.l_pid != pid) {
        printf("pid file '%s' does not match lock holder (file pid %d, lock holder pid %d); nothing sent.\n",
               pid_path, pid, (int)who.l_pid);
        return SLS_ERROR;
    }
    if (kill(pid, sig) != 0) {
        printf("sls_send_cmd failed, cannot signal pid %d from pid file '%s': %s.\n", pid, pid_path, strerror(errno));
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

