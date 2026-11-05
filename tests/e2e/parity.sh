#!/bin/bash
# Parity harness: the 1.5.5 relay/auth cases (from ~/sls-1.5.5-tests/run.sh),
# run with srtla absent ("off") and with `srtla on;` in every server block
# ("on"), plus front door cases. ASAN sls by default; loopback ports 8090-8093.
#   tests/e2e/parity.sh [off|on|both]        (run from anywhere; default both)
# Env: SLS_BIN (default <repo>/bin-asan/sls), WORK (default ~/sls-1.6.0-tests/parity).
# Servers: X 8090 (user_file, push mode all -> A 8091 + B 8092), A/B open,
#          P 8093 pulls from A. Prints PASS/FAIL per check; exit 1 on any FAIL.

REPO=$(cd "$(dirname "$0")/../.." && pwd)
SLS_BIN=${SLS_BIN:-$REPO/bin-asan/sls}
WORK_BASE=${WORK:-$HOME/sls-1.6.0-tests/parity}
MODES=${1:-both}; [ "$MODES" = both ] && MODES="off on"
TOTAL_FAILS=0

nap(){ timeout "$1" tail -f /dev/null; true; }
ts(){ date +%T.%3N; }
rand(){ head -c 18 /dev/urandom | base64 | tr '+/' 'xy'; }

run_mode(){
MODE=$1
T=$WORK_BASE/$MODE; rm -rf "$T"; mkdir -p "$T"; cd "$T" || return 1
R=$T/results; mkdir -p "$R"
FAILS=0
say(){ echo "[$(ts)] [$MODE] $*" | tee -a "$R/summary.txt"; }
check(){ # name cond-result(0/1) detail
    if [ "$2" = 0 ]; then say "PASS $1 — $3"; else say "FAIL $1 — $3"; FAILS=$((FAILS+1)); fi
}
SRTLA_LINE=""; [ "$MODE" = on ] && SRTLA_LINE="srtla on;"

( umask 077; for s in s1 s2 s3 s4 s5; do printf '%s %s %s\n' $s $(rand) $(rand); done > users.conf )
key(){ awk -v n=$1 -v c=$2 '$1==n{print $c}' "$T/users.conf"; }   # c=2 publish, 3 play

srv(){ # name port domain "extra server lines" "app lines"
    mkdir -p $1/logs $1/asan
    {
        printf 'srt {\n    worker_threads 1;\n    log_file logs/error.log;\n    log_level debug;\n'
        printf '    pid_file %s/%s/sls.pid;\n    server {\n        listen %s;\n        latency 120;\n' "$T" "$1" "$2"
        printf '        domain_publisher uplive.%s;\n        domain_player live.%s;\n        idle_streams_timeout 10;\n' "$3" "$3"
        [ -n "$SRTLA_LINE" ] && printf '        %s\n' "$SRTLA_LINE"
        [ -n "$4" ] && printf '%s\n' "$4"
        printf '        app {\n            app_publisher live;\n            app_player live;\n'
        [ -n "$5" ] && printf '%s\n' "$5"
        printf '        }\n    }\n}\n'
    } > $1/sls.conf
}
srv X 8090 x "        user_file $T/users.conf;" "            relay {
                type push;
                mode all;
                reconnect_interval 2;
                idle_streams_timeout 10;
                max_push_streams 16;
                upstreams 127.0.0.1:8091?streamid=uplive.a/live 127.0.0.1:8092?streamid=uplive.b/live;
            }"
srv A 8091 a "" ""
srv B 8092 b "" ""
srv P 8093 p "" "            relay {
                type pull;
                mode loop;
                reconnect_interval 2;
                idle_streams_timeout 5;
                max_pull_streams 4;
                upstreams 127.0.0.1:8091?streamid=live.a/live;
            }"

