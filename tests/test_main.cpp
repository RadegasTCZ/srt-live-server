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
#include "sls_test.hpp"

sls_test_case *&sls_test_list()
{
    static sls_test_case *list = NULL;
    return list;
}

int &sls_test_failures()
{
    static int failures = 0;
    return failures;
}

int main(int argc, char **argv)
{
    // registration order is reversed by the list; run in file order
    sls_test_case *cases[1024];
    int n = 0;
    for (sls_test_case *tc = sls_test_list(); tc && n < 1024; tc = tc->next) {
        cases[n++] = tc;
    }
    int pass = 0, fail = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (argc > 1 && strcmp(argv[1], cases[i]->name) != 0) {
            continue;
        }
        sls_test_failures() = 0;
        cases[i]->fn();
        if (sls_test_failures() == 0) {
            pass++;
        } else {
            printf("FAIL %s\n", cases[i]->name);
            fail++;
        }
    }
    printf("PASS %d / FAIL %d\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
