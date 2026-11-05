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


#include <string.h>
#include <stddef.h>
#include "sls_test.hpp"
#include "conf.hpp"

struct bool_holder { bool b; };

static const char *set(const char *v, bool *out, bool start)
{
    bool_holder h;
    h.b = start;
    sls_conf_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.offset = offsetof(bool_holder, b);
    const char *ret = sls_conf_set_bool(v, &cmd, &h);
    *out = h.b;
    return ret;
}

SLS_TEST(conf_bool_on_off)
{
    bool b;
    SLS_CHECK(set("on", &b, false) == SLS_CONF_OK);    SLS_CHECK(b == true);
    SLS_CHECK(set("off", &b, true) == SLS_CONF_OK);    SLS_CHECK(b == false);
    SLS_CHECK(set("true", &b, false) == SLS_CONF_OK);  SLS_CHECK(b == true);
    SLS_CHECK(set("false", &b, true) == SLS_CONF_OK);  SLS_CHECK(b == false);
    const char *r = set("yes", &b, true);
    SLS_CHECK(r != NULL && 0 == strcmp(r, SLS_CONF_WRONG_TYPE));
    SLS_CHECK(b == true);   // unchanged on error
    r = set("ON", &b, false);
    SLS_CHECK(r != NULL && 0 == strcmp(r, SLS_CONF_WRONG_TYPE));   // as strict as true/false
}
