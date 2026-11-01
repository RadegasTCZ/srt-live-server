
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


#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <iostream>
#include <fstream>

#include "conf.hpp"
#include "common.hpp"
#include "SLSLog.hpp"



sls_conf_base_t  sls_first_conf = {"", NULL, NULL} ;
sls_runtime_conf_t * sls_runtime_conf_t::first = NULL;

/*
 * runtime conf
 */
sls_runtime_conf_t::sls_runtime_conf_t(char * c, create_conf_func f, sls_conf_cmd_t * cmd, int len)
{
    conf_name      = c;
    create_fn      = f;
    conf_cmd       = cmd;
    conf_cmd_size  = len;
    next           = NULL;

    this->next     = first;
    first          = this;
}

sls_conf_cmd_t * sls_conf_find(const char *n, sls_conf_cmd_t *cmd, int size)
{
    for (int i = 0; i < size; i ++) {
        if (strcmp(n, cmd->name) == 0) {
            return cmd ;
        }
        cmd ++;
    }
    return NULL;
}

const char * sls_conf_set_int(const char *v, sls_conf_cmd_t *cmd, void *conf)
{
    char  * p = (char *)conf;
    int   * np;
    char  * end;
    long    lv;

    np = (int *) (p + cmd->offset);

    if (v == NULL || *v == '\0')
        return SLS_CONF_WRONG_TYPE;

    errno = 0;
    lv = strtol(v, &end, 10);
    if (end == v || *end != '\0' || errno == ERANGE || lv < INT_MIN || lv > INT_MAX)
        return SLS_CONF_WRONG_TYPE;
    if (lv < cmd->min || lv > cmd->max)
        return SLS_CONF_OUT_RANGE;
    *np = (int)lv;
    return SLS_CONF_OK;
}

const char * sls_conf_set_string(const char *v, sls_conf_cmd_t *cmd, void *conf)
{
    char  * p = (char *)conf;
    char  * np;
    int     len = strlen(v);

    // Strip a single pair of surrounding quotes (single or double) so
    // users can naturally write string values as "quoted strings".
    if (len >= 2 && (v[0] == '"' || v[0] == '\'') && v[len - 1] == v[0]) {
        v   += 1;
        len -= 2;
    }

    if (len < cmd->min || len > cmd->max)
        return SLS_CONF_OUT_RANGE;

    np = (char *) (p + cmd->offset);
    memcpy(np, v, len);
    np[len] = 0;
    return SLS_CONF_OK;
}

const char * sls_conf_set_double(const char *v, sls_conf_cmd_t *cmd, void *conf)
{
    char   * p = (char *)conf;
    double * np;
    char   * end;
    double   v1;

    np = (double *) (p + cmd->offset);

    if (v == NULL || *v == '\0')
        return SLS_CONF_WRONG_TYPE;

    errno = 0;
    v1 = strtod(v, &end);
    if (end == v || *end != '\0' || errno == ERANGE)
        return SLS_CONF_WRONG_TYPE;
    if (v1 < cmd->min || v1 > cmd->max)
        return SLS_CONF_OUT_RANGE;
    *np = v1;
    return SLS_CONF_OK;
}

const char * sls_conf_set_bool(const char *v, sls_conf_cmd_t *cmd, void *conf)
{
    char      * p = (char *)conf;
    bool      * np;
    char      * value;

    np = (bool *) (p + cmd->offset);

    if (0 == strcmp(v, "true")) {
        *np = true;
        return SLS_CONF_OK;
    } else if (0 == strcmp(v, "false")) {
        *np = false;
        return SLS_CONF_OK;
    } else {
        return SLS_CONF_WRONG_TYPE;
    }
}


int sls_conf_get_conf_count(sls_conf_base_t *c)
{
    int count = 0;
    while (c) {
        c = c->sibling;
        count ++;
    }
    return count;
}

vector<string> sls_conf_string_split(const string& str, const string& delim)
{
    vector<string> res;
    if("" == str) return res;

    char * strs = new char[str.length() + 1] ;
    strcpy(strs, str.c_str());

    char * d = new char[delim.length() + 1];
    strcpy(d, delim.c_str());

    char *p = strtok(strs, d);
    while(p) {
        string s = p;
        res.push_back(s);
        p = strtok(NULL, d);
    }

    delete[] strs;
    delete[] d;
    return res;
}

string& trim(string &s)
{
    if (s.empty())
    {
        return s;
    }
    s.erase(0,s.find_first_not_of(" "));
    s.erase(s.find_last_not_of(" ") + 1);
    return s;
}

