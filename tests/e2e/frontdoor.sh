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

CASES=${*:-$(declare -F | awk '{print $3}' | awk '/^case_/' | sed 's/^case_//')}
for c in $CASES; do
    "case_$c"
    [ -n "$KEEP_LOGS" ] && cp out.log "out-$c.log"
done
echo "frontdoor.sh: $FAILS failed"
[ "$FAILS" = 0 ]
