
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
#include <signal.h>
#include <unistd.h>

using namespace std;

#include "SLSLog.hpp"
#include "SLSManager.hpp"
#include "HttpClient.hpp"

/*
 * Signal handlers. Only async-signal-safe operations are allowed here
 * (POSIX.1-2008 §2.4.3); printf is not on the safe list and neither is
 * a plain bool write. Store flags as volatile sig_atomic_t and let the
 * main loop observe them and log.
 */
static volatile sig_atomic_t b_exit = 0;
static void ctrl_c_handler(int s){
    (void)s;
    b_exit = 1;
}


static volatile sig_atomic_t b_reload = 0;
static void reload_handler(int s){
    (void)s;
    b_reload = 1;
}

/**
 * usage information
 */
static void usage()
{
    printf("-------------------------------------------------\n");
    printf("           srt-live-srver \n");
    printf("                    v%s \n", SLS_VERSION_STRING);
    printf("-------------------------------------------------\n");
    printf("    \n");
}

//add new parameter here
static sls_conf_cmd_t  conf_cmd_opt[] = {
    SLS_SET_OPT(string, c, conf_file_name, "conf file name", 1, 1023),
    SLS_SET_OPT(string, s, c_cmd,          "cmd: reload|stop (needs -c <conf> to find pid_file)", 1, 1023),
    SLS_SET_OPT(string, l, log_level,      "log level: fatal/error/warning/info/debug/trace", 1, 1023),
//  SLS_SET_OPT(int, x, xxx,          "", 1, 100),//example
};

/*
 * pid file path: the 'pid_file' directive, or the built-in default.
 */
static void get_pid_path(const sls_conf_srt_t *conf_srt, char *out, size_t size)
{
    if (conf_srt && conf_srt->pid_file[0] != '\0') {
        snprintf(out, size, "%s", conf_srt->pid_file);
    } else {
        snprintf(out, size, "%s", SLS_DEFAULT_PID_FILE);
    }
}

/*
 * stat_post_url's server: parsed and resolved at start and on reload, on the
 * main thread (the HTTP client never resolves names itself).
 */
static int resolve_stat_url(const char *url, struct sockaddr_storage *addr, socklen_t *addr_len)
{
    std::string host, uri;
    int         port = 0;
    if (SLS_OK != sls_parse_http_url(url, &host, &port, &uri)) {
        return SLS_ERROR;
    }
    return sls_resolve(host.c_str(), port, addr, addr_len);
}

/*
 * 'sls -c <conf> -s reload|stop': read pid_file from the conf and signal the
 * running server. Returns the process exit code.
 */
static int run_cli_cmd(const char *conf_file, const char *cmd)
{
    char pid_path[URL_MAX_LEN];
    char quiet[] = "error";   // set_log_level upcases in place: needs a writable buffer

    if (conf_file[0] == '\0') {
        printf("error: '-s %s' needs the config file to find 'pid_file'; pass -c <conf>.\n", cmd);
        return 1;
    }
    sls_set_log_level(quiet);
    if (SLS_OK != sls_conf_open(conf_file)) {
        return 1;
    }
    sls_conf_srt_t *conf_srt = (sls_conf_srt_t *)sls_conf_get_root_conf();
    if (!conf_srt) {
        printf("error: no srt block in conf file '%s'.\n", conf_file);
        return 1;
    }
    get_pid_path(conf_srt, pid_path, sizeof(pid_path));
    // the process exits right after, so the parsed conf is not released here
    return (SLS_OK == sls_send_cmd(pid_path, cmd)) ? 0 : 1;
}