string&  replace_all(string& str, const string& old_value, const string& new_value)
{
    while(true) {
        string::size_type pos(0);
        if((pos=str.find(old_value))!=string::npos )
            str.replace(pos,old_value.length(),new_value);
        else
          break;
    }
    return  str;
}

sls_conf_base_t * sls_conf_create_block_by_name(string n, sls_runtime_conf_t *& p_runtime)
{
    sls_conf_base_t * p = NULL;
    p_runtime = sls_runtime_conf_t::first;
    while (p_runtime) {
        if (strcmp(n.c_str(), p_runtime->conf_name) == 0) {
            p = p_runtime->create_fn();
            //sls_add_conf_to_runtime(p, p_runtime);
            break;
        }
        p_runtime = p_runtime->next;
    }
    return p;

}
//#define new_conf(n, i)\
//        sls_conf_##n conf_##n##_##i##_info = new sls_conf_##n ;
/*
 * Secrets must never reach the log, at any level. Whether a config line is
 * secret is decided from the RAW line, before comment stripping and tab
 * normalisation (so no delimiter is needed after the name): if it starts, after
 * leading whitespace, with srt_passphrase. Every sls_log for such a line goes
 * through sls_conf_show(), which prints "srt_passphrase ***" instead.
 */
static bool sls_conf_line_is_secret(const string& raw)
{
    string::size_type b = raw.find_first_not_of(" \t");
    static const char key[] = "srt_passphrase";
    return b != string::npos && raw.compare(b, sizeof(key) - 1, key) == 0;
}

static string sls_conf_show(bool secret, const string& l)
{
    return secret ? string("srt_passphrase ***") : l;
}

/*
 * The webhook and stats URLs may carry a token in their query (or userinfo):
 * their lines and values are logged through sls_url_for_log.
 */
static bool sls_conf_line_is_url(const string& raw)
{
    static const char *keys[] = {"on_event_url", "stat_post_url"};
    string::size_type b = raw.find_first_not_of(" \t");
    if (b == string::npos) {
        return false;
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        size_t n = strlen(keys[i]);
        if (raw.compare(b, n, keys[i]) == 0 &&
            (raw.size() == b + n || raw[b + n] == ' ' || raw[b + n] == '\t')) {
            return true;
        }
    }
    return false;
}

// Shown as "<key> " + sls_url_for_log(<value>), the value cut exactly as the
// parser cuts it (comment stripped, tabs to spaces, trimmed, trailing ';'
// dropped, everything after the key). No other part of the raw line is ever
// shown, so nothing past a '#', ';' or space inside the URL can leak.
static string sls_conf_show_url_line(const string& raw)
{
    string l = raw;
    string::size_type hash = l.find('#');
    if (hash != string::npos) {
        l = l.substr(0, hash);
    }
    l = replace_all(l, "\t", " ");
    l = trim(l);
    if (!l.empty() && l[l.length() - 1] == ';') {
        l = l.substr(0, l.length() - 1);
        l = trim(l);
    }
    string::size_type sp = l.find(' ');
    if (sp == string::npos) {
        return l;                                   // the key alone
    }
    string key = l.substr(0, sp);
    string v   = l.substr(sp + 1);
    v = trim(v);
    return key + " " + sls_url_for_log(v.c_str());
}

static string sls_conf_show_line(bool secret, bool url, const string& l)
{
    return url ? sls_conf_show_url_line(l) : sls_conf_show(secret, l);
}

/*
 * Parses the lines of one block up to its closing '}' (or to the end of the
 * file at depth 0). b is the block being filled and p_runtime describes its
 * directives; at depth 0 b is the dummy root, which has no directives and may
 * only contain the 'srt' block. open_line is the line the block was opened on.
 * Every child block is linked under b, and after its '}' the current block is
 * b again.
 */
