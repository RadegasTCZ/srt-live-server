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


// Minimal unit-test runner for `make test`: no dependencies beyond libc++.
// SLS_TEST(name) { ... } registers a test; SLS_CHECK / SLS_CHECK_EQ record
// failures with file:line and let the test carry on.

#ifndef _SLS_TEST_INCLUDE_
#define _SLS_TEST_INCLUDE_

#include <stdio.h>

typedef void (*sls_test_fn)();

struct sls_test_case {
    const char *name;
    sls_test_fn fn;
    sls_test_case *next;
};

sls_test_case *&sls_test_list();
int &sls_test_failures();   // failed checks in the running test

struct sls_test_registrar {
    sls_test_case tc;
    sls_test_registrar(const char *name, sls_test_fn fn)
    {
        tc.name = name;
        tc.fn = fn;
        tc.next = sls_test_list();
        sls_test_list() = &tc;
    }
};

#define SLS_TEST(name) \
    static void sls_test_##name(); \
    static sls_test_registrar sls_test_reg_##name(#name, sls_test_##name); \
    static void sls_test_##name()

#define SLS_CHECK(cond) do { \
    if (!(cond)) { \
        printf("  %s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
        sls_test_failures()++; \
    } } while (0)

#define SLS_CHECK_EQ(a, b) do { \
    long long sls_a_ = (long long)(a), sls_b_ = (long long)(b); \
    if (sls_a_ != sls_b_) { \
        printf("  %s:%d: check failed: %s == %s (%lld != %lld)\n", \
               __FILE__, __LINE__, #a, #b, sls_a_, sls_b_); \
        sls_test_failures()++; \
    } } while (0)

#endif