int main(int argc, char* argv[])
{
    struct sigaction    sigIntHandler;
    struct sigaction    sigHupHandler;
    sls_opt_t           sls_opt;

    CSLSManager             *sls_manager = NULL;
    std::list<CSLSManager*>  reload_manager_list;
    CHttpClient             *http_stat_client = new CHttpClient;

    int ret = SLS_OK;
    int exit_code = 0;   // non-zero on any startup failure
    int l = sizeof(sockaddr_in);
    int64_t tm_begin_ms = 0;

    char stat_method[]        = "POST";
    sls_conf_srt_t * conf_srt = NULL;
    char pid_path[URL_MAX_LEN] = {0};
    int  pid_fd               = -1;      // locked pid file, held open until exit
    bool stat_posting         = false;   // stat_post_url resolved at start: the client runs

    usage();

    //parse cmd line
    memset(&sls_opt, 0, sizeof(sls_opt));
    if (argc > 1) {
        //parset argv
    	int cmd_size = sizeof(conf_cmd_opt)/sizeof(sls_conf_cmd_t);
        ret = sls_parse_argv(argc, argv, &sls_opt, conf_cmd_opt, cmd_size);
        if (ret!= SLS_OK) {
            CSLSLog::destory_instance();
            return 1;
        }
    }

    //-s reload|stop
    if (strcmp(sls_opt.c_cmd,  "") != 0) {
        return run_cli_cmd(sls_opt.conf_file_name, sls_opt.c_cmd);
    }

    //log level
    if (strlen(sls_opt.log_level) > 0) {
        sls_set_log_level(sls_opt.log_level);
    }

    //test erro info...
    //CSLSSrt::libsrt_print_error_info();

    //ctrl + c / kill (SIGTERM, e.g. systemd stop) to exit
    sigIntHandler.sa_handler = ctrl_c_handler;
    sigemptyset(&sigIntHandler.sa_mask);
    sigIntHandler.sa_flags = 0;
    sigaction(SIGINT, &sigIntHandler, 0);
    sigaction(SIGTERM, &sigIntHandler, 0);

    //a peer closing a TCP connection (stats/webhook) must not kill the process
    signal(SIGPIPE, SIG_IGN);

    //hup to reload
    sigHupHandler.sa_handler = reload_handler;
    sigemptyset(&sigHupHandler.sa_mask);
    sigHupHandler.sa_flags = 0;
    sigaction(SIGHUP, &sigHupHandler, 0);

    //init srt
    CSLSSrt::libsrt_init();

    //parse conf file
    if (strlen(sls_opt.conf_file_name) == 0) {
        sprintf(sls_opt.conf_file_name, "./sls.conf");
    }
    ret = sls_conf_open(sls_opt.conf_file_name);
    if (ret!= SLS_OK) {
        sls_log(SLS_LOG_ERROR, "sls_conf_open failed, EXIT!");
        exit_code = 1;
        goto EXIT_PROC;
    }

    conf_srt = (sls_conf_srt_t *)sls_conf_get_root_conf();
    if (!conf_srt) {
        sls_log(SLS_LOG_ERROR, "no srt block in the conf file, EXIT!");
        exit_code = 1;
        goto EXIT_PROC;
    }

    // the pid file is created and locked here and stays in place at exit
    // (only the lock goes, with the process: see sls_write_pid)
    get_pid_path(conf_srt, pid_path, sizeof(pid_path));
    pid_fd = sls_write_pid(pid_path, getpid());
    if (pid_fd < 0) {
        sls_log(SLS_LOG_ERROR, "sls_write_pid failed, EXIT!");
        exit_code = 1;
        goto EXIT_PROC;
    }

    // Startup order: conf -> pid file -> bind/listen -> drop privileges ->
    // worker threads -> stats client. The privileged steps (pid file in /run,
    // bind to a port < 1024) are done while still root; no thread that serves
    // a connection exists until the drop has succeeded.
    sls_log(SLS_LOG_INFO, "\nsrt live server is running...");

    sls_manager  = new CSLSManager;
    if (SLS_OK != sls_manager->start_listeners()) {
        sls_log(SLS_LOG_ERROR, "sls_manager->start_listeners failed, EXIT!");
        exit_code = 1;
        goto EXIT_PROC;
    }

    if (SLS_OK != sls_drop_privileges(conf_srt->user, conf_srt->group)) {
        sls_log(SLS_LOG_ERROR, "sls_drop_privileges failed, EXIT!");
        exit_code = 1;
        goto EXIT_PROC;
    }

    if (SLS_OK != sls_manager->start_workers()) {
        sls_log(SLS_LOG_ERROR, "sls_manager->start_workers failed, EXIT!");
        exit_code = 1;
        goto EXIT_PROC;
    }

    if (strlen(conf_srt->stat_post_url) > 0) {
        // resolved here and on reload; the client never does DNS itself
        struct sockaddr_storage stat_addr;
        socklen_t   stat_addr_len = 0;
        if (SLS_OK != resolve_stat_url(conf_srt->stat_post_url, &stat_addr, &stat_addr_len)) {
            sls_log(SLS_LOG_ERROR, "stat_post_url='%s' is not a usable http:// URL or does not resolve; stat posting is disabled.",
                    sls_url_for_log(conf_srt->stat_post_url).c_str());
        } else {
            http_stat_client->set_stage_callback(CSLSManager::stat_client_callback, sls_manager);
            http_stat_client->open(conf_srt->stat_post_url, &stat_addr, stat_addr_len, stat_method, conf_srt->stat_post_interval);
            stat_posting = true;
        }
    }

	while(!b_exit)
	{
		int64_t cur_tm_ms = sls_gettime_ms();
		ret = 0;
		if (sls_manager->is_single_thread()) {
			ret = sls_manager->single_thread_handler();
		}
		if (NULL != http_stat_client) {
			if (!http_stat_client->is_valid()) {
				if (SLS_OK == http_stat_client->check_repeat(cur_tm_ms)) {
					http_stat_client->reopen();
				}
			}
			ret = http_stat_client->handler();
			if (SLS_OK == http_stat_client->check_finished() ||
				SLS_OK == http_stat_client->check_timeout(cur_tm_ms)) {
				//http_stat_client->get_response_info();
				http_stat_client->close();
			}

		}

		msleep(10);

		// accepted connections still waiting for a worker past admission_timeout
		if (cur_tm_ms - tm_begin_ms >= 100) {
			tm_begin_ms = cur_tm_ms;
			sls_manager->expire_queued_roles(cur_tm_ms);
			// relay upstreams a relay end marked stale: resolved here, not on a worker
			sls_manager->refresh_relay_upstreams();
			// peer lines held back by the 1/s limit: a summary once a second
			sls_peer_log_flush();
		}

		/*for test reload...
		int64_t tm_cur = sls_gettime();
		int64_t d = tm_cur - tm;
		if ( d >= 10000000) {
			b_reload = !b_reload;
			tm = tm_cur;
			printf("\n\n\n\n");
		}
		//*/

		//check reloaded manager
		int reload_managers = reload_manager_list.size();
	    std::list<CSLSManager *>::iterator it;
	    std::list<CSLSManager *>::iterator it_erase;
	    for ( it = reload_manager_list.begin(); it != reload_manager_list.end();)
	    {
	    	CSLSManager * manager = *it;
    		it_erase = it;
    		it ++;
	    	if (NULL == manager) {
	    		continue;
	    	}
	    	if (SLS_OK == manager->check_invalid()) {
	            sls_log(SLS_LOG_INFO, "check reloaded manager, delete manager=%p ...", manager);
	            manager->stop();
	            delete manager;
	    		reload_manager_list.erase(it_erase);
	    	}
	    }

		if (b_reload) {
            // SIGHUP => live-reload user_file credentials on all listeners,
            // without tearing down the listener sockets, and re-resolve the
            // on_event_url host (per listener) and the stat_post_url host;
            // a host that does not resolve keeps its previous address. Other
            // config changes still require a restart.
            b_reload = 0;
            sls_log(SLS_LOG_INFO, "reload srt live server...");
            if (stat_posting) {
                struct sockaddr_storage stat_addr;
                socklen_t   stat_addr_len = 0;
                if (SLS_OK == resolve_stat_url(conf_srt->stat_post_url, &stat_addr, &stat_addr_len)) {
                    http_stat_client->set_server_addr(&stat_addr, stat_addr_len);
                    sls_log(SLS_LOG_INFO, "reload, stat_post_url host re-resolved.");
                } else {
                    sls_log(SLS_LOG_ERROR, "reload, cannot resolve the stat_post_url host, keeping the previous address.");
                }
            }
            if (NULL != sls_manager) {
                if (SLS_OK == sls_manager->reload_users()) {
                    sls_log(SLS_LOG_INFO, "reload successfully.");
                } else {
                    sls_log(SLS_LOG_ERROR, "reload failed; the previous user_file contents are still in force.");
                }
            }
		}
	}

EXIT_PROC:
    sls_log(SLS_LOG_INFO, "exit, stop srt live server...");

	//stop srt
    if (NULL != sls_manager) {
        sls_manager->stop();
        delete sls_manager;
        sls_manager = NULL;
        sls_log(SLS_LOG_INFO, "exit, release sls_manager ok.");
    }

    //release all reload manager
    sls_log(SLS_LOG_INFO, "exit, release reload_manager_list begin，count=%zu.", reload_manager_list.size());
    std::list<CSLSManager *>::iterator it;
    for ( it = reload_manager_list.begin(); it != reload_manager_list.end(); it++)
    {
    	CSLSManager * manager = *it;
    	if (NULL == manager) {
    		continue;
    	}
    	manager->stop();
        delete manager;
    }
    sls_log(SLS_LOG_INFO, "exit, release reload_manager_list ok.");
    reload_manager_list.clear();

    sls_log(SLS_LOG_INFO, "exit, release http_stat_client.");
    //release http_stat_client
    if (NULL != http_stat_client) {
    	http_stat_client->close();
    	delete http_stat_client;
    	http_stat_client = NULL;
    }

    sls_log(SLS_LOG_INFO, "exit, uninit srt .");
    //uninit srt
    CSLSSrt::libsrt_uninit();

    sls_log(SLS_LOG_INFO, "exit, close conf.");
    sls_conf_close();

    // The pid file is deliberately left in place (see common.cpp): the lock is
    // released when this process exits, which is what `sls -s` looks at.
    if (pid_fd >= 0) {
        close(pid_fd);
        pid_fd = -1;
    }

    sls_log(SLS_LOG_INFO, "exit, bye bye!");

    // last: closes (and flushes) the log file; a later sls_log would
    // silently re-create the logger
    CSLSLog::destory_instance();

    return exit_code;
}
