# list path-like strings inside the plugin-data events of an .fst / .flp
import re, sys
from flp import events
BS = '\\'
def scan(path):
    d = open(path, 'rb').read()
    for e, v in events(d):
        if e < 192: continue
        hits = []
        for m in re.finditer(rb'(?:[ -~]\x00){6,}', v):
            s = m.group().decode('utf-16le')
            if BS in s or re.search(r'\.(wav|mp3|ogg|flac|aif|aiff)', s, re.I):
                hits.append(('W', m.start(), s, v[max(0, m.start() - 8):m.start()].hex()))
        for m in re.finditer(rb'[ -~]{6,}', v):
            s = m.group().decode()
            if BS in s or re.search(r'\.(wav|mp3|ogg|flac|aif|aiff)', s, re.I):
                hits.append(('A', m.start(), s, v[max(0, m.start() - 8):m.start()].hex()))
        if hits:
            print('event', e, 'len', len(v))
            for h in hits[:12]: print('  %s @%d %r prefix=%s' % (h[0], h[1], h[2][:110], h[3]))
for p in sys.argv[1:]:
    print('==', p); scan(p)
