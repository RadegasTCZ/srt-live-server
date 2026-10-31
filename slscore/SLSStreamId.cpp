#include <string.h>

#include "SLSStreamId.hpp"

bool sls_sid_part_ok(const std::string &part)
{
    size_t n = part.length();
    if (n == 0 || n > SLS_SID_PART_MAX)
        return false;
    if (part == "." || part == "..")
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)part[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

bool sls_sid_parse(const char *sid, sls_sid_t *out)
{
    if (!sid || !out)
        return false;
    const char *s1 = strchr(sid, '/');
    const char *s2 = s1 ? strchr(s1 + 1, '/') : NULL;
    if (!s1 || !s2 || strchr(s2 + 1, '/'))
        return false;

    std::string host(sid, s1 - sid);
    std::string app(s1 + 1, s2 - s1 - 1);
    std::string stream(s2 + 1);
    if (!sls_sid_part_ok(host) || !sls_sid_part_ok(app) || !sls_sid_part_ok(stream))
        return false;

    out->host.swap(host);
    out->app.swap(app);
    out->stream.swap(stream);
    return true;
}

std::string sls_escape_for_log(const char *s, size_t max_len)
{
    static const char hex[] = "0123456789abcdef";
    std::string r;
    if (!s)
        return r;
    size_t i = 0;
    for (; s[i] && i < max_len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c <= 0x7e && c != '\\') {
            r += (char)c;
        } else {
            r += "\\x";
            r += hex[c >> 4];
            r += hex[c & 0xf];
        }
    }
    if (s[i])
        r += "...";
    return r;
}
