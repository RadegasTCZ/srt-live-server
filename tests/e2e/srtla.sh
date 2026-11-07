#!/bin/bash
# SRTLA (stage 2) e2e cases. ASAN sls by default; loopback only.
#   tests/e2e/srtla.sh [case ...]       (run from anywhere; default: all)
# Env: SLS_BIN (default bin-asan/sls), WORK (default ~/sls-1.6.0-tests/srtla-e2e),
#      PORT (public port, default 8094), SRTLA_SEND (stock srtla_send binary,
#      optional black-box case; default ~/srtla-tools/srtla/srtla_send).
# The bonded sender is tests/e2e/srtla_sender.py (2-3 links from 127.0.0.x)
# in front of srt-live-transmit; the player plays plain SRT from the same port.
# Prints one PASS/FAIL line per case; exits non-zero if any case failed.

REPO=$(cd "$(dirname "$0")/../.." && pwd)
SLS_BIN=${SLS_BIN:-$REPO/bin-asan/sls}
WORK=${WORK:-$HOME/sls-1.6.0-tests/srtla-e2e}
PORT=${PORT:-8094}
LOCAL=${LOCAL:-9110}
SENDER="python3 -I $REPO/tests/e2e/srtla_sender.py"
SRTLA_SEND=${SRTLA_SEND:-$HOME/srtla-tools/srtla/srtla_send}
SRC_TS=${SRC_TS:-$HOME/sls-1.6.0-tests/measure/src.ts}
FAILS=0
mkdir -p "$WORK/asan"
cd "$WORK" || exit 1

pass(){ echo "PASS $1${2:+ ($2)}"; }
fail(){ echo "FAIL $1${2:+ ($2)}"; FAILS=$((FAILS+1)); }
nap(){ timeout "$1" tail -f /dev/null; true; }
rand(){ head -c 18 /dev/urandom | base64 | tr '+/' 'xy'; }
[ -s users.conf ] || ( umask 077; printf 'm1 %s %s\n' $(rand) $(rand) > users.conf )
key(){ awk -v c=$1 '$1=="m1"{print $c}' users.conf; }   # 2 publish, 3 play

