#!/usr/bin/env python3
"""A small SRTLA (srtla2) test sender, written from the protocol facts in the
1.6.0 spec. It sits between a local SRT caller and an SRTLA receiver:

  SRT caller --UDP--> 127.0.0.1:LOCAL (this) ==N links==> receiver host:port

and can misbehave on purpose: delay one link (reordering), duplicate or drop
data packets, or kill a link mid-stream.

  srtla_sender.py --server 127.0.0.1:8090 --local 9000 --links 127.0.0.2,127.0.0.3
      [--delay 1:40] [--dup-every 50] [--drop-every 0] [--kill 1:20]
      [--duration 30] [--stats out.json]

--delay L:MS   hold every data packet on link L for MS ms
--kill L:SEC   link L stops sending (and answering) after SEC seconds
--late-induction SEC  re-send the caller's last SRT induction on link 0 at SEC
               (a late duplicate over a slow link)
Data packets go round robin over the live links. Control packets go on link 0
(or the first live one). Prints a JSON summary at the end.
"""
import argparse, heapq, json, os, select, socket, struct, sys, time

T_KEEPALIVE, T_ACK, T_REG1, T_REG2, T_REG3 = 0x9000, 0x9100, 0x9200, 0x9201, 0x9202
T_REG_ERR, T_REG_NGP, T_REG_NAK = 0x9210, 0x9211, 0x9212
ID_LEN = 256