start(){ # name
  START_SEQ=$((${START_SEQ:-0} + 1))
  [ -f "$T/$1/out.log" ] && mv "$T/$1/out.log" "$T/$1/out-$(printf %02d $START_SEQ).log"
  ( cd "$T/$1" && rm -f asan/* logs/error.log out.log sls.pid
    ASAN_OPTIONS=verify_asan_link_order=0:detect_leaks=1:log_path=$T/$1/asan/asan \
    UBSAN_OPTIONS=print_stacktrace=1:log_path=$T/$1/asan/ubsan \
    setsid nice stdbuf -oL "$SLS_BIN" -c sls.conf > out.log 2>&1 < /dev/null & )
  for i in $(seq 50); do [ -s "$T/$1/sls.pid" ] && awk '/libsrt_listen, ok/{f=1} END{exit !f}' "$T/$1/out.log" 2>/dev/null && break; nap 0.2; done
  say "started $1 pid=$(cat "$T/$1/sls.pid" 2>/dev/null)"
}
STOPPED_ASAN=0
stop(){ # name
  local p=$(cat "$T/$1/sls.pid" 2>/dev/null); [ -n "$p" ] || return
  kill -INT $p; for i in $(seq 100); do kill -0 $p 2>/dev/null || break; nap 0.2; done
  if kill -0 $p 2>/dev/null; then check "stop_$1" 1 "pid $p did not exit in 20 s"; return; fi
  local a=$(cat "$T/$1"/asan/* 2>/dev/null | awk '/runtime error|ERROR: AddressSanitizer|ERROR: LeakSanitizer/' | wc -l)
  STOPPED_ASAN=$((STOPPED_ASAN + a))
  rm -f "$T/$1/sls.pid"
  say "stopped $1 pid=$p, asan reports: $a"
}
PUBPID=
publish(){ # stream secs [port]
  nice ffmpeg -nostdin -loglevel error -re -f lavfi -i testsrc=size=320x180:rate=15 -t $2 \
    -c:v libx264 -preset ultrafast -g 15 -f mpegts \
    "srt://127.0.0.1:${3:-8090}?streamid=uplive.x/live/$1&passphrase=$(key $1 2)" >> $R/ffmpeg-$1.log 2>&1 &
  PUBPID=$!; say "publish $1 (ffmpeg pid $PUBPID)"
}
unpublish(){ [ -n "$PUBPID" ] && { kill $PUBPID 2>/dev/null; wait $PUBPID 2>/dev/null; }; PUBPID=; }
play(){ local f=$R/play-$1.ts; timeout $3 srt-live-transmit -q "$2" file://con > $f 2>/dev/null; stat -c%s $f 2>/dev/null || echo 0; }
waitlog(){ for i in $(seq $(( $3 * 5 ))); do awk -v p="$2" '$0 ~ p{f=1} END{exit !f}' "$T/$1/out.log" && return 0; nap 0.2; done; return 1; }
cnt(){ awk -v p="$2" '$0 ~ p{n++} END{print n+0}' "$T/$1/out.log"; }
rb(){ cnt X "reconnect_all, ok, connect url='srt://127.0.0.1:8092"; }
lastline(){ awk -v p="$2" '$0 ~ p{l=NR} END{print l+0}' "$T/$1/out.log"; }
ge(){ [ "$1" -ge "$2" ] 2>/dev/null; echo $?; }
eq(){ [ "$1" = "$2" ]; echo $?; }

say "=== mode $MODE, binary $SLS_BIN"
say "=== (a) PUSH RETRY, main: B down at start"
start X; start A; start P
publish s1 300; nap 5
b=$(play a-s1 'srt://127.0.0.1:8091?streamid=live.a/live/s1' 4); check a_A_receives $(ge $b 50000) "A play s1 $b bytes"
nap 4; start B
waitlog B 'session start, publisher.*s1' 20; check a_B_starts_after_retry $? "B got publisher s1"
b=$(play b-s1 'srt://127.0.0.1:8092?streamid=live.b/live/s1' 4); check a_B_receives $(ge $b 50000) "B play s1 $b bytes"
n=$(cnt X 'session start, publisher.*s1'); check a_no_republish $(eq $n 1) "X publisher s1 sessions $n"

say "=== (b) PULL REASON"
b=$(play p-s1 'srt://127.0.0.1:8093?streamid=live.p/live/s1' 4); check b_pull_plays $(ge $b 50000) "P play s1 $b bytes"
waitlog P "session end, puller.*idle timeout .no players" 20; check b_pull_reason $? "$(awk '/session end, puller/' $T/P/out.log | tail -1 | sed 's/.*reason=/reason=/')"

say "=== (a) retire after the publisher stops"
unpublish; nap 3; n1=$(rb); nap 12; n2=$(rb)
check a_retire_no_endless_retries $(eq $n1 $n2) "B retries +3s=$n1 +15s=$n2"
n=$(cnt X 'release, last reference gone.*s1'); check a_retire_manager_deleted $(eq $n 1) "release lines $n"

say "=== (a) variant: B down, stop + re-publish s2 while A's pusher is live, then B up"
stop B
publish s2 300; nap 5
b=$(play a-s2 'srt://127.0.0.1:8091?streamid=live.a/live/s2' 3); check a2_A_receives $(ge $b 30000) "A play s2 $b bytes"
unpublish; nap 1; publish s2 300; nap 2; m1=$(rb); nap 10; m2=$(rb)
n=$(cnt X 'carries on for the new publisher.*s2'); check a2_carried_on $(ge $n 1) "'carries on' lines $n"
check a2_B_retried_after_republish $([ "$m2" -gt "$m1" ]; echo $?) "B retries +2s=$m1 +12s=$m2"
start B
waitlog B 'session start, publisher.*s2' 20; check a2_B_gets_s2 $? "B got s2 after re-publish"
b=$(play b-s2 'srt://127.0.0.1:8092?streamid=live.b/live/s2' 3); check a2_B_receives $(ge $b 30000) "B play s2 $b bytes"
unpublish; nap 15

say "=== (a) publisher gone for good, B down"
stop B
publish s4 300; nap 6; unpublish
if waitlog X 'retire, publisher gone.*stream_name=s4' 30; then
  L=$(lastline X 'retire, publisher gone.*stream_name=s4'); nap 12
  n=$(awk -v L=$L 'NR>L && /reconnect_all/' $T/X/out.log | wc -l); check a3_silent_after_retire $(eq $n 0) "reconnect lines after retire: $n"
else check a3_retire 1 "no 'retire, publisher gone' for s4 in 30 s"; fi
n=$(awk '/retire, not retired/' $T/X/out.log | wc -l); check a_no_retire_flood $([ "$n" -le 10 ]; echo $?) "'not retired' lines in total: $n"
start B

say "=== (c) REGRESSION on X"
publish s3 300; nap 4
b=$(play c-ok "srt://127.0.0.1:8090?streamid=live.x/live/s3&passphrase=$(key s3 3)" 4); check c_right_key $(ge $b 50000) "$b bytes"
b=$(play c-bad "srt://127.0.0.1:8090?streamid=live.x/live/s3&passphrase=wrongwrongwrong&mode=caller&adapter=127.0.0.2" 4)
n=$(cnt X 'BADSECRET|too many wrong passphrases'); check c_wrong_key_refused $([ "$b" = 0 ] && [ "$n" -ge 1 ]; echo $?) "$b bytes, refusal lines $n"
unpublish; nap 2; publish s3 300; nap 4
n=$(cnt X 'session start, publisher.*s3'); check c_republish_same_name $(eq $n 2) "publisher s3 sessions $n"
b=$(play c-again "srt://127.0.0.1:8090?streamid=live.x/live/s3&passphrase=$(key s3 3)" 4); check c_play_after_republish $(ge $b 50000) "$b bytes"
if [ "$MODE" = on ]; then
  pub=$(awk '/session start, publisher.*s3/{match($0,/remote=[^:]*/); print substr($0,RSTART+7,RLENGTH-7)}' $T/X/out.log | sort -u | tr '\n' ' ')
  n=$(awk '/front door: public port 8090, internal 127\.0\.0\.1:/' $T/X/out.log | wc -l)
  check c_front_door_active $(eq $n 1) "X front door line $n; publisher remotes: $pub"
