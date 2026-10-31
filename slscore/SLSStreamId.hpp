#ifndef _SLSStreamId_INCLUDE_
#define _SLSStreamId_INCLUDE_

#include <stddef.h>
#include <string>

/**
 * Streamid parsing shared by the SRT listen callback and the accept handler.
 *
 * A valid streamid is exactly "host/app/stream". Each part must match
 * [A-Za-z0-9._-]{1,128} and must not be "." or "..".
 */
#define SLS_SID_PART_MAX 128

struct sls_sid_t {
    std::string host;
    std::string app;
    std::string stream;
};

enum sls_sid_role_t {
    SLS_SID_REJECT = 0,
    SLS_SID_PUBLISHER,
    SLS_SID_PLAYER
};

// charset + length + not "." / ".."
bool        sls_sid_part_ok(const std::string &part);

// exactly host/app/stream (two '/' only); every part must pass sls_sid_part_ok
bool        sls_sid_parse(const char *sid, sls_sid_t *out);

// Printable copy of untrusted text for logs: bytes outside 0x20-0x7e (and '\')
// become \xNN; input longer than max_len bytes is cut and "..." appended.
std::string sls_escape_for_log(const char *s, size_t max_len = 128);

#endif
