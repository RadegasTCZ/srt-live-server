description for config file.

https://github.com/Edward-Wu/srt-live-server/wiki/Directives

## user_file

Path to the per-user passphrase file (format: `sls-users.conf.example`).
When set, every connection must name a listed user in its streamid and use
that user's publish or play passphrase (`-` = plaintext allowed for that
direction).

`user_file` fails closed. A file that cannot be opened, holds no users, or has
a malformed line (wrong column count, passphrase outside 10-79 bytes, invalid
or duplicate name) makes sls refuse to start (non-zero exit, error naming the
line). On reload (`sls -c sls.conf -s reload`, i.e. SIGHUP) a bad file is
rejected and the previous users stay active; the result is logged
(`reload ok (N user(s))` at INFO, `reload failed` at ERROR).