fi
unpublish; nap 3

say "=== asan_stop_with_peers: SIGINT with 1 publisher + 3 players connected"
publish s5 300; nap 4
STOP_PLAYERS=""
for i in 1 2 3; do
  timeout 30 srt-live-transmit -q "srt://127.0.0.1:8090?streamid=live.x/live/s5&passphrase=$(key s5 3)" file://con > $R/stopplay-$i.ts 2>/dev/null &
  STOP_PLAYERS="$STOP_PLAYERS $!"
done
nap 4
n=$(awk '/session start, player.*s5/' $T/X/out.log | wc -l); check stop_players_connected $(eq $n 3) "players on s5: $n"
STOPPED_ASAN=0; stop X; check asan_stop_with_peers $(eq $STOPPED_ASAN 0) "X asan reports at stop: $STOPPED_ASAN, last line: $(tail -1 $T/X/out.log | cut -c25-60)"
unpublish
# the players would retry against the next cases for the rest of their 30 s
kill $STOP_PLAYERS 2>/dev/null; wait $STOP_PLAYERS 2>/dev/null

say "=== stop all"
STOPPED_ASAN=0; for n in P B A; do stop $n; done
check asan_clean_all $(eq $STOPPED_ASAN 0) "A/B/P asan reports $STOPPED_ASAN"
for n in X A B P; do
  e=$(cnt $n ' ERROR: '); w=$(awk '/ WARNING: / && !/has NO authentication/' $T/$n/out.log | wc -l)
  say "$n: ERROR=$e WARNING(excl. no-auth)=$w"
