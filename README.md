Introduction
============

srt-live-server(SLS) is an open source live streaming server for low latency based on Secure Reliable Transport(SRT).
The latency of transport by SLS is usually under 1 second through the internet.

This is a fork from [Edward-Wu/srt-live-server](https://github.com/Edward-Wu/srt-live-server) to address missing features, updates the code for libsrt 1.5 support and more, thanks to the paid AI assistance the modern age has.

Requirements
============

Please install the SRT library first, refer to SRT repository [Haivision/srt](https://github.com/Haivision/srt) for system environment.
SLS runs on Linux-based OS. I don't expect anyone running such a server on Windows on a low-cost VPS.

Compile
=======

$ make

bin file is generated in subdir of 'bin'.

Directives
===========

The directives added or changed in this fork are described in
[doc/Directives.md](doc/Directives.md), and every directive is listed, commented, in
[sls.conf.example](sls.conf.example). The original directives are described in the
upstream wiki:
[Edward-Wu/srt-live-server/wiki/Directives](https://github.com/Edward-Wu/srt-live-server/wiki/Directives)

Usage
=====

$ cd bin

- Prints help information
------------------
$ ./sls -h

- Runs with default config file
------------------------------
$ cp ../sls.conf.example ../sls.conf      # first run only
$ ./sls -c ../sls.conf

The pid file defaults to `/run/sls/sls.pid`; sls does not create the directory.
As a normal user set `pid_file` in the srt block to an absolute path you own, e.g.
`pid_file /home/you/sls/sls.pid;` (under systemd use `RuntimeDirectory=sls`). Prefer
an absolute path: a relative one is resolved against the current directory of each
`sls` invocation, so `sls -s reload` run from another directory would not find it.
See `doc/Directives.md`.

- Reload users / stop a running server
--------------------------------------
$ ./sls -c ../sls.conf -s reload
$ ./sls -c ../sls.conf -s stop

If you want per-user authentication, also copy the users file template:

$ cp ../sls-users.conf.example ../sls-users.conf
$ # edit ../sls-users.conf, then uncomment `user_file` in sls.conf

- Client tool
-------------
$ ./slc -r 'srt://host:8080?streamid=live.example.com/live/test' -o out.ts
$ ./slc -r 'srt://host:8080?streamid=uplive.example.com/live/test' -i in.ts -l 1

`-r` (required) is the srt url, `-o` writes what is played to a file, `-i` pushes a
TS file (`-l 1` loops it), `-c` forks extra players (0-256). Every flag takes a
value; a missing or over-long value, an unknown flag or a bad number prints an
error and exits with status 1.

Upgrading from 1.5.3
====================

1.5.4 is a security and robustness release. These behaviours differ from 1.5.3;
check your config and tooling before you restart.

- **Authentication fails closed.** With `user_file` set, a file that cannot be
  opened, has no users or has a malformed line stops sls from starting; on reload
  it is rejected and the previous users stay. Publisher or player is decided by
  the streamid's domain and app (in per-user mode a cross-role passphrase no
  longer works), and `srt_pbkeylen` and enforced encryption apply per user. With
  only a listener-wide `srt_passphrase` everyone holds the same key, publishers
  and players alike: that mode is single-tenant only, use `user_file`.
- **Streamids are strict.** Exactly `domain/app/stream`; each part is 1-128
  characters of `A-Za-z0-9._-` and not `.` or `..`. Anything else is refused, and
  log lines show the rejected value escaped and truncated.
- **pid file.** The location is the new `pid_file` directive (default
  `/run/sls/sls.pid`, was `/tmp/sls/pid.txt`). sls does not create the directory,
  and the file is created 0600 without following symlinks. `sls -s reload|stop`
  needs the same `-c sls.conf`. Create `/run/sls` (systemd: `RuntimeDirectory=sls`)
  or set `pid_file`; sls refuses to start if it cannot write the file.
- **Connection limits.** At most `worker_connections` x `max(1, worker_threads)`
  accepted connections exist at once, and `max_connections_per_ip` (default 16,
  0 = unlimited) caps one address. Over either cap the connection is closed at
  accept. An accepted connection waits at most `admission_timeout` ms (default
  5000) for a worker slot. Per-attempt connection lines are DEBUG now, and
  refusals are logged at most once per second (with a count of the rest);
  `log_level debug` shows them all.
- **Webhook.** `on_connect` is an admission gate: anything but a `200` answer, no
  answer within `on_event_timeout` ms (default 2000), or an unreachable hook closes
  the connection. `on_close` is sent once for every `on_connect`. The hook host is
  resolved at start and on reload, and a host that does not resolve stops sls from
  starting. The query parameter is `on_event=`.
- **Stats POST.** `stat_post_url` receives a JSON document (`stat_post_interval`
  seconds, default 5) instead of the broken body 1.5.3 sent; see
  `doc/Directives.md` for the schema. The stats host is resolved at start and
  re-resolved on reload (a failure keeps the previous address); one that does not
  resolve at start only disables posting.
- **Reload** (`sls -s reload`, SIGHUP) re-reads `user_file` and re-resolves the
  `on_event_url` and `stat_post_url` hosts, nothing else; relay upstreams are
  re-resolved after a relay connection fails (never connected, broke, or the
  upstream disconnected; an end sls chose itself keeps the addresses, at most one
  lookup per 5 s). Other config changes
  need a restart.
- **Relays.** A relay block without `upstreams` is a configuration error. Upstream
  hosts are resolved when the configuration loads, `host:port/app` honours the
  port, and a pull is capped by `max_pull_streams` (default 16). A pull now lives
  only for its players: once idle or ended with none attached it is dropped, not
  reconnected, until a player asks again.
- **Startup warnings.** sls logs a WARNING when a server block has no
  authentication at all (no `user_file`, `srt_passphrase` or `on_event_url`) and
  when it runs as root without `user`. The shipped `sls.conf.example` is an open
  server until you set `user_file`; its pull example now uses a finite
  `idle_streams_timeout`.
- **One directive per line.** A value containing `; ` (a second directive on the
  same line) and a URL containing a space are config errors; they used to be
  swallowed into the first directive's value.
- **Session end reason.** `reason=` is `peer disconnected`, `idle timeout (no
  data)`, `shutdown`, `error` or `closed`, taken from the socket state;
  `clean disconnect` and `idle timeout (peer silent)` are gone (libsrt reports both
  as one broken state, and the old lines read a stale error code).
- **Idle timeout default.** An omitted server-block `idle_streams_timeout` is `10`
  seconds (it was `0`, which dropped idle roles at once); `-1` still disables it.
- **Log file.** `log_file` is opened without following symlinks (mode 0640) and
  must be a plain file you own (when sls starts as root: owned by root) with one link; otherwise, or when it cannot be
  opened, sls refuses to start. Session start/end lines are limited to 20 per
  second process-wide (the rest are counted in a summary); see
  `doc/Directives.md`.
- **Users file.** It must not be accessible to group or others (`chmod 600`), and
  a `#` inside a passphrase is an error (also for `srt_passphrase`). Its errors
  name the line and column, never a value. The process is non-dumpable, a failed
  internal lock aborts it, and the query and userinfo of any config text or relay
  URL are redacted in log lines.
- **Process.** Privileges are dropped after bind and before any worker starts;
  SIGTERM stops sls like SIGINT; startup failures exit non-zero; the config parser
  rejects unbalanced braces. Passphrases are never written to the log.
- **Media.** A re-publish no longer leaks the previous publisher's bytes to
  connected players, and a late-joining player gets a PAT/PMT/parameter-set
  bootstrap first (H.264 and HEVC).
- **slc.** Arguments are length-checked and every flag needs a value (see above).

Test
====

SLS only supports the MPEG-TS format streaming. 

Test with OBS
---------------

The OBS supports SRT protocol to publish stream since release v25.0. You can use the following example URL to custom service:

*srt://[your.sls.ip]:8080?streamid=uplive.sls.com/live/test*

Use SLS with docker
---------------------------
Please refer to [docker.com/r/ravenium/srt-live-server](https://hub.docker.com/r/ravenium/srt-live-server) for now. *I will see if more up-to-date docker image is something I would be able to cook.*

Note:
=====

This fork is in work-in-progress as of 04/2026. I hope, that I can bring a fully featured SRT relay server for IRL streamers to enjoy low-cost and effective setups.


