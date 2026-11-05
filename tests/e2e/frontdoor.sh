#!/bin/bash
# Front door (srtla on) e2e cases. ASAN sls by default; loopback only.
#   tests/e2e/frontdoor.sh [case ...]       (run from the repo root; default: all)
# Env: SLS_BIN (default bin-asan/sls), WORK (default ~/sls-1.6.0-tests/frontdoor),
#      PORT (public port, default 8090).
# Needs ffmpeg, srt-live-transmit, python3. Prints one PASS/FAIL line per case
# and exits non-zero if any case failed.

REPO=$(cd "$(dirname "$0")/../.." && pwd)
SLS_BIN=${SLS_BIN:-$REPO/bin-asan/sls}
WORK=${WORK:-$HOME/sls-1.6.0-tests/frontdoor}
PORT=${PORT:-8090}
FAILS=0
mkdir -p "$WORK/logs" "$WORK/asan"
cd "$WORK" || exit 1

pass(){ echo "PASS $1${2:+ ($2)}"; }
fail(){ echo "FAIL $1${2:+ ($2)}"; FAILS=$((FAILS+1)); }
nap(){ timeout "$1" tail -f /dev/null; true; }
rand(){ head -c 18 /dev/urandom | base64 | tr '+/' 'xy'; }

# users: s1 (publish + play keys)
if [ ! -s users.conf ]; then
    ( umask 077; printf 's1 %s %s\ns2 %s %s\n' $(rand) $(rand) $(rand) $(rand) > users.conf )
fi
key(){ awk -v n=$1 -v c=$2 '$1==n{print $c}' users.conf; }   # c=2 publish, 3 play

write_conf(){ # extra server lines...
    {
        printf 'srt {\n    worker_threads 1;\n    log_file logs/error.log;\n    log_level %s;\n' "${LOG_LEVEL:-debug}"
        printf '    pid_file %s/sls.pid;\n' "$WORK"
        [ -n "$STAT_URL" ] && printf '    stat_post_url %s;\n    stat_post_interval 1;\n' "$STAT_URL"
        printf '    server {\n        listen %s;\n        latency 120;\n' "$PORT"
        printf '        domain_publisher uplive.x;\n        domain_player live.x;\n'
        printf '        idle_streams_timeout 10;\n        user_file users.conf;\n        srtla on;\n'
        for l in "$@"; do printf '        %s\n' "$l"; done
        printf '        app {\n            app_publisher live;\n            app_player live;\n        }\n    }\n}\n'
    } > sls.conf
}