done

if [ "$MODE" = on ]; then
  say "=== spoof_flood: 10k handshakes from 1000 spoofed 127.0.x.y sources at X"
  # RSS is measured on the release build (ASAN's allocator quarantine keeps
  # freed memory resident); the ASAN build runs the same flood for errors
  for FLOOD_BIN in ${SLS_REL_BIN:-$REPO/bin/sls} $SLS_BIN; do
  SLS_BIN_SAVED=$SLS_BIN; SLS_BIN=$FLOOD_BIN
  start X; XP=$(cat $T/X/sls.pid)
  SLS_BIN=$SLS_BIN_SAVED
  XI=$(awk '/front door: public port 8090/{match($0,/127\.0\.0\.1:[0-9]+/); print substr($0,RSTART+10,RLENGTH-10); exit}' $T/X/out.log)
  maps(){ ss -uan | awk -v d="127.0.0.1:$XI" '$5==d{n++} END{print n+0}'; }   # sls is non-dumpable: no /proc/<pid>/fd
  say "flood against $FLOOD_BIN"
  rss0=$(awk '/^VmRSS/{print $2}' /proc/$XP/status); fd0=$(maps)
  python3 -I - <<'EOF' > $R/flood.txt
import socket, os
socks = []
for i in range(1000):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.%d.%d" % (10 + i // 250, 1 + i % 250), 0))
    socks.append(s)
hs = bytes.fromhex("8000") + os.urandom(62)
for r in range(10):
    for s in socks:
        s.sendto(hs, ("127.0.0.1", 8090))
