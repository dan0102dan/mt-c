#!/usr/bin/env python3
"""Host-only A/B DNS journal transport load. No iptables/routing changes.
Run only in an isolated test environment: the daemon uses its normal Unix path.
Usage: dns_journal_load.py --binary label=/path/to/magitrickled-c ... --out results.jsonl
The label 'base' has no journal; other binaries implement explicit capture.
Netfilter is disabled, groups are empty. NOT a router/full-routing benchmark.
"""
import argparse, asyncio, contextlib, json, os, signal, socket, statistics, struct
import subprocess, tempfile, time
from pathlib import Path

class Upstream(asyncio.DatagramProtocol):
    def connection_made(self, transport): self.transport = transport
    def datagram_received(self, data, peer):
        if len(data) >= 12:
            reply = bytearray(data)
            reply[2:4] = b'\x81\x80'
            self.transport.sendto(reply, peer)

class Probe(asyncio.DatagramProtocol):
    def __init__(self): self.pending = {}; self.latencies = []; self.sent = self.received = 0; self.sequence = 0
    def connection_made(self, transport): self.transport = transport
    def datagram_received(self, data, peer):
        key = int.from_bytes(data[:2], 'big')
        sent = self.pending.pop(key, None)
        if sent is not None:
            self.received += 1
            self.latencies.append((time.perf_counter_ns() - sent) / 1000)
    def send(self, long_name):
        self.sequence += 1; key = self.sequence & 65535
        if key in self.pending: raise RuntimeError('load exceeded 65535 outstanding IDs')
        token = f'{self.sequence:08x}' if long_name else f'{self.sequence % 200:08x}'
        labels = [token + 'x' * 55, 'y' * 63, 'z' * 63, 'w' * 61] if long_name else [token, 'example', 'org']
        wire = b''.join(bytes([len(s)]) + s.encode() for s in labels) + b'\0'
        packet = struct.pack('!6H', key, 0x100, 1, 0, 0, 0) + wire + b'\0\1\0\1'
        self.pending[key] = time.perf_counter_ns(); self.sent += 1
        self.transport.sendto(packet)
    def reset(self): self.pending.clear(); self.latencies.clear(); self.sent = self.received = 0

async def http(port, method, path, body=''):
    reader, writer = await asyncio.open_connection('127.0.0.1', port)
    writer.write(f'{method} {path} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n{body}'.encode())
    await writer.drain()
    data = await asyncio.wait_for(reader.read(), 5)
    writer.close(); await writer.wait_closed()
    status = int(data.split(b' ', 2)[1])
    if status != 200: raise RuntimeError(f'{method} {path}: {data[:300]!r}')
    return json.loads(data.split(b'\r\n\r\n', 1)[1])

async def stream(port, counts, slow=False):
    reader, writer = await asyncio.open_connection('127.0.0.1', port, limit=65536)
    writer.write(b'GET /api/v1/system/dns/stream HTTP/1.1\r\nHost: localhost\r\n\r\n'); await writer.drain()
    head = await reader.readuntil(b'\r\n\r\n')
    if not head.startswith(b'HTTP/1.1 200'): raise RuntimeError(head)
    counts['connected'] += 1
    try:
        if slow:
            # Stop consuming beyond the HTTP headers; StreamReader itself is bounded.
            await asyncio.sleep(3600)
        else:
            while True:
                line = await reader.readline()
                if not line: break
                n = int(line.strip().split(b';')[0], 16)
                if not n: break
                body = await reader.readexactly(n); await reader.readexactly(2)
                counts['bytes'] += n
                counts['queries'] += body.count(b'event: query\n')
                counts['snapshots'] += body.count(b'event: snapshot_begin\n')
    finally:
        writer.close()
        with contextlib.suppress(Exception): await writer.wait_closed()

