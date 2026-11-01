
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

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

using namespace std;

#include "SLSLog.hpp"
#include "SLSClient.hpp"

/*
 * ctrl + c controller
 */
static bool b_exit = 0;
static void ctrl_c_handler(int s){
    printf("\ncaught signal %d, exit.\n",s);
    b_exit = true;
}


/**
 * usage information
 */
#define SLS_MAJOR_VERSION "1"
#define SLS_MIN_VERSION "1"
#define SLS_TEST_VERSION "x"
static void usage()
{
    printf("-------------------------------------------------\n");
    printf("           srt-live-client \n");
    printf("                    v%s.%s.%s \n", SLS_MAJOR_VERSION, SLS_MIN_VERSION, SLS_TEST_VERSION);
    printf("-------------------------------------------------\n");
    printf("    -r srt_url [-o out_file_name] [-c worker_count] \n");
    printf("    -r srt_url -i ts_file_name \n");
    printf("    \n");
}

struct sls_opt_client_t {
    char input_ts_file[1024];
    char srt_url[1024];
    char out_file_name[1024];
    char ts_file_name[1024];
    int  worker_count;
    bool loop;
//  int xxx;                  //-x example
};


// Copy a command line value into a fixed buffer; false (and a message) if it
// does not fit. The value itself is not logged: a srt url may carry a passphrase.
static bool copy_arg(char *dst, size_t dst_size, const char *src, const char *flag)
{
    int n = snprintf(dst, dst_size, "%s", src);
    if (n < 0 || (size_t)n >= dst_size) {
        sls_log(SLS_LOG_ERROR, "srt live client, the value of '%s' is too long (max %zu characters), EXIT!",
                flag, dst_size - 1);
        return false;
    }
    return true;
}

// Parse a whole decimal number in [lo, hi].
static bool parse_int_arg(const char *s, long lo, long hi, long *out, const char *flag)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi) {
        sls_log(SLS_LOG_ERROR, "srt live client, the value of '%s' must be a number from %ld to %ld, EXIT!",
                flag, lo, hi);
        return false;
    }
    *out = v;
    return true;
}

int main(int argc, char* argv[])
{
    struct sigaction    sigIntHandler;
    sls_opt_client_t    sls_opt;

    usage();

    //parse cmd line
    if (argc < 3) {
        sls_log(SLS_LOG_ERROR, "srt live client, no enough parameters, EXIT!");
        return 1;
    }

    //parset argv
    memset(&sls_opt, 0, sizeof(sls_opt));
    int i = 1;
	while (i < argc) {
	    sls_remove_marks(argv[i]);
	    const char *flag = argv[i];
	    if (strcmp("-r", flag) != 0 && strcmp("-i", flag) != 0 && strcmp("-o", flag) != 0
	        && strcmp("-c", flag) != 0 && strcmp("-l", flag) != 0) {
	        sls_log(SLS_LOG_ERROR, "srt live client, wrong parameter '%s', EXIT!", flag);
	        return 1;
	    }
	    // every flag takes a value
	    if (i + 1 >= argc) {
	        sls_log(SLS_LOG_ERROR, "srt live client, parameter '%s' needs a value, EXIT!", flag);
	        return 1;
	    }
	    char *val = argv[i + 1];
	    sls_remove_marks(val);
	    long num = 0;
		if (strcmp("-r", flag) == 0) {
			if (!copy_arg(sls_opt.srt_url, sizeof(sls_opt.srt_url), val, flag)) return 1;
		} else if (strcmp("-i", flag) == 0) {
			if (!copy_arg(sls_opt.ts_file_name, sizeof(sls_opt.ts_file_name), val, flag)) return 1;
		} else if (strcmp("-o", flag) == 0) {
			if (!copy_arg(sls_opt.out_file_name, sizeof(sls_opt.out_file_name), val, flag)) return 1;
		} else if (strcmp("-c", flag) == 0) {
			if (!parse_int_arg(val, 0, 256, &num, flag)) return 1;
			sls_opt.worker_count = (int)num;
		} else {
			if (!parse_int_arg(val, 0, 1, &num, flag)) return 1;
			sls_opt.loop = (num != 0);
		}
		i += 2;
	}
	if (strlen(sls_opt.srt_url) == 0) {
	    sls_log(SLS_LOG_ERROR, "srt live client, '-r srt_url' is required, EXIT!");
	    return 1;
	}
	CSLSClient sls_client;
	if (strlen(sls_opt.ts_file_name) > 0) {
		if (SLS_OK != sls_client.push(sls_opt.srt_url, sls_opt.ts_file_name, sls_opt.loop)) {
		    sls_log(SLS_LOG_INFO, "sls_client.push failed, EXIT!");
			return SLS_ERROR;
		}
	} else {
		if (SLS_OK != sls_client.play(sls_opt.srt_url, sls_opt.out_file_name)) {
			sls_log(SLS_LOG_INFO, "sls_client.play failed, EXIT!");
			return SLS_ERROR;
		}
		for(i = 1; i < sls_opt.worker_count; i++) {
	        pid_t fpid; //fpid表示fork函数返回的值
	        fpid=fork();
	        if (fpid < 0){
	            printf("srt live client, error in fork!");
	        }else if (fpid == 0) {
	            printf("srt live client, i am the child process, my process id is %d\n",getpid());
	            break;
	        }else {
	            printf("srt live client, i am the parent process, my process id is %d\n",getpid());
	        }
	    }
	}


    //ctrl + c to exit
    sigIntHandler.sa_handler = ctrl_c_handler;
    sigemptyset(&sigIntHandler.sa_mask);
    sigIntHandler.sa_flags = 0;
    sigaction(SIGINT, &sigIntHandler, 0);

    sls_log(SLS_LOG_INFO, "\nsrt live client is running...");
	while(!b_exit)
	{
		//printf log info
		int64_t kb = sls_client.get_bitrate() ;
	    printf("\rsrt live client, cur bitrate=%lld(kb)", (long long)kb);

		int ret = sls_client.handler();
		if ( ret > 0) {
			continue;
		} else if (0 == ret) {
			msleep(1);
		} else {
			break;
		}
	}

    sls_log(SLS_LOG_INFO, "exit, stop srt live client...");
    sls_client.close();
    sls_log(SLS_LOG_INFO, "exit, bye bye!");

    return 0;
}
