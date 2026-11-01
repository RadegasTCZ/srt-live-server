description for config file.

https://github.com/Edward-Wu/srt-live-server/wiki/Directives

## Config file syntax

One directive per line, ended by `;`. A value that contains a `;` followed by a
space (`on_event_url http://h/hook?t=X; on_event_timeout 1000;`) is refused with
an error naming the directive, never its value: before 1.5.4 the second directive
became part of the first one's value, silently. `on_event_url` and
`stat_post_url` may not contain a space at all, and no other value may contain a
`;` at all, except a passphrase (`srt_passphrase abc;def;` is the passphrase
`abc;def`). `#` starts a comment.

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

`user_file` fails closed. A file that cannot be opened, holds no users, has
a malformed line (wrong column count, passphrase outside 10-79 bytes, invalid
or duplicate name, a `#` inside a column) or is accessible to group or others
(`mode & 077`: run `chmod 600`) makes sls refuse to start (non-zero exit, error
naming the line and the column, never a value from the file). A `#` that follows
white space starts a comment; one inside a column is an error, so a passphrase
cannot contain `#` (the same holds for `srt_passphrase` in `sls.conf`). On reload (`sls -c sls.conf -s reload`, i.e. SIGHUP) a bad file is
rejected and the previous users stay active; the result is logged
(`reload ok (N user(s))` at INFO, `reload failed` at ERROR).