def ptype(b):
    return struct.unpack('!H', b[:2])[0] if len(b) >= 2 else -1

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', required=True)
    ap.add_argument('--local', type=int, required=True)
    ap.add_argument('--links', required=True)
    ap.add_argument('--delay', default='')
    ap.add_argument('--dup-every', type=int, default=0)
    ap.add_argument('--drop-every', type=int, default=0)
    ap.add_argument('--kill', default='')
    ap.add_argument('--duration', type=float, default=30)
    ap.add_argument('--stats', default='')
    ap.add_argument('--register-only', action='store_true')
    ap.add_argument('--late-induction', type=float, default=0)
    a = ap.parse_args()
    host, port = a.server.rsplit(':', 1)
    server = (host, int(port))
    delay = {}
    if a.delay:
        l, ms = a.delay.split(':'); delay[int(l)] = float(ms) / 1000
    kill = {}
    if a.kill:
        l, sec = a.kill.split(':'); kill[int(l)] = float(sec)

    links = []
    for ip in a.links.split(','):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((ip, 0))
        s.setblocking(False)
        links.append({'sock': s, 'ip': ip, 'sent': 0, 'acks': 0, 'acked_seqs': 0, 'ka_back': 0,
                      'srt_back': 0, 'alive': True})
    stats = {'reg': None, 'links': links, 'dups': 0, 'drops': 0, 'from_caller': 0, 'to_caller': 0,
             'errors': [], 'reg_replies': []}

    def recv_type(s, want, timeout=2.0):
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([s], [], [], max(0, end - time.time()))
            if not r:
                break
            b, _ = s.recvfrom(2048)
            t = ptype(b)
            stats['reg_replies'].append(hex(t))
            if t in want:
                return t, b
        return None, None

    # registration: REG1 on link 0, then REG2 on every link
    sid = os.urandom(ID_LEN)
    links[0]['sock'].sendto(struct.pack('!H', T_REG1) + sid, server)
    t, b = recv_type(links[0]['sock'], (T_REG2, T_REG_ERR, T_REG_NAK))
    if t != T_REG2 or len(b) != 2 + ID_LEN or b[2:2 + 128] != sid[:128]:
        stats['reg'] = 'REG1 failed: %s' % (hex(t) if t else 'no answer')
        print(json.dumps(summary(stats))); return 1
    full = b[2:]
    for i, l in enumerate(links):
        l['sock'].sendto(struct.pack('!H', T_REG2) + full, server)
        t, _ = recv_type(l['sock'], (T_REG3, T_REG_ERR, T_REG_NGP))
        l['reg'] = hex(t) if t else 'none'
    stats['reg'] = 'ok' if all(l['reg'] == hex(T_REG3) for l in links) else 'partial'
    if a.register_only:
        print(json.dumps(summary(stats))); return 0

    local = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    local.bind(('127.0.0.1', a.local))
    local.setblocking(False)
    caller = None
    t0 = time.time()
    end = t0 + a.duration
    next_ka = t0
    rr = 0
    ndata = 0
    pending = []   # (due, n, link index, bytes)
    last_ind = None
    li_done = False
    n = 0
    while time.time() < end:
        now = time.time()
        for i, l in enumerate(links):
            if l['alive'] and i in kill and now - t0 >= kill[i]:
                l['alive'] = False
        live = [i for i, l in enumerate(links) if l['alive']]
        if a.late_induction and not li_done and last_ind and now - t0 >= a.late_induction:
            links[0]['sock'].sendto(last_ind, server); li_done = True; stats['late_induction_sent'] = 1
        if now >= next_ka:
            for i in live:
                links[i]['sock'].sendto(struct.pack('!H', T_KEEPALIVE) + struct.pack('!Q', int(now * 1000)), server)
            next_ka = now + 1.0
        while pending and pending[0][0] <= now:
            _, _, i, b = heapq.heappop(pending)
            if links[i]['alive']:
                links[i]['sock'].sendto(b, server); links[i]['sent'] += 1
        timeout = 0.05
        if pending:
            timeout = max(0, min(timeout, pending[0][0] - now))
        socks = [local] + [links[i]['sock'] for i in live]
        r, _, _ = select.select(socks, [], [], timeout)
        for s in r:
            if s is local:
                for _ in range(256):
                    try:
                        b, addr = local.recvfrom(2048)
                    except BlockingIOError:
                        break
                    caller = addr
                    stats['from_caller'] += 1
                    if len(b) >= 44 and b[:2] == b'\x80\x00' and struct.unpack('!I', b[36:40])[0] == 1:
                        last_ind = b
                    if not live:
                        continue
                    if len(b) >= 16 and not (b[0] & 0x80):
                        ndata += 1
                        if a.drop_every and ndata % a.drop_every == 0:
                            stats['drops'] += 1; continue
                        i = live[rr % len(live)]; rr += 1
                        copies = [i]
                        if a.dup_every and ndata % a.dup_every == 0:
                            copies.append(live[rr % len(live)]); stats['dups'] += 1
                        for j in copies:
                            if j in delay:
                                n += 1; heapq.heappush(pending, (time.time() + delay[j], n, j, b))
                            else:
                                links[j]['sock'].sendto(b, server); links[j]['sent'] += 1
                    else:
                        links[live[0]]['sock'].sendto(b, server); links[live[0]]['sent'] += 1
            else:
                l = next(l for l in links if l['sock'] is s)
                for _ in range(256):
                    try:
                        b, _ = s.recvfrom(2048)
                    except BlockingIOError:
                        break
                    t = ptype(b)
                    if t == T_ACK and len(b) == 44:
                        l['acks'] += 1; l['acked_seqs'] += 10
                    elif t == T_KEEPALIVE:
                        l['ka_back'] += 1
                    elif t >= 0 and (t < 0x9000 or t > 0x92FF):
                        l['srt_back'] += 1
                        if caller:
                            local.sendto(b, caller); stats['to_caller'] += 1
                    else:
                        stats['reg_replies'].append(hex(t))
    print(json.dumps(summary(stats)))
    if a.stats:
        with open(a.stats, 'w') as f:
            json.dump(summary(stats), f)
    return 0

def summary(stats):
    out = dict(stats)
    out['links'] = [{k: v for k, v in l.items() if k != 'sock'} for l in stats['links']]
    out['reg_replies'] = stats['reg_replies'][-20:]
    return out

if __name__ == '__main__':
    sys.exit(main())
