# The server limits (both servers: server/src/server.ts, client/launcher/host.c): a full session, the message
# rate, connections per address, wrong passwords per address. Needs a fresh server (the last step locks this
# address out for 10 minutes): python server_limits.py <host:port> <password> <max people>
import os, sys, socket, base64, json, struct, time

HOST, PORT = sys.argv[1].split(':')[0], int(sys.argv[1].split(':')[1])
PW, MAXP = sys.argv[2], int(sys.argv[3])
ok = fail = 0
def check(name, cond, detail=''):
    global ok, fail
    print(('OK   ' if cond else 'FAIL ') + name + ('' if cond else '  ' + str(detail))); ok += bool(cond); fail += not cond

class Ws:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=5)
        self.s.sendall(('GET / HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                        'Sec-WebSocket-Key: %s\r\n\r\n' % (HOST, PORT, base64.b64encode(os.urandom(16)).decode())).encode())
        r = b''
        while b'\r\n\r\n' not in r:
            k = self.s.recv(4096)
            if not k: raise EOFError('closed during the handshake')
            r += k
        self.buf, self.got, self.closed = r.split(b'\r\n\r\n', 1)[1], [], False
    def send(self, obj):
        d, m = json.dumps(obj, separators=(',', ':')).encode(), os.urandom(4)
        h = bytes([0x81, 0x80 | len(d)]) if len(d) < 126 else bytes([0x81, 0xfe]) + struct.pack('>H', len(d))
        try: self.s.sendall(h + m + bytes(c ^ m[i & 3] for i, c in enumerate(d)))
        except OSError: self.closed = True
    def join(self, uid, pw=PW, room='FLIM'): self.send({'t': 'join_room', 'room': room, 'password': pw, 'user': {'id': uid, 'name': uid}})
    def pump(self, secs=1.0):
        self.s.settimeout(0.2); t0 = time.time()
        while time.time() - t0 < secs and not self.closed:
            try:
                while len(self.buf) < 2:
                    k = self.s.recv(1 << 20)
                    if not k: self.closed = True; return
                    self.buf += k
                n, o = self.buf[1] & 127, 2
                if n == 126: n, o = (struct.unpack('>H', self.buf[2:4])[0] if len(self.buf) >= 4 else 1 << 30), 4
                elif n == 127: n, o = (struct.unpack('>Q', self.buf[2:10])[0] if len(self.buf) >= 10 else 1 << 30), 10
                while len(self.buf) < o + n:
                    k = self.s.recv(1 << 20)
                    if not k: self.closed = True; return
                    self.buf += k
                op, d = self.buf[0] & 15, self.buf[o:o + n]; self.buf = self.buf[o + n:]
                if op == 1: self.got.append(json.loads(d))
                elif op == 8: self.closed = True
            except socket.timeout: pass
            except OSError: self.closed = True
    def codes(self): return [m.get('code') for m in self.got if m.get('t') == 'error']
    def close(self):
        try: self.s.close()
        except OSError: pass

# a full session: MAXP people get in, the next one doesn't (a reconnect of someone inside still does)
people = []
for i in range(MAXP):
    w = Ws(); w.join('p%d' % i); w.pump(0.8); people.append(w)
check('%d people get in' % MAXP, all(not w.closed and 'full' not in w.codes() for w in people), [w.codes() for w in people])
x = Ws(); x.join('extra'); x.pump(1.5)
check('the next one is refused: full', 'full' in x.codes(), x.codes()); x.close()
r = Ws(); r.join('p1'); r.pump(1.0)
check('someone inside reconnecting still gets in', 'full' not in r.codes() and not r.closed, r.codes()); r.close()
for w in people: w.close()
time.sleep(1)

# the message rate: far more than flim sends, at once
w = Ws(); w.join('fast'); w.pump(0.8)
for i in range(6000):
    w.send({'t': 'op', 'op': {'kind': 'cur', 'x': i}})
    if w.closed: break
w.pump(2.0)
check('a flood of messages is cut off', w.closed or 'rate' in w.codes(), (w.closed, w.codes())); w.close()
w = Ws(); w.join('normal'); w.pump(0.5)
for i in range(200): w.send({'t': 'op', 'op': {'kind': 'cur', 'x': i}}); time.sleep(0.005)
w.pump(1.0)
check('normal traffic (cursors every 5 ms) is fine', not w.closed and 'rate' not in w.codes(), w.codes()); w.close()
time.sleep(1)

# connections per address: 12 open, the 13th refused
socks, refused = [], False
for i in range(13):
    try:
        s = Ws(); socks.append(s)
    except (OSError, EOFError): refused = True; break
if not refused:
    socks[-1].pump(1.0); refused = socks[-1].closed or 'too_many' in socks[-1].codes()
check('the 13th connection from one address is refused', refused)
for s in socks: s.close()
time.sleep(1)

# wrong passwords: 8, then even the right one is refused (this address, 10 minutes)
for i in range(8):
    w = Ws(); w.join('guess%d' % i, pw='wrong%d' % i); w.pump(0.4); w.close()
w = Ws(); w.join('late'); w.pump(1.5)
check('after 8 wrong passwords the right one is refused too', 'too_many_failures' in w.codes() or (w.closed and 'welcome' not in str(w.got)), w.codes()); w.close()

print('%d ok, %d failed' % (ok, fail))