SLS_PID=
start_sls(){
    rm -f sls.pid out.log asan/* logs/error.log
    ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1:log_path=$WORK/asan/asan \
    UBSAN_OPTIONS=print_stacktrace=1:log_path=$WORK/asan/ubsan \
    setsid nice stdbuf -oL "$SLS_BIN" -c sls.conf > out.log 2>&1 < /dev/null &
    for i in $(seq 50); do
        [ -s sls.pid ] && awk '/libsrt_listen, ok/{f=1} END{exit !f}' out.log && break
        nap 0.2
    done
    SLS_PID=$(cat sls.pid 2>/dev/null)
    INTERNAL=$(awk '/front door: public port/{match($0,/127\.0\.0\.1:[0-9]+/); print substr($0,RSTART+10,RLENGTH-10); exit}' out.log)
}
stop_sls(){
    [ -n "$SLS_PID" ] || return
    kill -INT "$SLS_PID" 2>/dev/null
    for i in $(seq 100); do kill -0 "$SLS_PID" 2>/dev/null || break; nap 0.2; done
    if kill -0 "$SLS_PID" 2>/dev/null; then echo "  !! sls $SLS_PID did not exit"; fi
    SLS_PID=
}
asan_reports(){ cat asan/* 2>/dev/null | awk '/runtime error|ERROR: AddressSanitizer|ERROR: LeakSanitizer/' | wc -l; }
counter(){ # name -> value from the front door's stop line
    awk -v n="$1" '/front door stopped/{ if (match($0, n "=[0-9]+")) v=substr($0,RSTART+length(n)+1,RLENGTH-length(n)-1) } END{print v+0}' out.log
}

PUBPID=
publish(){ # stream secs [host]
    nice ffmpeg -nostdin -loglevel error -re -f lavfi -i testsrc=size=320x180:rate=15 -t "$2" \
        -c:v libx264 -preset ultrafast -g 15 -f mpegts \
        "srt://${3:-127.0.0.1}:$PORT?streamid=uplive.x/live/$1&passphrase=$(key $1 2)" > ffmpeg-$1.log 2>&1 &
    PUBPID=$!
}
unpublish(){ [ -n "$PUBPID" ] && { kill "$PUBPID" 2>/dev/null; wait "$PUBPID" 2>/dev/null; }; PUBPID=; }
play(){ # out url secs -> bytes
    timeout "$3" srt-live-transmit -q "$2" file://con > "$1" 2>/dev/null
    stat -c%s "$1" 2>/dev/null || echo 0
}
udp_send(){ # src_ip dst_port count len first_byte_hex [hex_prefix] -> prints replies received
    python3 -I - "$@" <<'EOF'
import os, socket, sys
src, dport, count, length, first = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
prefix = bytes.fromhex(sys.argv[6]) if len(sys.argv) > 6 else bytes.fromhex(first)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind((src, 0))
for _ in range(count):
    pkt = prefix + os.urandom(max(0, length - len(prefix)))
    s.sendto(pkt[:length], ("127.0.0.1", dport))
s.settimeout(1.0)
replies = 0
try:
    while True:
        s.recvfrom(2048)
        replies += 1
except socket.timeout:
    pass
print(replies)
EOF
}

# ---------------------------------------------------------------------------
case_frontdoor_publish_play(){
    write_conf; start_sls
    [ -n "$INTERNAL" ] || { fail frontdoor_publish_play "no front door line"; stop_sls; return; }
    publish s1 20; nap 4
    local b=$(play play.ts "srt://127.0.0.1:$PORT?streamid=live.x/live/s1&passphrase=$(key s1 3)" 5)
    local codecs=$(ffprobe -v error -show_entries stream=codec_name -of csv=p=0 play.ts 2>/dev/null | sort -u | tr '\n' ' ')
    unpublish; nap 1; stop_sls
    if [ "$b" -ge 100000 ] && [ -n "$codecs" ] && [ "$(asan_reports)" = 0 ]; then
        pass frontdoor_publish_play "$b bytes, $codecs, relayed_in=$(counter relayed_in) relayed_out=$(counter relayed_out)"
    else
        fail frontdoor_publish_play "$b bytes, codecs='$codecs', asan=$(asan_reports)"
    fi
}

case_frontdoor_unknown_data_dropped(){
    write_conf; start_sls
    publish s1 20; nap 3
    local replies=$(udp_send 127.0.0.5 "$PORT" 100 1316 00)
    nap 1
    local b=$(play play2.ts "srt://127.0.0.1:$PORT?streamid=live.x/live/s1&passphrase=$(key s1 3)" 4)
    unpublish; nap 1; stop_sls
    local d=$(counter dropped_unknown)
    if [ "$d" -ge 100 ] && [ "$replies" = 0 ] && [ "$b" -ge 50000 ] && [ "$(asan_reports)" = 0 ]; then
        pass frontdoor_unknown_data_dropped "dropped_unknown=$d, still served $b bytes"
    else
        fail frontdoor_unknown_data_dropped "dropped_unknown=$d replies=$replies play=$b asan=$(asan_reports)"
    fi
}

case_frontdoor_srtla_dropped(){
    write_conf; start_sls
    local replies=$(udp_send 127.0.0.6 "$PORT" 1 258 9200)
    nap 0.5; stop_sls
    local d=$(counter dropped_srtla)
    if [ "$d" -ge 1 ] && [ "$replies" = 0 ] && [ "$(asan_reports)" = 0 ]; then
        pass frontdoor_srtla_dropped "dropped_srtla=$d, no reply"
    else
        fail frontdoor_srtla_dropped "dropped_srtla=$d replies=$replies asan=$(asan_reports)"
    fi
}

# --- Task 5: real identity ---------------------------------------------------
publish_from(){ # stream secs src_ip   (ffmpeg -> srt-live-transmit bound to src_ip)
    ( nice ffmpeg -nostdin -loglevel error -re -f lavfi -i testsrc=size=320x180:rate=15 -t "$2" \
        -c:v libx264 -preset ultrafast -g 15 -f mpegts - 2>/dev/null \
      | srt-live-transmit -q file://con \
        "srt://127.0.0.1:$PORT?streamid=uplive.x/live/$1&passphrase=$(key $1 2)&mode=caller&adapter=$3" > /dev/null 2>&1 ) &
    PUBPID=$!
}
unpublish_tree(){ [ -n "$PUBPID" ] && { pkill -P "$PUBPID" 2>/dev/null; kill "$PUBPID" 2>/dev/null; wait "$PUBPID" 2>/dev/null; }; PUBPID=; }
SINK_PID=
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

case_frontdoor_real_ip_logged(){
    start_sink 18601
    STAT_URL=http://127.0.0.1:18601/sls/stat write_conf; start_sls
    publish_from s1 20 127.0.0.2; nap 4
    local b=$(play play3.ts "srt://127.0.0.1:$PORT?streamid=live.x/live/s1&passphrase=$(key s1 3)&mode=caller&adapter=127.0.0.4" 3)
    nap 1.5
    unpublish_tree; nap 1; stop_sls; stop_sink
    local pub=$(awk '/session start, publisher/{match($0,/remote=[^,]*/); print substr($0,RSTART+7,RLENGTH-7); exit}' out.log)
    local ply=$(awk '/session start, player/{match($0,/remote=[^,]*/); print substr($0,RSTART+7,RLENGTH-7); exit}' out.log)
    local stat_pub=$(awk '/"role":"publisher"|"role": "publisher"|publisher/' stats.jsonl 2>/dev/null | awk '/127\.0\.0\.2/' | wc -l)
    local stat_lo=$(awk '/remote_ip"[: ]*"127\.0\.0\.1"/' stats.jsonl 2>/dev/null | wc -l)
    if [ "${pub%%:*}" = 127.0.0.2 ] && [ "${ply%%:*}" = 127.0.0.4 ] && [ "$b" -ge 30000 ] \
       && [ "$stat_pub" -ge 1 ] && [ "$stat_lo" = 0 ] && [ "$(asan_reports)" = 0 ]; then
        pass frontdoor_real_ip_logged "publisher remote=$pub, player remote=$ply, stats lines with 127.0.0.2: $stat_pub, with 127.0.0.1: 0"
    else
        fail frontdoor_real_ip_logged "pub=$pub ply=$ply play=$b stats(127.0.0.2)=$stat_pub stats(127.0.0.1)=$stat_lo asan=$(asan_reports)"
    fi
}

