# A slow internet connection between flim and its server, for tests on one PC: a TCP relay that
# limits each connection's bandwidth (each direction) and adds latency. WebSocket and blob store
# (same port) both go through it.
#   python slowlink.py [--listen 8081] [--to 8080] [--kbit 1000] [--ms 60]
# then connect the FLs to localhost:8081 (restart_test.ps1 -Url localhost:8081).
import argparse, asyncio, time

ap = argparse.ArgumentParser()
ap.add_argument('--listen', type=int, default=8081)
ap.add_argument('--to', type=int, default=8080)
ap.add_argument('--kbit', type=float, default=1000, help='bandwidth per direction per connection, kbit/s')
ap.add_argument('--ms', type=float, default=60, help='one-way latency added, ms')
A = ap.parse_args()
RATE = A.kbit * 1000 / 8          # bytes/s
DELAY = A.ms / 1000

async def pipe(r, w):
    q = asyncio.Queue(maxsize=8)      # ~64 KB in flight: a sender feels the slow link, as on a real one
    async def read():
        while True:
            d = await r.read(8192)
            await q.put((time.monotonic() + DELAY, d))
            if not d: return
    async def write():
        while True:
            due, d = await q.get()
            if not d: break
            await asyncio.sleep(max(0, due - time.monotonic()))
            w.write(d)
            await w.drain()
            await asyncio.sleep(len(d) / RATE)       # the link's bandwidth
        w.close()
    try: await asyncio.gather(read(), write())
    except (ConnectionError, OSError): w.close()

async def client(cr, cw):
    try: sr, sw = await asyncio.open_connection('127.0.0.1', A.to)
    except OSError: cw.close(); return
    await asyncio.gather(pipe(cr, sw), pipe(sr, cw))

async def main():
    srv = await asyncio.start_server(client, '127.0.0.1', A.listen)
    print('slowlink :%d -> :%d, %g kbit/s each way, +%g ms' % (A.listen, A.to, A.kbit, A.ms), flush=True)
    async with srv: await srv.serve_forever()

asyncio.run(main())