async def generate(probe, rate, duration, long_name):
    start = time.monotonic(); batch = max(1, rate // 200); scheduled = 0
    while True:
        target = start + scheduled / rate
        if target >= start + duration: break
        await asyncio.sleep(max(0, target - time.monotonic()))
        for _ in range(batch): probe.send(long_name)
        scheduled += batch


def proc_stats(pid):
    text = Path(f'/proc/{pid}/stat').read_text().split(') ', 1)[1].split()
    fields = dict(line.split(':', 1) for line in Path(f'/proc/{pid}/status').read_text().splitlines() if ':' in line)
    return {'cpu': (int(text[11]) + int(text[12])) / os.sysconf('SC_CLK_TCK'),
            'rss_kib': int(fields['VmRSS'].split()[0]), 'fds': len(list(Path(f'/proc/{pid}/fd').iterdir()))}

async def run_one(binary, label, viewers, long_name, enabled, slow, args, rep):
    if Path('/var/run/magitrickle.sock').exists(): raise RuntimeError('another daemon Unix socket exists; refusing to interfere')
    with tempfile.TemporaryDirectory(prefix='mt-dns-ab-') as directory:
        cfg = Path(directory)/'config.yaml'
        cfg.write_text(f'''configVersion: 0.8.5
app:
  logLevel: error
  httpWeb:
    enabled: true
    auth: {{enabled: false}}
    host: {{address: 127.0.0.1, port: {args.http_port}}}
  dnsProxy:
    host: {{address: 127.0.0.1, port: {args.proxy_port}}}
    upstream: {{address: 127.0.0.1, port: {args.upstream_port}}}
    disableRemap53: true
    maxConcurrent: 1000
    maxIdleConns: 64
    timeout: 2s
  netfilter: {{disableIPv4: true, disableIPv6: true}}
  link: []
groups: []
subscriptions: []
''')
        log = open(Path(directory)/'daemon.log', 'w+')
        process = subprocess.Popen([binary, '--config', str(cfg)], stdout=log, stderr=subprocess.STDOUT)
        tasks=[]; transport=None
        try:
            for _ in range(100):
                if process.poll() is not None:
                    log.seek(0); raise RuntimeError(log.read())
                try:
                    await http(args.http_port, 'GET', '/api/v1/auth'); break
                except (OSError, asyncio.TimeoutError): await asyncio.sleep(.02)
            else: raise RuntimeError('daemon not ready')
            if label != 'base' and enabled:
                await http(args.http_port, 'PUT', '/api/v1/system/dns/capture', '{"enabled":true}')
            transport, probe = await asyncio.get_running_loop().create_datagram_endpoint(Probe, remote_addr=('127.0.0.1',args.proxy_port))
            # Fill the production memory-bounded ring before measuring steady churn.
            await generate(probe, 5000, 2, long_name); await asyncio.sleep(.1); probe.reset()
            counts={'connected':0,'queries':0,'bytes':0,'snapshots':0}
            for _ in range(viewers): tasks.append(asyncio.create_task(stream(args.http_port, counts)))
            if slow: tasks.append(asyncio.create_task(stream(args.http_port, counts, True)))
            for _ in range(200):
                if counts['connected'] == viewers + int(slow): break
                for t in tasks:
                    if t.done(): t.result()
                await asyncio.sleep(.01)
            await asyncio.sleep(.1)
            before=proc_stats(process.pid); start=time.monotonic()
            await generate(probe,args.rate,args.duration,long_name)
            elapsed=time.monotonic()-start; after=proc_stats(process.pid)
            await asyncio.sleep(.2)
            ordered=sorted(probe.latencies)
            percentile=lambda q: ordered[min(len(ordered)-1, int(q*len(ordered)))] if ordered else None
            status=await http(args.http_port,'GET','/api/v1/system/dns/capture') if label!='base' else {}
            return dict(candidate=label,rep=rep,viewers=viewers,slow_reader=slow,capture=enabled,
                        long_unique=long_name,target_qps=args.rate,seconds=elapsed,sent=probe.sent,
                        received=probe.received,lost=probe.sent-probe.received,
                        p50_us=percentile(.50),p95_us=percentile(.95),p99_us=percentile(.99),
                        cpu_pct=(after['cpu']-before['cpu'])/elapsed*100,rss_kib=after['rss_kib'],
                        fds_before=before['fds'],fds_after=after['fds'],stream=counts,capture_state=status)
        finally:
            for t in tasks:t.cancel()
            await asyncio.gather(*tasks,return_exceptions=True)
            if transport:transport.close()
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
            log.close()

async def main(args):
    binaries=dict(pair.split('=',1) for pair in args.binary)
    upstream,_=await asyncio.get_running_loop().create_datagram_endpoint(Upstream,local_addr=('127.0.0.1',args.upstream_port))
    try:
        with open(args.out,'w') as result:
            for rep in range(1,args.repeats+1):
                labels=list(binaries) if rep%2 else list(reversed(binaries))
                for long_name in (False,True):
                    for label in labels:
                        cases=[(0,False,False)] if label=='base' else [(0,False,False),(0,True,False),(1,True,False),(4,True,False),(8,True,False),(1,True,True)]
                        for viewers,enabled,slow in cases:
                            row=await run_one(binaries[label],label,viewers,long_name,enabled,slow,args,rep)
                            result.write(json.dumps(row)+'\n');result.flush()
                            print(label,rep,'long' if long_name else 'short',viewers,enabled,slow,'lost',row['lost'],'p99',round(row['p99_us'],1),flush=True)
    finally:upstream.close()

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary',action='append',required=True);p.add_argument('--out',required=True)
    p.add_argument('--rate',type=int,default=3000);p.add_argument('--duration',type=float,default=3)
    p.add_argument('--repeats',type=int,default=3)
    p.add_argument('--http-port',type=int,default=18090);p.add_argument('--proxy-port',type=int,default=15353);p.add_argument('--upstream-port',type=int,default=15354)
    args=p.parse_args()
    if args.rate<=0 or args.duration<=0 or args.rate*args.duration>50000:p.error('positive duration/rate with at most 50000 probes required')
    asyncio.run(main(args))