case_frontdoor_lockout_real_ip(){
    write_conf; start_sls
    publish_from s1 30 127.0.0.2; nap 3
    # srt-live-transmit retries a refused connection in a loop: 4 s of a wrong key is well over 5 tries
    play bad.ts "srt://127.0.0.1:$PORT?streamid=live.x/live/s1&passphrase=wrongwrongwrong&mode=caller&adapter=127.0.0.5" 4 > /dev/null
    nap 0.5
    local again=$(play bad2.ts "srt://127.0.0.1:$PORT?streamid=live.x/live/s1&passphrase=$(key s1 3)&mode=caller&adapter=127.0.0.5" 3)
    local other=$(play ok.ts "srt://127.0.0.1:$PORT?streamid=live.x/live/s1&passphrase=$(key s1 3)&mode=caller&adapter=127.0.0.3" 4)
    unpublish_tree; nap 1; stop_sls
    local locked=$(awk '/too many wrong passphrases/ && /\[127\.0\.0\.5:/' out.log | wc -l)
    local locked_lo=$(awk '/too many wrong passphrases/ && /\[127\.0\.0\.1:/' out.log | wc -l)
    if [ "$locked" -ge 1 ] && [ "$locked_lo" = 0 ] && [ "$again" = 0 ] && [ "$other" -ge 30000 ] && [ "$(asan_reports)" = 0 ]; then
        pass frontdoor_lockout_real_ip "127.0.0.5 locked out ($locked lines; right key then refused), 127.0.0.3 played $other bytes"
    else
        fail frontdoor_lockout_real_ip "locked(.5)=$locked locked(.1)=$locked_lo right-key-after=$again other=$other asan=$(asan_reports)"
    fi
}

