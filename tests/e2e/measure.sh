#!/bin/bash
# Front door overhead: release sls, 1 publisher at ~6 Mbit/s (a pre-encoded TS
# replayed with -c copy) + 10 players on loopback, srtla off vs on.
#   tests/e2e/measure.sh [seconds]          (default 60)
# Env: SLS_BIN (default <repo>/bin/sls), WORK (default ~/sls-1.6.0-tests/measure).
# Reports sls %CPU (pidstat, average), RSS, and one player's SRT RTT (msRTT,
# from srt-live-transmit stats) per mode, and the pass verdict
# (CPU on <= 2x off, |RTT on - RTT off| < 1 ms).

REPO=$(cd "$(dirname "$0")/../.." && pwd)
SLS_BIN=${SLS_BIN:-$REPO/bin/sls}
WORK=${WORK:-$HOME/sls-1.6.0-tests/measure}
SECS=${1:-60}
mkdir -p "$WORK"; cd "$WORK" || exit 1
nap(){ timeout "$1" tail -f /dev/null; true; }
rand(){ head -c 18 /dev/urandom | base64 | tr '+/' 'xy'; }

[ -s users.conf ] || ( umask 077; printf 'm1 %s %s\n' $(rand) $(rand) > users.conf )
key(){ awk -v c=$1 '$1=="m1"{print $c}' users.conf; }
if [ ! -s src.ts ]; then
    echo "encoding src.ts (30 s, 720p30, 6 Mbit/s CBR) ..."
    nice -n 15 ffmpeg -nostdin -loglevel error -f lavfi -i testsrc2=size=1280x720:rate=30 -f lavfi -i sine=f=440 \
        -t 30 -c:v libx264 -preset veryfast -b:v 5800k -maxrate 6000k -minrate 5800k -bufsize 3000k -g 60 \
        -x264-params nal-hrd=cbr -c:a aac -b:a 128k -muxrate 6200k -f mpegts src.ts || exit 1
fi

run(){ # mode
    local mode=$1 d=$WORK/$1; rm -rf "$d"; mkdir -p "$d/logs"
    {
        printf 'srt {\n    worker_threads 1;\n    log_file logs/error.log;\n    log_level info;\n    pid_file %s/sls.pid;\n' "$d"
        printf '    server {\n        listen 8090;\n        latency 120;\n        domain_publisher uplive.m;\n        domain_player live.m;\n'
        printf '        user_file %s/users.conf;\n' "$WORK"
        [ "$mode" = on ] && printf '        srtla on;\n'
        printf '        app {\n            app_publisher live;\n            app_player live;\n        }\n    }\n}\n'
    } > "$d/sls.conf"
    ( cd "$d" && setsid "$SLS_BIN" -c sls.conf > out.log 2>&1 < /dev/null & )
    for i in $(seq 25); do [ -s "$d/sls.pid" ] && break; nap 0.2; done
    local pid=$(cat "$d/sls.pid")
    nice ffmpeg -nostdin -loglevel error -stream_loop -1 -re -i src.ts -c copy -f mpegts \
        "srt://127.0.0.1:8090?streamid=uplive.m/live/m1&passphrase=$(key 2)&latency=120000" > "$d/pub.log" 2>&1 &
    local pub=$!
    nap 3
    local players=""
    timeout $((SECS + 15)) srt-live-transmit -q -s 600 -pf csv -statsout "$d/stats.csv" \
        "srt://127.0.0.1:8090?streamid=live.m/live/m1&passphrase=$(key 3)" file://con > /dev/null 2>&1 &
    players="$players $!"
    for i in $(seq 9); do
        timeout $((SECS + 15)) srt-live-transmit -q \
            "srt://127.0.0.1:8090?streamid=live.m/live/m1&passphrase=$(key 3)" file://con > /dev/null 2>&1 &
        players="$players $!"
    done
    nap 5
    local cpu=$(LC_ALL=C pidstat -p "$pid" 1 "$SECS" | awk '/^Average/ && $0 !~ /%CPU/ {print $(NF-2)}')
    local rss=$(awk '/^VmRSS/{print $2}' /proc/$pid/status)
    kill $players 2>/dev/null; wait $players 2>/dev/null
    kill $pub 2>/dev/null; wait $pub 2>/dev/null
    kill -INT "$pid"; for i in $(seq 50); do kill -0 "$pid" 2>/dev/null || break; nap 0.2; done
    local nplay=$(awk '/session start, player/' "$d/out.log" | wc -l)   # stdout is flushed at exit
    # srt-live-transmit csv: find the msRTT and mbpsRecvRate columns by name
    local rtt=$(awk -F, 'NR==1{for(i=1;i<=NF;i++){if($i=="msRTT")r=i; if($i=="mbpsRecvRate")b=i}; next} r && $r+0>0 {s+=$r; n++; if (b) m+=$b} END{if(n) printf "%.3f %.2f %d", s/n, m/n, n; else print "nan nan 0"}' "$d/stats.csv")
    echo "$mode cpu=$cpu rss_kb=$rss players=$nplay rtt_ms,mbps,samples=$rtt"
}

R1=$(run off); echo "$R1"
R2=$(run on);  echo "$R2"
c1=$(echo "$R1" | sed 's/.*cpu=\([0-9.]*\).*/\1/'); c2=$(echo "$R2" | sed 's/.*cpu=\([0-9.]*\).*/\1/')
t1=$(echo "$R1" | sed 's/.*samples=\([0-9.]*\).*/\1/'); t2=$(echo "$R2" | sed 's/.*samples=\([0-9.]*\).*/\1/')
# no verdict from a run that did not measure: every player connected, RTT samples in both
for R in "$R1" "$R2"; do
    np=$(echo "$R" | sed 's/.*players=\([0-9]*\).*/\1/'); ns=$(echo "$R" | awk '{print $NF}')
    case "$R" in *samples=nan*) ns=0;; esac
    if [ "$np" != 10 ] || ! [ "${ns:-0}" -gt 0 ] 2>/dev/null; then
        echo "INVALID RUN ($R): need 10 players and RTT samples in both modes"; exit 2
    fi
done
awk -v c1="$c1" -v c2="$c2" -v t1="$t1" -v t2="$t2" 'BEGIN{
    ratio = (c1 > 0) ? c2 / c1 : 0; d = t2 - t1; if (d < 0) d = -d;
    ok = (c2 <= 2 * c1 && d < 1.0);
    printf "cpu off=%.2f%% on=%.2f%% ratio=%.2f; rtt off=%.3f ms on=%.3f ms diff=%.3f ms -> %s\n", c1, c2, ratio, t1, t2, d, ok ? "PASS" : "OVER THRESHOLD";
    exit !ok }'
