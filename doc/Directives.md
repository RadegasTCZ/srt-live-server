description for config file.

https://github.com/Edward-Wu/srt-live-server/wiki/Directives

## Streamid format

A streamid is `<domain>/<app>/<stream>`: exactly two `/`, and each of the three
parts is 1-128 characters from `A-Za-z0-9._-` and not `.` or `..`. Publishers
use `domain_publisher`/`app_publisher`, players `domain_player`/`app_player`
(the same pair of values decides the role everywhere, including per-user
authentication). A connection whose streamid does not parse is refused before
any state is created; the refusal is logged with the value escaped (non-printable
bytes as `\xNN`) and truncated.

## user_file

Path to the per-user passphrase file (format: `sls-users.conf.example`).
When set, every connection must name a listed user in its streamid and use
that user's publish or play passphrase (`-` = plaintext allowed for that
direction); `srt_pbkeylen` applies and encryption is enforced.

`user_file` fails closed. A file that cannot be opened, holds no users, or has
a malformed line (wrong column count, passphrase outside 10-79 bytes, invalid
or duplicate name) makes sls refuse to start (non-zero exit, error naming the
line). On reload (`sls -c sls.conf -s reload`, i.e. SIGHUP) a bad file is
rejected and the previous users stay active; the result is logged
(`reload ok (N user(s))` at INFO, `reload failed` at ERROR).

## pid_file

Path of the pid file (default `/run/sls/sls.pid`), written at startup.
`sls -c sls.conf -s reload|stop` reads the same path, so `-s` needs `-c`; the
CLI prints `reload signal sent; result in log` and the outcome is in the
server log.

sls never creates the directory and never follows a symlink at the file name
(it opens with `O_NOFOLLOW`): if the directory is missing or not writable, or
the path is a symlink, startup fails with an error naming the path. Create the
directory, point `pid_file` at a writable place, or under systemd set
`RuntimeDirectory=sls`.

Security model:
- The directory must be writable only by root or the service user (never a
  shared directory such as `/tmp`).
- The file is created with mode 0600. An existing file with group/other
  permissions, another owner or several links is refused at startup (delete it
  and start again).
- The kernel's lock table is the source of truth, not the file's contents. A
  running sls holds an `fcntl` lock on the file through a read-only descriptor
  (the writable one is closed before privileges are dropped); `sls -s` asks the kernel
  which process holds it, checks that process runs the same sls binary, and
  signals that pid. The pid written in the file is informational only.
- A leftover file from a previous run is harmless: with no lock holder `sls -s`
  says "sls is not running (stale pid file)" and signals nothing, and the next
  start simply reuses the file. sls does not delete the file at exit.
- `sls -s` must be run as root (or as the user sls itself runs as, if it never
  runs as root): the file is root-owned 0600, and the daemon, once it has
  dropped privileges, is not dumpable so `/proc/<pid>/exe` of it is only
  readable by root.
- After the sls binary is replaced on disk (upgrade), `sls -s` refuses with
  "lock holder pid N is not sls" until the old process has been restarted,
  because the running process's executable no longer matches the new file.