print("sent", 10 * len(socks))
EOF
  nap 1
  rss1=$(awk '/^VmRSS/{print $2}' /proc/$XP/status); fd1=$(maps)
  nap 7.5   # half-open mappings: 5 s; quarantined sockets: 6 s; + the 1 s expiry tick
  rss2=$(awk '/^VmRSS/{print $2}' /proc/$XP/status); fd2=$(maps)
  publish s1 20; nap 4
  b=$(play flood-after "srt://127.0.0.1:8090?streamid=live.x/live/s1&passphrase=$(key s1 3)" 4)
  unpublish; nap 1
  STOPPED_ASAN=0; stop X
  sl=$(awk '/front door stopped/' $T/X/out.log | sed 's/.*front door stopped: //')
  # the cap is 300 mappings plus at most 300 quarantined (evicted) sockets
  check spoof_flood_capped $([ $((fd1 - fd0)) -le 610 ]; echo $?) "relay sockets $fd0 -> $fd1 (cap 300 + quarantine 300)"
  check spoof_flood_drained $([ $((fd2 - fd0)) -le 4 ]; echo $?) "relay sockets 8.5 s after the flood: $fd2"
  check spoof_flood_still_serves $(ge $b 50000) "play after the flood $b bytes; $sl"
  if [ "$FLOOD_BIN" = "$SLS_BIN" ]; then
    check spoof_flood_asan $(eq $STOPPED_ASAN 0) "asan $STOPPED_ASAN"
  else
    check spoof_flood_rss $([ $((rss1 - rss0)) -lt 5120 ] && [ $((rss2 - rss0)) -lt 5120 ]; echo $?) "release RSS kB $rss0 -> $rss1 -> $rss2"
  fi
  done

  say "=== two_blocks: 8090 srtla on + 8091 off in one sls"
  mkdir -p TB/logs TB/asan
  {
    printf 'srt {\n    worker_threads 1;\n    log_file logs/error.log;\n    log_level debug;\n    pid_file %s/TB/sls.pid;\n' "$T"
    for p in 8090 8091; do
      printf '    server {\n        listen %s;\n        latency 120;\n        domain_publisher uplive.x;\n        domain_player live.x;\n' $p
      printf '        user_file %s/users.conf;\n' "$T"
      [ $p = 8090 ] && printf '        srtla on;\n'
      printf '        app {\n            app_publisher live;\n            app_player live;\n        }\n    }\n'
    done
    printf '}\n'
  } > TB/sls.conf
  start TB
  ss -uln | awk '$4 ~ /:(8090|8091)$/ || $4 ~ /^127\.0\.0\.1:/ {print "   bound " $4}' | tee -a $R/summary.txt
  publish s1 20 8090; P1=$PUBPID; publish s2 20 8091; P2=$PUBPID; nap 4
  b1=$(play tb-1 "srt://127.0.0.1:8090?streamid=live.x/live/s1&passphrase=$(key s1 3)" 4)
  b2=$(play tb-2 "srt://127.0.0.1:8091?streamid=live.x/live/s2&passphrase=$(key s2 3)" 4)
  x1=$(play tb-x "srt://127.0.0.1:8091?streamid=live.x/live/s1&passphrase=$(key s1 3)" 3)
  kill $P1 $P2 2>/dev/null; wait $P1 $P2 2>/dev/null; nap 1
  n=$(awk '/front door: public port/' $T/TB/out.log | wc -l)
  STOPPED_ASAN=0; stop TB
  check two_blocks_plays $([ "$b1" -ge 50000 ] && [ "$b2" -ge 50000 ]; echo $?) "8090 (on) $b1 bytes, 8091 (off) $b2 bytes"
  check two_blocks_separate $([ "$n" = 1 ] && [ "$x1" = 0 ]; echo $?) "front door lines $n; s1 (8090's) not playable on 8091: $x1 bytes"
  check two_blocks_asan $(eq $STOPPED_ASAN 0) "asan $STOPPED_ASAN"
fi

say "=== mode $MODE: $FAILS failed"
TOTAL_FAILS=$((TOTAL_FAILS + FAILS))
}

for m in $MODES; do run_mode $m; done
echo "parity.sh: $TOTAL_FAILS failed"
[ "$TOTAL_FAILS" = 0 ]