static int sls_conf_parse_block(ifstream& ifs, int& line, sls_conf_base_t * b, sls_runtime_conf_t * p_runtime, int depth, int open_line)
{
    int ret = SLS_OK;
    sls_conf_base_t * block = NULL;
    sls_conf_base_t * last_child = NULL;
    sls_runtime_conf_t * block_runtime = NULL;
    string str_line, str_line_last;
    bool last_secret = false;   // str_line_last came from a srt_passphrase line
    string n, v, line_end_flag;
    int index ;

    while(getline(ifs, str_line))
    {
        line ++;
        const bool secret = sls_conf_line_is_secret(str_line);
        const bool url    = sls_conf_line_is_url(str_line);
        sls_log(SLS_LOG_TRACE, "line:%d='%s'", line, sls_conf_show_line(secret, url, str_line).c_str());
        //remove #
        index = str_line.find('#');
        if (index != -1) {
            str_line = str_line.substr(0, index);
        }
         //trim and replace '\t'
        str_line = replace_all(str_line, "\t", " ");
        str_line = trim(str_line);
        if (str_line.length() == 0) {
            sls_log(SLS_LOG_TRACE, "line:%d='%s', is comment.", line, sls_conf_show_line(secret, url, str_line).c_str());
            continue;
        }

        //check if the last char is ';', '{', '}'
        line_end_flag = str_line.substr(str_line.length() - 1);

        if (line_end_flag == ";") {
            if (depth == 0) {
                sls_log(SLS_LOG_ERROR, "line:%d='%s', not found block.", line, sls_conf_show_line(secret, url, str_line).c_str());
                ret = SLS_ERROR;
                break;
            }
            //key value
            str_line = str_line.substr(0, str_line.length() - 1);

            str_line = replace_all(str_line, "\t", " ");
            str_line = trim(str_line);

            //split by space
            int index = str_line.find(' ');
            if (index == -1) {
                sls_log(SLS_LOG_ERROR, "line:%d='%s', no space separator.", line, sls_conf_show_line(secret, url, str_line).c_str());
                ret = SLS_ERROR;
                break;
            }
            n = str_line.substr(0, index);
            v = str_line.substr(index + 1, str_line.length() - (index + 1));
            v = trim(v);

            sls_conf_cmd_t * it = sls_conf_find(n.c_str(), p_runtime->conf_cmd, p_runtime->conf_cmd_size);
            if (!it) {
                sls_log(SLS_LOG_ERROR, "line:%d='%s', wrong name='%s'.", line, sls_conf_show_line(secret, url, str_line).c_str() ,n.c_str());
                ret = SLS_ERROR;
                break;
            }
            const char * r = it->set(v.c_str(), it, b);
            if (r != SLS_CONF_OK) {
                sls_log(SLS_LOG_ERROR, "line:%d, set failed, %s, name='%s', value='%s'.", line, r ,n.c_str(), (secret ? string("***") : url ? sls_url_for_log(v.c_str()) : v).c_str());
                ret = SLS_ERROR;
                break;
            }
            sls_log(SLS_LOG_TRACE, "line:%d, set name='%s', value='%s'.", line, n.c_str(), (secret ? string("***") : url ? sls_url_for_log(v.c_str()) : v).c_str());

        } else  if (line_end_flag == "{") {
            str_line = str_line.substr(0, str_line.length() - 1);
            str_line = replace_all(str_line, "\t", " ");
            str_line = trim(str_line);

            n = str_line;
            bool n_secret = secret;
            if (n.length() == 0) {
                if (str_line_last.length() == 0) {
                    sls_log(SLS_LOG_ERROR, "line:%d, no name found.", line);
                    ret = SLS_ERROR;
                    break;
                }
                n = str_line_last;
                n_secret = last_secret;
                str_line_last = "";
                last_secret = false;
            }
            if (depth == 0 && n != "srt") {
                sls_log(SLS_LOG_ERROR, "line:%d, the root block must be 'srt', found '%s'.", line, sls_conf_show(n_secret, n).c_str());
                ret = SLS_ERROR;
                break;
            }
            // new block
            block = sls_conf_create_block_by_name(n, block_runtime);
            if (!block) {
                sls_log(SLS_LOG_ERROR, "line:%d, name='%s' not found.", line, sls_conf_show(n_secret, n).c_str());
                ret = SLS_ERROR;
                break;
            }
            if (last_child)
                last_child->sibling = block;
            else
                b->child = block;
            last_child = block;
            ret = sls_conf_parse_block(ifs, line, block, block_runtime, depth + 1, line);
            if (ret != SLS_OK) {
                sls_log(SLS_LOG_ERROR, "line:%d, parse block=‘%s’ failed.", line, block->name);
                ret = SLS_ERROR;
                break;
            }
            // the block is closed: this block continues with the next line
            str_line_last = "";
            last_secret = false;
            continue;
        } else if (line_end_flag == "}" ) {
            if (str_line != line_end_flag) {
                sls_log(SLS_LOG_ERROR, "line:%d=‘%s’, end indicator ‘}’ with more info.", line, sls_conf_show_line(secret, url, str_line).c_str());
                ret = SLS_ERROR;
                break;
            }
            if (depth == 0) {
                sls_log(SLS_LOG_ERROR, "line:%d, '}' without a matching '{'.", line);
                ret = SLS_ERROR;
                break;
            }
            return SLS_OK;
        } else {
            sls_log(SLS_LOG_ERROR, "line:%d='%s', invalid end flag, except ';', '{', '}',", line, sls_conf_show_line(secret, url, str_line).c_str());
            ret = SLS_ERROR;
            break;
        }
        str_line_last = str_line;
        last_secret = secret;
    }
    if (SLS_OK == ret && depth > 0) {
        sls_log(SLS_LOG_ERROR, "line:%d, end of file reached, '}' missing for the block opened at line %d.", line, open_line);
        ret = SLS_ERROR;
    }
    return ret;

}