write_conf(){ # extra server lines...
    {
        printf 'srt {\n    worker_threads 1;\n    log_file logs-error.log;\n    log_level %s;\n' "${LOG_LEVEL:-info}"
        printf '    pid_file %s/sls.pid;\n' "$WORK"
        [ -n "$STAT_URL" ] && printf '    stat_post_url %s;\n    stat_post_interval 1;\n' "$STAT_URL"
        printf '    server {\n        listen %s;\n        latency 120;\n' "$PORT"
        printf '        domain_publisher uplive.m;\n        domain_player live.m;\n'
        printf '        idle_streams_timeout 10;\n        user_file %s/users.conf;\n        srtla on;\n' "$WORK"
        for l in "$@"; do printf '        %s\n' "$l"; done
        printf '        app {\n            app_publisher live;\n            app_player live;\n        }\n    }\n}\n'
    } > sls.conf
}
SLS_PID=
start_sls(){
    rm -f sls.pid out.log asan/*
    ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1:log_path=$WORK/asan/asan \
    UBSAN_OPTIONS=print_stacktrace=1:log_path=$WORK/asan/ubsan \
    setsid nice stdbuf -oL "$SLS_BIN" -c sls.conf > out.log 2>&1 < /dev/null &
    for i in $(seq 50); do
        [ -s sls.pid ] && awk '/CSLSFrontDoor, started/{f=1} END{exit !f}' out.log && break
        nap 0.2
    done
    SLS_PID=$(cat sls.pid 2>/dev/null)
}
stop_sls(){
    [ -n "$SLS_PID" ] || return
    kill -INT "$SLS_PID" 2>/dev/null
    for i in $(seq 100); do kill -0 "$SLS_PID" 2>/dev/null || break; nap 0.2; done
    kill -0 "$SLS_PID" 2>/dev/null && echo "  !! sls $SLS_PID did not exit"
    SLS_PID=
}
asan_reports(){ cat asan/* 2>/dev/null | awk '/runtime error|ERROR: AddressSanitizer|ERROR: LeakSanitizer/' | wc -l; }
counter(){ awk -v n="$1" '/front door stopped/{ if (match($0, " " n "=[0-9]+")) v=substr($0,RSTART+length(n)+2,RLENGTH-length(n)-2) } END{print v+0}' out.log; }
# INFO+ lines that are per packet would grow with the packet count: count the
# lines that are not session/group/link/startup/stop lines
noisy_lines(){ awk '/ (INFO|WARNING|ERROR):/ && !/session (start|end)|SRTLA group|CSLS(Manager|Listener|Group|Thread|EpollThread|FrontDoor), (start|started|stop|front door stopped)|CSLSManager|CSLSGroup|CSLSThread|CSLSEpollThread|CSLSListener::(start|init_conf_app|load_users|stop)|libsrt_(setup|listen)|sls_conf|sls_write_pid|PSI bootstrap|CSLSMap|exit,|^.{24}SLS INFO: $|held back/' out.log | wc -l; }

# kill only our own helpers (never pkill -f)
kill_cmd(){ # comm pattern
    for p in $(pgrep -x "$1"); do
        tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | awk -v p=$p -v pat="$2" 'index($0, pat){print p}'
    done | xargs -r kill 2>/dev/null
}

# bonded publish: sender (args) + srt-live-transmit caller with stats; returns at once
SPID= PPID_=
bonded_publish(){ # secs sender-args...
    local secs=$1; shift
    rm -f sender.json pub.csv
    $SENDER --server 127.0.0.1:$PORT --local $LOCAL --duration "$secs" "$@" > sender.json 2> sender.err &
    SPID=$!
    nap 1
    ( nice ffmpeg -nostdin -loglevel error -re -stream_loop -1 -i "$SRC_TS" -c copy -f mpegts - 2>/dev/null | \
      timeout "$secs" srt-live-transmit -q $PUB_OPTS -chunk 1316 -s 1000 -pf csv -statsout pub.csv file://con \
        "srt://127.0.0.1:$LOCAL?streamid=uplive.m/live/m1&passphrase=${PUB_PASS:-$(key 2)}&latency=2000&mode=caller" ) > /dev/null 2>&1 &
    PPID_=$!
}
bonded_wait(){
    wait "$SPID" 2>/dev/null
    kill_cmd srt-live-transm "127.0.0.1:$LOCAL"; kill_cmd ffmpeg "$SRC_TS"
    wait "$PPID_" 2>/dev/null
}
play(){ # secs -> bytes (play.ts, play.csv)
    rm -f play.ts play.csv
    timeout "$1" srt-live-transmit -q -s 1000 -pf csv -statsout play.csv \
        "srt://127.0.0.1:$PORT?streamid=live.m/live/m1&passphrase=$(key 3)&mode=caller&latency=2000" file://con > play.ts 2>/dev/null
    stat -c%s play.ts 2>/dev/null || echo 0
}
pub_stat(){ # column -> sum over pub.csv
    awk -F, -v c="$1" 'NR==1{for(i=1;i<=NF;i++) if($i==c) k=i; next} k{s+=$k} END{print s+0}' pub.csv 2>/dev/null
}
sender_field(){ python3 -I -c 'import json,sys; d=json.load(open("sender.json")); print(eval(sys.argv[1]))' "$1" 2>/dev/null; }
decode_errors(){ nice ffmpeg -nostdin -v error -i play.ts -f null - 2>&1 | awk '!/co located POCs|reference picture missing|Missing reference picture/' | wc -l; }

start_sink(){ # port -> POST bodies appended to stats.jsonl
    rm -f stats.jsonl
    python3 -I - "$1" "$WORK/stats.jsonl" > sink.log 2>&1 <<'EOF2' &
import http.server, sys
port, out = int(sys.argv[1]), sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(n)
        with open(out, 'ab') as f:
            f.write(body.replace(b'\n', b' ') + b'\n')
        self.send_response(200); self.send_header('Content-Length', '0'); self.end_headers()
    def log_message(self, *a): pass
http.server.HTTPServer(('127.0.0.1', port), H).serve_forever()
EOF2
    SINK_PID=$!
    nap 0.5
}
stop_sink(){ [ -n "$SINK_PID" ] && { kill "$SINK_PID" 2>/dev/null; wait "$SINK_PID" 2>/dev/null; }; SINK_PID=; }

# raw srtla2 messages from one source: prints the reply types
srtla_raw(){ # src_ip script
    python3 -I - "$PORT" "$@" <<'PYEOF'
import os, socket, struct, sys, time
port, src, script = int(sys.argv[1]), sys.argv[2], sys.argv[3]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind((src, 0)); s.settimeout(0.5)
def ask(pkt):
    s.sendto(pkt, ("127.0.0.1", port))
    try:
        b, _ = s.recvfrom(2048); return b
    except socket.timeout:
        return b""
out = []
ids = []
for step in script.split(','):
    if step == 'reg1':
        b = ask(struct.pack('!H', 0x9200) + os.urandom(256))
        if len(b) == 258: ids.append(b[2:])
        out.append(b[:2].hex() or 'none')
    elif step == 'reg2':      # the last group's id, from a fresh source port
        t = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); t.bind((src, 0)); t.settimeout(0.5)
        t.sendto(struct.pack('!H', 0x9201) + (ids[-1] if ids else os.urandom(256)), ("127.0.0.1", port))
        try: b, _ = t.recvfrom(2048)
        except socket.timeout: b = b""
        out.append(b[:2].hex() or 'none')
    elif step == 'reg2self':  # REG2 from this socket (the REG1 source)
        b = ask(struct.pack('!H', 0x9201) + (ids[-1] if ids else os.urandom(256)))
        out.append(b[:2].hex() or 'none')
    elif step == 'forged':
        b = ask(struct.pack('!H', 0x9201) + os.urandom(256)); out.append(b[:2].hex() or 'none')
    elif step.startswith('ka'):   # a keepalive of N bytes from this socket: echoed length or none
        n = int(step[2:])
        b = ask(struct.pack('!H', 0x9000) + bytes(n - 2))
        out.append(str(len(b)) if b else 'none')
    elif step.startswith('sleep'):
        time.sleep(float(step[5:]))
print(' '.join(out))
PYEOF
}

case_reorder_clean(){
    # link 1 delayed 30 ms (reordering): the buffer hides it from libsrt
    write_conf; start_sls
    bonded_publish 24 --links 127.0.0.2,127.0.0.3 --delay 1:30
    nap 5; local b=$(play 14); bonded_wait; stop_sls
    local r=$(pub_stat pktRetrans) s=$(pub_stat pktSent) l=$(pub_stat pktSndLoss) n=$(noisy_lines)
    local acks=$(counter srtla_acks) de=$(decode_errors)
    if [ "$b" -ge 5000000 ] && [ "$r" -le $((s / 1000)) ] && [ "$acks" -gt 100 ] && [ "$n" = 0 ] \
            && [ "$de" -le 4 ] && [ "$(asan_reports)" = 0 ]; then
        pass srtla_reorder_clean "player $b B, publisher sent=$s retrans=$r naked=$l, srtla_acks=$acks, noisy lines $n, decode errors $de"
    else
        fail srtla_reorder_clean "player $b B, sent=$s retrans=$r naked=$l, acks=$acks, noisy=$n, decode=$de, asan=$(asan_reports)"
    fi
    REORDER_ON_RETRANS=$r
}

case_stats(){
    # the publisher's stats line carries the group: links and reorder counters
    start_sink 18602
    STAT_URL=http://127.0.0.1:18602/sls/stat write_conf; start_sls
    bonded_publish 14 --links 127.0.0.2,127.0.0.3 --delay 1:30
    nap 10; bonded_wait; stop_sls; stop_sink
    local last=$(awk '/"srtla"/' stats.jsonl 2>/dev/null | tail -1)
    local ok=$(printf '%s' "$last" | python3 -I -c '
import json, sys
d = json.loads(sys.stdin.read() or "{}")
def roles(x):
    if isinstance(x, dict):
        if "srtla" in x: yield x
        for v in x.values(): yield from roles(v)
    elif isinstance(x, list):
        for v in x: yield from roles(v)
r = next(roles(d), None)
s = r["srtla"] if r else {}
ls = s.get("links", [])
good = r and r.get("kind") == "publisher" and len(ls) == 2 and all(l["kbitrate"] > 0 and l["packets"] > 0 for l in ls) \
    and 95 <= sum(l["share_pct"] for l in ls) <= 100 and s.get("reordered", 0) > 0 and "held_max_ms" in s
print("ok" if good else "bad", json.dumps(s)[:300])' 2>&1)
    if [ "${ok%% *}" = ok ] && [ "$(asan_reports)" = 0 ]; then
        pass srtla_stats "${ok#ok }"
    else
        fail srtla_stats "$ok"
    fi
}

case_reorder_off_compare(){
    # the same with srtla_reorder_max 0 (pass-through): libsrt now sees the reordering
    write_conf "srtla_reorder_max 0;"; start_sls
    bonded_publish 24 --links 127.0.0.2,127.0.0.3 --delay 1:30
    nap 5; local b=$(play 14); bonded_wait; stop_sls
    local r=$(pub_stat pktRetrans) s=$(pub_stat pktSent) l=$(pub_stat pktSndLoss)
    if [ "$b" -ge 5000000 ] && [ "$l" -gt $(( ${REORDER_ON_RETRANS:-0} * 10 + 50 )) ] && [ "$(asan_reports)" = 0 ]; then
        pass srtla_reorder_off_compare "pass-through: publisher sent=$s naked=$l retrans=$r (buffer on: retrans ${REORDER_ON_RETRANS:-?}), player $b B"
    else
        fail srtla_reorder_off_compare "pass-through: sent=$s naked=$l retrans=$r (on: ${REORDER_ON_RETRANS:-?}), player $b B, asan=$(asan_reports)"
    fi
}

case_link_dies(){
    write_conf; start_sls
    bonded_publish 26 --links 127.0.0.2,127.0.0.3,127.0.0.4 --delay 1:20 --kill 1:9
    nap 4
    local b=$(play 18); bonded_wait; stop_sls
    local left=$(awk '/SRTLA group .* link 127\.0\.0\.3:[0-9]+ left \(timed out\)/' out.log | wc -l)
    local loss=$(awk -F, 'NR==1{for(i=1;i<=NF;i++) if($i=="pktRcvDrop") k=i; next} k{s+=$k} END{print s+0}' play.csv)
    if [ "$b" -ge 7000000 ] && [ "$left" = 1 ] && [ "$loss" = 0 ] && [ "$(asan_reports)" = 0 ]; then
        pass srtla_link_dies "link 127.0.0.3 stopped at 9 s, left once; player $b B, rcv drops $loss"
    else
        fail srtla_link_dies "player $b B, left lines $left, rcv drops $loss, asan=$(asan_reports)"
    fi
}

case_dup_and_drop(){
    write_conf; start_sls
    bonded_publish 20 --links 127.0.0.2,127.0.0.3 --dup-every 40 --drop-every 300
    nap 4; local b=$(play 12); bonded_wait; stop_sls
    local dups=$(sender_field 'd["dups"]') drops=$(sender_field 'd["drops"]') r=$(pub_stat pktRetrans)
    if [ "$b" -ge 4000000 ] && [ "${r:-0}" -ge 1 ] && [ "$(asan_reports)" = 0 ] && [ "$(decode_errors)" -le 2 ]; then
        pass srtla_dup_and_drop "sender duplicated $dups, dropped $drops; publisher retransmitted $r; player $b B"
    else
        fail srtla_dup_and_drop "dups=$dups drops=$drops retrans=$r player=$b asan=$(asan_reports)"
    fi
}

case_late_induction(){
    # a refused attempt first, then the real publisher; at 12 s a late copy of
    # an SRT induction arrives over a link: the live session must not be cut
    write_conf; start_sls
    $SENDER --server 127.0.0.1:$PORT --local $LOCAL --links 127.0.0.2,127.0.0.3 --delay 1:30 \
        --duration 26 --late-induction 12 > sender.json 2> sender.err &
    local sp=$!
    nap 1
    timeout 3 srt-live-transmit -q -a no "srt://127.0.0.1:$LOCAL?streamid=uplive.m/live/nobody&passphrase=xxxxxxxxxxxxxxxx&mode=caller" \
        file://con < /dev/null > /dev/null 2>&1
    nap 0.5
    ( nice ffmpeg -nostdin -loglevel error -re -stream_loop -1 -i "$SRC_TS" -c copy -f mpegts - 2>/dev/null | \
      timeout 22 srt-live-transmit -q -chunk 1316 file://con \
        "srt://127.0.0.1:$LOCAL?streamid=uplive.m/live/m1&passphrase=$(key 2)&latency=2000&mode=caller" ) > /dev/null 2>&1 &
    local pp=$!
    nap 5
    rm -f play.ts
    timeout 16 srt-live-transmit -q "srt://127.0.0.1:$PORT?streamid=live.m/live/m1&passphrase=$(key 3)&mode=caller" file://con > play.ts 2>/dev/null &
    local pl=$!
    local flat=0 prev=0 cur
    for i in $(seq 15); do nap 1; cur=$(stat -c%s play.ts 2>/dev/null || echo 0); [ "$cur" = "$prev" ] && flat=$((flat+1)); prev=$cur; done
    wait $pl 2>/dev/null; wait $sp
    kill_cmd srt-live-transm "127.0.0.1:$LOCAL"; kill_cmd ffmpeg "$SRC_TS"; wait $pp 2>/dev/null
    stop_sls
    local li=$(sender_field 'd.get("late_induction_sent", 0)') pubs=$(awk '/session start, publisher/' out.log | wc -l)
    if [ "$li" = 1 ] && [ "$pubs" = 1 ] && [ "$flat" -le 1 ] && [ "$prev" -ge 5000000 ] && [ "$(asan_reports)" = 0 ]; then
        pass srtla_late_induction "late induction sent, 1 publisher session, player $prev B, flat seconds $flat"
    else
        fail srtla_late_induction "late induction sent=$li, publisher sessions $pubs, player $prev B, flat seconds $flat, asan=$(asan_reports)"
    fi
}

case_caps(){
    write_conf "srtla_max_groups_per_ip 1;" "srtla_max_links_per_group 2;" "srtla_reg_per_ip_per_sec 100;"
    start_sls
    # one group from 127.0.0.20 (REG1 + REG2 from the REG1 source), then a second one:
    # REG1 is stateless (always answered), the per-IP cap refuses its REG2
    local a=$(srtla_raw 127.0.0.20 reg1,reg2self,reg1,reg2self)
    # links: two fit, the third is refused
    local l=$(srtla_raw 127.0.0.21 reg1,reg2,reg2,reg2)
    stop_sls
    if [ "$a" = "9201 9202 9201 9210" ] && [ "$l" = "9201 9202 9202 9210" ] && [ "$(asan_reports)" = 0 ]; then
        pass srtla_caps "groups per IP: $a; links per group: $l"
    else
        fail srtla_caps "groups per IP: '$a' (want 9201 9202 9201 9210); links: '$l' (want 9201 9202 9202 9210)"
    fi
}

case_reg_rate(){
    write_conf "srtla_reg_per_ip_per_sec 3;"; start_sls
    local r=$(srtla_raw 127.0.0.22 reg1,reg1,reg1,reg1,reg1,sleep1.1,reg1)
    stop_sls
    if [ "$r" = "9201 9201 9201 9210 9210 9201" ]; then
        pass srtla_reg_rate "3 per second: $r"
    else
        fail srtla_reg_rate "'$r' (want 9201 9201 9201 9210 9210 9201)"
    fi
}

case_forged_reg2(){
    write_conf; start_sls
    local r=$(srtla_raw 127.0.0.23 forged,reg1,forged)
    stop_sls
    if [ "$r" = "9211 9201 9211" ] && [ "$(counter srtla_groups)" -le 1 ]; then
        pass srtla_forged_reg2 "unknown ids get REG_NGP: $r"
    else
        fail srtla_forged_reg2 "'$r' (want 9211 9201 9211)"
    fi
}

case_player_through_group(){
    # a player behind a bonded sender is refused (publishers only), while the
    # stream exists (a plain publisher), so 0 bytes means refused
    write_conf; start_sls
    ( nice ffmpeg -nostdin -loglevel error -re -stream_loop -1 -i "$SRC_TS" -c copy -f mpegts - 2>/dev/null | \
      timeout 12 srt-live-transmit -q -chunk 1316 file://con \
        "srt://127.0.0.1:$PORT?streamid=uplive.m/live/m1&passphrase=$(key 2)&mode=caller" ) > /dev/null 2>&1 &
    local pp=$!
    nap 2
    $SENDER --server 127.0.0.1:$PORT --local $LOCAL --links 127.0.0.2,127.0.0.3 --duration 8 > sender.json 2>&1 &
    local sp=$!
    nap 1
    timeout 6 srt-live-transmit -q "srt://127.0.0.1:$LOCAL?streamid=live.m/live/m1&passphrase=$(key 3)&mode=caller" file://con > play.ts 2>/dev/null
    wait $sp
    kill_cmd srt-live-transm "127.0.0.1:$PORT?streamid=uplive.m"; kill_cmd ffmpeg "$SRC_TS"; wait $pp 2>/dev/null
    stop_sls
    local b=$(stat -c%s play.ts) ref=$(awk '/a player through an SRTLA group/' out.log | wc -l)
    local pubs=$(awk '/session start, publisher/' out.log | wc -l)
    if [ "$pubs" = 1 ] && [ "$b" = 0 ] && [ "$ref" -ge 1 ] && [ "$(awk '/session start, player/' out.log | wc -l)" = 0 ]; then
        pass srtla_player_through_group "stream live; player through a group refused ($ref lines), 0 bytes"
    else
        fail srtla_player_through_group "publishers $pubs, player got $b bytes, refusal lines $ref"
    fi
}

case_locked_out_ip(){
    # 127.0.0.24 fails the passphrase 6 times (plain SRT), then tries to register a group
    # (debug: the lockout line is a peer line, held back by the budget at info)
    LOG_LEVEL=debug write_conf; start_sls
    # one try each (-a no: srt-live-transmit would otherwise reconnect in a loop
    # and hit the handshake rate first, which also refuses a REG1)
    for i in 1 2 3 4 5 6; do   # SLS_AUTH_FAIL_MAX is 5
        timeout 3 srt-live-transmit -q -a no "srt://127.0.0.1:$PORT?streamid=uplive.m/live/m1&passphrase=wrongwrongwrong$i&mode=caller&adapter=127.0.0.24" file://con < /dev/null > /dev/null 2>&1
    done
    nap 1.5   # the handshake bucket refills: what refuses now is the lockout
    local blocked=$(awk '/wrong passphrases from 127\.0\.0\.24/' out.log | wc -l)
    local r=$(srtla_raw 127.0.0.24 reg1)
    local other=$(srtla_raw 127.0.0.25 reg1)
    stop_sls
    # the refusal must name the lockout (not the rate or a cap)
    local why=$(awk '/refused SRTLA REG1 from \[127\.0\.0\.24:/ && /locked out \(wrong passphrases\)/' out.log | wc -l)
    if [ "$blocked" -ge 1 ] && [ "$r" = "9210" ] && [ "$why" -ge 1 ] && [ "$other" = "9201" ]; then
        pass srtla_locked_out_ip "127.0.0.24 locked out -> REG1 answered $r; 127.0.0.25 -> $other"
    else
        fail srtla_locked_out_ip "lockout lines $blocked, locked REG1 '$r' (want 9210, reason lines $why), other '$other' (want 9201)"
    fi
}

case_keepalive_echo_cap(){
    # a link's keepalives are echoed up to SLS_SRTLA_KEEPALIVE_MAX (128) bytes only
    write_conf; start_sls
    local r=$(srtla_raw 127.0.0.26 reg1,reg2self,ka2,ka10,ka128,ka129,ka1400)
    stop_sls
    if [ "$r" = "9201 9202 2 10 128 none none" ] && [ "$(asan_reports)" = 0 ]; then
        pass srtla_keepalive_echo_cap "$r"
    else
        fail srtla_keepalive_echo_cap "'$r' (want 9201 9202 2 10 128 none none)"
    fi
}

case_refused_session_expires(){
    # a group whose SRT handshake libsrt refuses (a wrong passphrase) never
    # gets a session: it ends after 10 s although its links keep sending keepalives
    LOG_LEVEL=debug write_conf; start_sls
    PUB_PASS=wrongwrongwrong1 PUB_OPTS="-a no" bonded_publish 16 --links 127.0.0.2,127.0.0.3
    bonded_wait; stop_sls
    local ended=$(awk '/SRTLA group [0-9]+ ended/ && /reason=no session/' out.log | wc -l)
    local acc=$(awk '/session accepted by libsrt/' out.log | wc -l)
    if [ "$ended" -ge 1 ] && [ "$acc" = 0 ] && [ "$(asan_reports)" = 0 ]; then
        pass srtla_refused_session_expires "group ended with reason=no session ($ended), accepted sessions $acc"
    else
        fail srtla_refused_session_expires "ended lines $ended (want >=1), accepted $acc (want 0), asan=$(asan_reports)"
    fi
}

case_refused_reason_logged(){
    # at log_level info a bonded publisher that libsrt refuses (a passphrase
    # the user does not have) is named with libsrt's reason, its real address,
    # the streamid and the group, and a sender retrying in a loop stays within
    # the peer-line budget (one per second)
    LOG_LEVEL=info write_conf; start_sls
    PUB_PASS=wrongwrongwrong1 PUB_OPTS="-a no" bonded_publish 8 --links 127.0.0.2,127.0.0.3
    bonded_wait; stop_sls
    local line=$(awk '/ INFO: .*refused by libsrt: reason=/' out.log | head -1)
    local n=$(awk '/ INFO: .*refused by libsrt: reason=/' out.log | wc -l)
    local all=$(awk '/refused by libsrt: reason=/' out.log | wc -l)
    local lo=$(awk '/refused by libsrt/ && /\[127\.0\.0\.1:/' out.log | wc -l)
    if echo "$line" | grep -q -E "\[127\.0\.0\.[23]:[0-9]+\] refused by libsrt: reason=[0-9]+ \(.+\), streamid='uplive\.m/live/m1', SRTLA group [0-9]+\." \
       && [ "$n" -le 10 ] && [ "$lo" = 0 ] && [ "$(asan_reports)" = 0 ]; then
        pass srtla_refused_reason_logged "$n INFO line(s) for $all refusal(s): ${line#*CSLSFrontDoor, }"
    else
        fail srtla_refused_reason_logged "first='$line' info=$n (want 1-10) all=$all from 127.0.0.1=$lo asan=$(asan_reports)"
    fi
}

case_stock_srtla_send(){
    # BELABOX srtla_send as a black-box second sender (if built)
    if [ ! -x "$SRTLA_SEND" ]; then pass srtla_stock_srtla_send "skipped: no $SRTLA_SEND"; return; fi
    write_conf; start_sls
    printf '127.0.0.2\n127.0.0.3\n' > ips.txt
    "$SRTLA_SEND" $LOCAL 127.0.0.1 $PORT ips.txt > srtla_send.log 2>&1 &
    local sp=$!
    nap 1
    ( nice ffmpeg -nostdin -loglevel error -re -stream_loop -1 -i "$SRC_TS" -c copy -f mpegts - 2>/dev/null | \
      timeout 20 srt-live-transmit -q -chunk 1316 -s 1000 -pf csv -statsout pub.csv file://con \
        "srt://127.0.0.1:$LOCAL?streamid=uplive.m/live/m1&passphrase=$(key 2)&latency=2000&mode=caller" ) > /dev/null 2>&1 &
    local pp=$!
    nap 5; local b=$(play 12)
    wait $pp 2>/dev/null; kill_cmd ffmpeg "$SRC_TS"; kill $sp 2>/dev/null; wait $sp 2>/dev/null
    stop_sls
    local links=$(awk '/SRTLA group .* joined/' out.log | wc -l) r=$(pub_stat pktRetrans) s=$(pub_stat pktSent)
    if [ "$b" -ge 4000000 ] && [ "$links" -ge 2 ] && [ "$(asan_reports)" = 0 ]; then
        pass srtla_stock_srtla_send "links joined $links, publisher sent=$s retrans=$r, player $b B"
    else
        fail srtla_stock_srtla_send "links $links, sent=$s retrans=$r, player $b B, asan=$(asan_reports)"
    fi
}

ALL="reorder_clean stats reorder_off_compare link_dies dup_and_drop late_induction caps reg_rate forged_reg2 player_through_group locked_out_ip keepalive_echo_cap refused_session_expires stock_srtla_send"
for c in ${*:-$ALL}; do
    c=${c#srtla_}
    "case_$c"
done
echo "srtla.sh: $FAILS failed"
exit $((FAILS > 0))
