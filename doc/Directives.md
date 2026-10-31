description for config file.

https://github.com/Edward-Wu/srt-live-server/wiki/Directives

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
- A second instance with the same `pid_file` refuses to start ("another
  instance is running (lock holder pid N)") without touching the file.

## srt_passphrase

Listener-wide passphrase. The value is never written to the log; config lines
setting it are logged as `srt_passphrase ***`.