- A second instance with the same `pid_file` refuses to start ("another
  instance is running (lock holder pid N)") without touching the file.

## worker_connections, max_connections_per_ip (srt block)

`worker_connections` is the number of roles one worker thread holds. It also
caps admission: at most `worker_connections x max(1, worker_threads)`
listener-accepted connections (publishers and players, on all `server` blocks
together) exist at once. `max_connections_per_ip` (default 16, 0 = unlimited,
0-10000) caps the concurrent accepted connections from one IP address.

Both are checked at accept: over either cap the new socket is closed at once
and one INFO line is logged (`refused [ip:port]: per-IP connection limit (N)
reached` or `... total connection limit (N) reached`; rate-limited like every
other connection-attempt line, see below). Outbound relay connections
(pull and push) never count. A slot is given back when the connection closes,
whichever way it closes.

## on_event_url, on_event_timeout (server block)

`on_event_url http://host[:port][/path][?query];` makes sls call a webhook
with a plain HTTP/1.1 `GET` for each publisher and player:

    <url>?on_event=on_connect|on_close&role_name=publisher|player&srt_url=<streamid>&remote_ip=<ip>&remote_port=<port>

The values are percent-encoded (the stream id's `/` is sent as `%2F`); the
joiner is `&` when the URL already has a query, and a URL without a path gets
`/`. Only `http://` is supported. The answers are read case-insensitively and
may carry `Content-Length`, be chunked, or be delimited by the close.

- `on_connect` is sent when a worker adopts the connection. The role is held
  (no data in or out) until the answer arrives; status 200 admits it, anything
  else - another status, a malformed answer, a connection error, no answer
  within `on_event_timeout` - closes it (`http refused` in the log).
  `on_event_timeout` is in ms (100-30000, default 2000). Waiting for a hook
  never blocks other roles of the worker.
- `on_close` is sent once for every `on_connect` that was sent, also when the
  hook refused or timed out, after the connection is gone. It is
  fire-and-forget: the answer is not used and the request is dropped after 2 s.
  A connection that expired while still queued for a worker sent neither.

The hook host is resolved when sls starts (a host that does not resolve, or a
URL that is not `http://`, stops sls from starting) and again on a reload
(`-s reload`); if the re-resolution fails the previous address stays in use and
an error is logged. Names are never resolved while serving connections.

## stat_post_url, stat_post_interval (srt block)

`stat_post_url http://host[:port][/path];` makes sls POST a JSON document
describing the running streams every `stat_post_interval` seconds (default 5;
the interval also paces how often the per-role figures are refreshed). The
request carries `Content-Type: application/json`; the response is ignored.

    {"server":"sls","version":"1.5.4","generated_ms":1730000000000,
     "roles":[{"kind":"publisher","stream":"uplive.sls.com/live/alice",
               "remote_ip":"1.2.3.4","remote_port":5000,
               "start_ms":1730000000000,"kbitrate":5980}]}

- `kind` is `publisher`, `player`, `puller` or `pusher`. Listeners are not
  reported; `roles` is `[]` when nothing is connected.
- `stream` is `<domain>/<app>/<stream>` as the role asked for it (a player
  shows the publisher domain). `remote_ip`/`remote_port` are the peer; for a
  puller or pusher that is the upstream or downstream relay server.
- `generated_ms` and `start_ms` are Unix epoch milliseconds. `kbitrate` is an
  integer in kbit/s, averaged over the last second the role moved data.
- Strings are JSON-escaped; all numbers are JSON numbers.

The stat host is resolved once, when sls starts, and is not re-resolved on a
reload: change `stat_post_url` and restart to point at a different address.
Unlike the webhook host, a stat host that does not resolve (or a URL that is
not `http://`) is not fatal: an error is logged and posting stays disabled
for that run.

## Connection-attempt log lines

A remote peer can cause a log line with every handshake, so these lines are
bounded. The per-attempt lines (`accept user`, `new client`, `sid`) are DEBUG.
Refusals (bad or unknown streamid, unknown user, no such stream, stream busy,
connection limits, pull refused or failed, a peer gone before accept) share one
budget of one line per second for the whole process: a refusal over the
budget is logged at DEBUG instead, and the next one within the budget says how
many were held back; once a second the main loop also logs `peer lines: N held
back in the last S s` if any were. `log_level debug` therefore shows every
attempt. The start of an admitted session (`new publisher[...]`, `new
player[...]`) is always logged at INFO.

## admission_timeout (server block)

Milliseconds (100-60000, default 5000) an accepted connection may wait for a
free worker slot. The worker also holds the listener and relay roles, so an
accepted connection can be queued while the workers are full; past this
timeout it is closed and `CSLSRoleList::expire, expired queued <role>` is
logged.

## max_pull_streams (relay block), pull relay lifetime

A pull relay is started by the first player asking for a stream that has no
publisher. `max_pull_streams` (default 16, 1-1024) caps how many streams one
pull relay block (one app) pulls at once; a request beyond it is refused
(`add_relay_manager, refused ... max_pull_streams=N reached`). With
`user_file` set, only stream names listed there can start a pull.

A pull exists only for its players. It is dropped, with no reconnect, when
- no player has been attached for `idle_streams_timeout` seconds (`-1` never
  closes a running pull), or
- its upstream connection ends and no player is attached (also while it waits
  for `reconnect_interval` to retry).
While players are attached a broken pull reconnects every `reconnect_interval`
seconds, as before. The next player request starts a new pull.

A relay block (pull or push) without `upstreams` is a configuration error:
sls refuses to start.

Each upstream is `host:port?streamid=...` or `host:port/app` (the stream name
is appended). Hosts are resolved when the configuration is loaded; a relay
block with an upstream that does not resolve is refused with an error in the
log. The addresses are looked up again only after a relay connection has
ended, never per packet. An upstream with a `?` but no `streamid=` is refused
when the relay tries to connect (error logged, no socket left open).

## Command line

    sls -c sls.conf [-l level]      run the server
    sls -c sls.conf -s reload       re-read the users file and re-resolve hook/relay hosts (SIGHUP)
    sls -c sls.conf -s stop         stop the running server (SIGINT)

`-s` finds the running server through the configured `pid_file`, so it needs
the same `-c` as the server.

    slc -r <srt url> [-o out.ts] [-c n]     play (optionally into a file; -c forks n-1 more players)
    slc -r <srt url> -i in.ts [-l 1]        push a TS file, -l 1 loops it

Every `slc` flag takes a value. A missing or too long value (over 1023
characters), an unknown flag, a number out of range (`-c` 0-256, `-l` 0 or 1)
or a missing `-r` is an error: slc says so and exits with status 1.

## srt_passphrase

Listener-wide passphrase. The value is never written to the log; config lines
setting it are logged as `srt_passphrase ***`.