int sls_conf_open(const char * conf_file)
{
    ifstream    ifs(conf_file);
    int         ret = 0;
    int         line = 0;

    sls_log(SLS_LOG_INFO, "sls_conf_open, parsing conf file='%s'.", conf_file);
    if (!ifs.is_open()) {
        sls_log(SLS_LOG_FATAL, "open conf file='%s' failed, please check if the file exist.", conf_file);
        return SLS_ERROR;
    }

    ret = sls_conf_parse_block(ifs, line, &sls_first_conf, NULL, 0, 0);
    if (ret == SLS_OK && sls_first_conf.child == NULL) {
        sls_log(SLS_LOG_ERROR, "conf file='%s' has no 'srt' block.", conf_file);
        ret = SLS_ERROR;
    }
    if (ret != SLS_OK) {
        sls_log(SLS_LOG_FATAL, "parse conf file='%s' failed.", conf_file);
    }
    return ret;
}

// Releases c, its children and its siblings (NULL: nothing to release).
void sls_conf_release(sls_conf_base_t * c)
{
    while (c) {
        sls_conf_base_t * next = c->sibling;
        sls_conf_release(c->child);
        sls_log(SLS_LOG_DEBUG, "sls_conf_release, delete '%s'.", c->name);
        // A block is a plain struct derived from sls_conf_base_t (created by
        // new and zero-filled, no virtual destructor), so it is freed as raw
        // memory: 'delete c' through the base would pass the base's size.
        ::operator delete(c);
        c = next;
    }
}

void sls_conf_close()
{
    sls_conf_release(sls_first_conf.child);
    sls_first_conf.child = NULL;
}

sls_conf_base_t * sls_conf_get_root_conf()
{
    return sls_first_conf.child;
}


int sls_parse_argv(int argc, char * argv[], sls_opt_t * sls_opt, sls_conf_cmd_t *conf_cmd_opt, int cmd_size)
{
    char opt_name[256] = {0};
    char opt_value[256] = {0};
    char temp[256] = {0};

    int ret  = SLS_OK;
    int i    = 1;//skip
    int len  = cmd_size;

    //special for '-h'
    if (argc == 2) {
        snprintf(temp, sizeof(temp), "%s", argv[1]);
        sls_remove_marks(temp);
        if (strcmp(temp, "-h") == 0) {
            printf("option help info:\n");
            for (i=0; i<len; i++) {
                printf("-%s, %s, range: %.0f-%.0f.\n",
                    conf_cmd_opt[i].name,
                    conf_cmd_opt[i].mark,
                    conf_cmd_opt[i].min,
                    conf_cmd_opt[i].max);
            }
        } else {
            printf("wrong parameter, '%s'.\n", argv[1]);
        }
        return SLS_ERROR;
    }
    while (i < argc) {
        snprintf(temp, sizeof(temp), "%s", argv[i]);
        len = strlen(temp);
        if (len ==0) {
        	printf("wrong parameter, is ''.");
            ret = SLS_ERROR;
            return ret;
        }
        sls_remove_marks(temp);
        if (temp[0] != '-') {
        	printf("wrong parameter '%s', the first character must be '-'.", opt_name);
            ret = SLS_ERROR;
            return ret;
        }
        snprintf(opt_name, sizeof(opt_name), "%s", temp + 1);

        sls_conf_cmd_t * it = sls_conf_find(opt_name, conf_cmd_opt, cmd_size);
        if (!it) {
        	printf("wrong parameter '%s'.", argv[i]);
            ret = SLS_ERROR;
            return ret;
        }
        i ++;
        if (i >= argc) {
            printf("missing value for parameter '-%s'.\n", opt_name);
            return SLS_ERROR;
        }
        snprintf(opt_value, sizeof(opt_value), "%s", argv[i++]);
        sls_remove_marks(opt_value);
        const char * r = it->set(opt_value, it, sls_opt);
        if (r != SLS_CONF_OK) {
        	printf("parameter set failed, %s, name='%s', value='%s'.", r, opt_name, opt_value);
            ret = SLS_ERROR;
            return ret;
        }
    }
    return ret;
}