case_internal_port_direct(){
    write_conf; start_sls
    publish_from s1 20 127.0.0.2; nap 3
    local b=$(play direct.ts "srt://127.0.0.1:$INTERNAL?streamid=live.x/live/s1&passphrase=$(key s1 3)" 3)
    unpublish_tree; nap 1; stop_sls
    local refused=$(awk '/not through the front door/' out.log | wc -l)
    if [ -n "$INTERNAL" ] && [ "$b" = 0 ] && [ "$refused" -ge 1 ] && [ "$(asan_reports)" = 0 ]; then
        pass internal_port_direct "127.0.0.1:$INTERNAL refused ($refused lines)"
    else
        fail internal_port_direct "internal=$INTERNAL bytes=$b refused_lines=$refused asan=$(asan_reports)"
    fi
}

# --- publishers sending the largest SRT live payload (1456 bytes) -----------
case_large_payload_1456(){
    write_conf; start_sls
    # srt-live-transmit's default chunk is 1456: TS packets straddle SRT messages
    ( nice ffmpeg -nostdin -loglevel error -re -f lavfi -i testsrc=size=320x180:rate=15 -t 20 \
        -c:v libx264 -preset ultrafast -g 15 -f mpegts - 2>/dev/null \
      | srt-live-transmit -q -chunk 1456 file://con \
        "srt://127.0.0.1:$PORT?streamid=uplive.x/live/s1&passphrase=$(key s1 2)" > /dev/null 2>&1 ) &
    PUBPID=$!
    nap 4
    local b=$(play big.ts "srt://127.0.0.1:$PORT?streamid=live.x/live/s1&passphrase=$(key s1 3)" 5)
    unpublish_tree; nap 1; stop_sls
    local trunc=$(awk '/small dst buffer/' out.log | wc -l)
    local errs=$(ffprobe -v error -i big.ts -f null - 2>&1 | wc -l)
    local codecs=$(ffprobe -v error -show_entries stream=codec_name -of csv=p=0 big.ts 2>/dev/null | sort -u | tr '\n' ' ')
    if [ "$b" -ge 50000 ] && [ "$trunc" = 0 ] && [ -n "$codecs" ] && [ "$(asan_reports)" = 0 ]; then
        pass large_payload_1456 "$b bytes, $codecs, truncation warnings 0, ffprobe error lines $errs"
    else
        fail large_payload_1456 "$b bytes, codecs='$codecs', truncation warnings $trunc, ffprobe error lines $errs, asan=$(asan_reports)"
    fi
}

# --- sls stop: connected peers are told at once (the front door relays libsrt's shutdowns)
case_stop_notifies_peers(){
    write_conf; start_sls
    publish s1 60; nap 4
    local p0=$PUBPID
    kill -INT "$SLS_PID"; local t0=$(date +%s%N)
    for i in $(seq 100); do kill -0 "$p0" 2>/dev/null || break; nap 0.1; done
    local t1=$(date +%s%N)
    local alive=0; kill -0 "$p0" 2>/dev/null && alive=1
    unpublish
    for i in $(seq 100); do kill -0 "$SLS_PID" 2>/dev/null || break; nap 0.2; done
    SLS_PID=
    local ms=$(( (t1 - t0) / 1000000 ))
    if [ "$alive" = 0 ] && [ "$ms" -lt 2000 ] && [ "$(asan_reports)" = 0 ]; then
        pass stop_notifies_peers "publisher ended ${ms} ms after SIGINT"
    else
        fail stop_notifies_peers "publisher still running=$alive after ${ms} ms (needs < 2000), asan=$(asan_reports)"
    fi
}

CASES=${*:-$(declare -F | awk '{print $3}' | awk '/^case_/' | sed 's/^case_//')}
for c in $CASES; do
    "case_$c"
    [ -n "$KEEP_LOGS" ] && cp out.log "out-$c.log"
done
echo "frontdoor.sh: $FAILS failed"
[ "$FAILS" = 0 ]