A successful reload also applies to the sessions already running. Each one
remembers the user and the direction (publish or play) it was admitted with,
and the passphrase its handshake was checked against. Within about a second
(the owning worker's next pass) sls closes every session whose user is no
longer in the file, or whose passphrase for that direction changed, and logs
one INFO line for each:
`session revoked, <role>, remote=..., stream=..., user='<name>' (<direction>), reason=user removed|passphrase changed`.
Its session end line says `reason=revoked (...)`. No passphrase is logged.
A change to one direction leaves the other alone: a new `play_pass` closes the
user's players, not its publisher. A user whose entry did not change keeps
running, so a reload that only adds a user closes nothing. Connections that
are accepted but still waiting for a worker are checked when a worker takes
them; they are closed before any `on_connect` is sent. Without `user_file` a
reload closes nothing (there are no per-user credentials to compare).

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
- After the sls binary is replaced on disk (an upgrade that installs the new
  file at the same path), `sls -s` still reaches the old process: its
  executable shows as `<path> (deleted)`, which is accepted as the same binary.
  A process running any other executable is refused ("lock holder pid N is not
  sls").
- A second instance with the same `pid_file` refuses to start ("another
  instance is running (lock holder pid N)") without touching the file.

## worker_connections, max_connections_per_ip, reserved_publisher_connections (srt block), max_players_per_stream (server block)

`worker_connections` is the number of roles one worker thread holds (1-1024,
default `300` when the directive is omitted; before 1.5.4 an omitted value was
`0`, and sls then never listened and logged no error; an explicit `0` is refused
at startup with the allowed range). It also caps admission: at most `worker_connections x max(1, worker_threads)`
listener-accepted connections (publishers and players, on all `server` blocks
together) exist at once. `max_connections_per_ip` (default 16, 0 = unlimited,
0-10000) caps the concurrent accepted connections from one IP address. Both
per-IP limits (this one and `max_handshakes_per_ip_per_sec`) count an
IPv4-mapped IPv6 address (`::ffff:a.b.c.d`) as that IPv4 address and an IPv6
address by its /64 prefix.

`reserved_publisher_connections` keeps that many of those slots for publishers:
players together may hold at most the cap minus the reserve, publishers may use
every slot. So the viewers of a public stream, or the players of one streamer who
knows their own `play_pass`, can no longer fill the server and lock every
publisher out. Default when omitted: a tenth of the cap, at most 16 and at least
1 (300 x 1 worker: 16). Only a verified publisher may use the reserve, decided
per connection: one on whose handshake libsrt enforced a passphrase, i.e. a
`user_file` user whose `publish_pass` is set (not `-`), or any publisher when
the server block has `srt_passphrase` and no `user_file`. Every other publisher
(a `-` user, a server without a passphrase) holds a shared slot like a player;
with `on_event_url` it moves to the reserve once the hook answers 200, without a
hook it stays shared. So someone who merely names a publisher stream fills
shared slots at most, never the reserve (over them: `refused ... shared
connection limit ... reached by a publisher without a verified passphrase or hook
admission`). On a server without any passphrase or hook the reserve therefore
protects no publisher. `0` reserves nothing; a value that leaves no slot for
players (the cap or more) stops sls at startup. The startup line
`connection limits: ... reserved for publishers=N (players may hold M)` shows
the values in use.

`max_players_per_stream` (server block, default `0` = unlimited, 0-100000) caps
the concurrent players of one stream (`<domain>/<app>/<stream>` of that server
block). The example configuration sets 100.

All are checked at accept: over a cap the new socket is closed at once and one
INFO line is logged (`refused [ip:port]: per-IP connection limit (N) reached`,
`... total connection limit (N) reached`, `... player connection limit (M; N of
T kept for publishers) reached` or `... stream '<app>/<stream>' has
max_players_per_stream (N) players`; rate-limited like every other
connection-attempt line, see below). Outbound relay connections
(pull and push) never count. A slot is given back when the connection closes,
whichever way it closes.

## Before the handshake: the listen callback, max_handshakes_per_ip_per_sec (srt block)

Every connection request first reaches sls's listen callback, before libsrt
completes the handshake (and before it checks a passphrase). The callback
refuses, with an SRT reject reason (`1402` overload for the limits below), a
request
- over `max_handshakes_per_ip_per_sec` (default `20`, `0` = no limit,
  0-100000): connection requests one IP address may make per second, counted
  as a token bucket of that size refilled at that rate (a burst of 20, then 20 a
  second). Every request counts, also one refused for another reason. sls
  tracks up to 65536 addresses. When that table is full, a new address first
  makes sls drop every address whose bucket has refilled (that loses nothing;
  done at most every 50 ms); only if the table is still full is the new address
  left untracked, and all untracked addresses share one bucket of
  max(4 x the rate, the connection cap) requests a second. The trade-off: under
  a spray from more active addresses than the table holds, newcomers share
  that bounded budget (and may be refused) until buckets refill; handshakes
  stay bounded rather than unlimited;
- with a malformed streamid, or one that is neither a publisher nor a player app;
- over the connection caps above (they are checked again at accept, where the
  slot is taken);
- with `user_file`, for an unknown user.
Refusals are logged as `listen_callback, refused ...` through the refusal budget.

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
an error is logged. With worker_threads >= 1 names are never resolved by a
thread that serves connections; with `worker_threads 0` the single serving
thread is also the main thread, so the relay upstream refresh and a reload's
re-resolution run on it and can briefly block serving.

The query of `on_event_url` and `stat_post_url` may carry a token: wherever sls
logs either URL (also at trace level) it shows `?<redacted>` instead of the
query, and `<redacted>@` instead of any userinfo.

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

The stat host is resolved when sls starts and again on a reload (`-s
reload`), like the webhook host: if the re-resolution fails the previous
address stays in use and an error is logged; after a successful one the next
POST goes to the new address. The URL itself is read only at start: change
`stat_post_url` and restart to post somewhere else. Unlike the webhook host, a
stat host that does not resolve at start (or a URL that is not `http://`) is
not fatal: an error is logged and posting stays disabled until a restart (a
reload does not enable it).

## Connection-attempt log lines

A remote peer can cause a log line with every handshake, so these lines are
bounded. The per-attempt lines (`accept user`, `new client`, `sid`) are DEBUG.
Refusals (bad or unknown streamid, unknown user, no such stream, stream busy,
connection limits, pull refused or failed, a peer gone before accept) share one
budget of one line per second for the whole process: a refusal over the
budget is logged at DEBUG instead, and the next one within the budget says how
many were held back; once a second the main loop also logs `peer lines: N held
back in the last S s` if any were. `log_level debug` therefore shows every
attempt.

Every admitted session (publisher, player, puller, pusher) logs exactly two INFO
lines. Normal operation logs every one; a separate budget of 20 session lines
per second for the whole process (refusals cannot use it up, nor it theirs)
keeps a peer that connects and leaves in a loop from flooding the log. A line
over that budget is logged at DEBUG instead and counted; the next one within
the budget carries `[N session line(s) suppressed]`, and the main loop logs
`session lines: N suppressed` once the burst is over:

    session start, <kind>, remote=<ip>:<port>, stream=<app>/<stream>.
    session end, <kind>, remote=<ip>:<port>, stream=<app>/<stream>, duration=<s>s, reason=<why>.

Without `on_event_url` the start is logged when a worker adopts the
connection; with it, when the hook answers 200. A refused connection (hook
answer other than 200, no answer, refused at accept) logs neither line. The
reason is `peer disconnected` (the peer closed the connection or stopped
answering: libsrt reports both the same way), `idle timeout (no data)`,
`shutdown`, `error` or `closed`. The per-session bookkeeping lines
(map add/remove, epoll, socket close, reader resync, pull manager and relay
open/close) are DEBUG. An unexpected libsrt error caused by a peer (a failed
accept, a read or write on a broken connection) is logged through the refusal
budget at INFO, not at ERROR.

libsrt's own log (its warnings and errors, such as the lines for a rejected
handshake) is written to the sls log as `libsrt: <text>` lines, at WARNING at
most (a peer must not be able to cause ERROR lines; other bytes than printable
ASCII as `\xNN`), never to stderr. They share the refusal budget above (most are caused by
a peer, such as a rejected handshake): one line a second with sls's own
refusals, the rest at DEBUG and counted. libsrt's notice and debug levels are
not enabled.

## log_file (srt block)

Path of the log file, appended to; the directory must exist. sls opens it with
`O_NOFOLLOW` and mode 0640 before it drops privileges, then checks the open file:
a symlink, a device or other non-regular file, a file with more than one link, or
a file owned by another user (when sls starts as root, a log pre-created for the
service user counts) stops sls from starting (an error names the path),
and so does a file that cannot be opened, which used to leave you with no log
file and no message. Delete a stale file you do not own and start again.

## idle_streams_timeout (server block)

Seconds a role may sit idle before it is dropped: a player with no publisher, a
publisher that sends no data. Range -1 to 86400; `-1` never drops an idle role.
Default `10` when the directive is omitted (1.5.3 and the first 1.5.4 builds
treated an omitted value as `0`, which dropped roles the moment they were idle);
an explicit value, `0` included, is used as written. A relay block has its own
`idle_streams_timeout` (see below), with the same default of `10` when omitted
(it too was `0` before).

## backlog (server block)

Length of the queue of SRT handshakes waiting to be accepted (1-1024). Default
`10` when the directive is omitted (before, an omitted value was `0`, and sls
could not listen on the port). libsrt refuses a handshake while the queue is full,
so a small backlog lets a burst of connection requests (a flood, or many players
reconnecting at once) lock everyone else out until it drains: the example sets
`64`. The listen callback refuses floods before they queue (see above), and the
worker accepts queued connections every few milliseconds.

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
- no player has been attached for `idle_streams_timeout` seconds (relay block,
  -1 to 3600, default 10; `-1` never closes a running pull), or
- its upstream connection ends and no player is attached (also while it waits
  for `reconnect_interval` to retry).
While players are attached a broken pull reconnects every `reconnect_interval`
seconds, as before; the attached players resume when it does. A new player
asking for the stream meanwhile neither connects early nor ends the pull: it
is refused (`waiting to reconnect`) and may retry, so retrying players never
raise the upstream connect rate. The next player request after a pull was
dropped starts a new pull.

A relay block (pull or push) without `upstreams` is a configuration error:
sls refuses to start.

Each upstream is `host:port?streamid=...` or `host:port/app` (the stream name
is appended). Hosts are resolved when the configuration is loaded; a relay
block with an upstream that does not resolve is refused with an error in the
log. The addresses are looked up again only after a relay connection has
failed (it never reached the connected state and carried no data, it broke, or
the upstream disconnected: libsrt cannot tell an upstream that closed from one
that went silent) or failed to open, by the main loop (within about 100 ms,
and at most once per 5 seconds), never by a worker thread and never per packet.
A relay that connected and that sls ended itself (no player was left, its idle
timeout, a shutdown) keeps the cached addresses, so a churn of short pulls costs no lookup; if that lookup fails the previous
addresses stay in use. (With `worker_threads 0` the main thread is also the
serving thread, so this lookup runs on it and can briefly block serving.)
An upstream with a `?` but no `streamid=` is refused when the relay tries to
connect (error logged, no socket left open).

## Command line

    sls -c sls.conf [-l level]      run the server
    sls -c sls.conf -s reload       re-read user_file, re-resolve the on_event_url and stat_post_url hosts (SIGHUP)
    sls -c sls.conf -s stop         stop the running server (SIGINT)

`-s` finds the running server through the configured `pid_file`, so it needs
the same `-c` as the server. It signals the process that holds the lock on that
file only if that process runs this binary: the same executable, or, because a
non-dumpable server's `/proc/<pid>/exe` cannot be read, one whose program name
(`/proc/<pid>/comm`) is this binary's file name. A reload does exactly this: it re-reads every
`user_file` (a rejected file keeps the previous users; an accepted one closes
the sessions whose user it removed or whose passphrase it changed, see
`user_file`) and re-resolves the
`on_event_url` host of every server block and the `stat_post_url` host (a
host that does not resolve keeps its previous address); each outcome is logged
at INFO or ERROR. Relay upstream hosts are not re-resolved by a reload: they
are looked up again after a relay connection fails (see above). Every other
`sls.conf` change needs a restart.

    slc -r <srt url> [-o out.ts] [-c n]     play (optionally into a file; -c forks n-1 more players)
    slc -r <srt url> -i in.ts [-l 1]        push a TS file, -l 1 loops it

Every `slc` flag takes a value. A missing or too long value (over 1023
characters), an unknown flag, a number out of range (`-c` 0-256, `-l` 0 or 1)
or a missing `-r` is an error: slc says so and exits with status 1.

## srt_passphrase

Listener-wide passphrase. The value is never written to the log; config lines
setting it are logged as `srt_passphrase ***`.

It is one key for the whole listener, held by publishers and players alike, so
anyone who can play a stream can publish with the same key: use it only when one
person runs the server and every client. With more than one streamer use
`user_file`.

## Log redaction

Anything from the configuration that is shown in a log line (the trace of every
config line, parse errors, relay upstreams, the URLs a relay opens) has its
userinfo and everything after the first `?` replaced by `<redacted>`. That covers
a mistyped directive name as well as `on_event_url`, `stat_post_url` and the
`?streamid=...` of a relay upstream. `srt_passphrase` is logged as `***`.

## Hardening

sls clears its dumpable flag at start (`prctl(PR_SET_DUMPABLE, 0)`), so the
passphrases in memory are not reachable through a core dump, `ptrace` or
`/proc/<pid>/mem` by the same user. The side effects: no core file when it
crashes, no `gdb -p`, and `/proc/<pid>/exe` and `/proc/<pid>/fd` are unreadable
(`sls -s reload|stop` copes with that). To debug a crash start sls with the
environment variable `SLS_DUMPABLE=1`, which keeps the process dumpable (also
after the privilege drop) and logs a WARNING saying so; unset it afterwards.

A lock that cannot be taken aborts the process with a message on stderr instead
of running the critical section unlocked.

## Startup warnings

sls logs a WARNING at start when
- a server block has no authentication: no `user_file`, no `srt_passphrase` and
  no `on_event_url`. Anyone who can reach the port may then publish and play;
- it runs as root and `user` is not set in the srt block, so the threads that
  serve peers keep full privileges.

