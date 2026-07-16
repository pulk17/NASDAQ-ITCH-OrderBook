#!/usr/bin/env python3
"""Slice an ITCH 5.0 tape into gzip chunks for the browser demo.

Keeps all 'R' directory records + every message for the chosen symbols, from a
snapshot cut onward. Messages before the cut rebuild a resting-order book that
is emitted at the cut as synthetic Add messages carrying the ORIGINAL order
refs -- the same snapshot+incremental recovery a real feed uses -- so premarket
costs no bytes and the rebuilt book is byte-exact from the cut onward.

Output: <outdir>/slice.NNN.gz (each an independent gzip, kept under a CDN's
per-file limit) plus meta.json describing the set. The browser inflates each
chunk with DecompressionStream and starts replaying after the first one.

Usage:
  slice_itch.py <tape> <outdir> TOP80 [raw_mb=240] [cut=09:10] [chunk_mb=56]
  slice_itch.py <tape> <outdir> AAPL,TSLA,NVDA 40 09:10
"""
import datetime
import gzip
import json
import os
import struct
import sys

READ = 1 << 23  # 8 MB source reads


def top_symbols(n):
    with open('tools/ranking.txt') as f:
        next(f)
        return {next(f).split()[1] for _ in range(n)}


def tape_id(path):
    """S071321-v50.txt -> ('2021-07-13', 'Tue Jul 13 2021'). Falls back to the
    bare filename when it does not match the S<MMDDYY> convention."""
    name = os.path.basename(path)
    if len(name) >= 7 and name[0] in 'S' and name[1:7].isdigit():
        mm, dd, yy = int(name[1:3]), int(name[3:5]), int(name[5:7])
        d = datetime.date(2000 + yy, mm, dd)
        return d.isoformat(), d.strftime('%a %b %-d %Y')
    return name, name


def hms(ns):
    s = ns // 10**9
    return f'{s // 3600:02d}:{s // 60 % 60:02d}:{s % 60:02d}'


class Chunker:
    """Writes whole messages into rotating independent gzip files."""

    def __init__(self, outdir, chunk_raw_limit):
        self.dir, self.limit = outdir, chunk_raw_limit
        self.chunks, self.f, self.gz, self.n = [], None, None, 0
        self.total_raw = 0

    def _open(self):
        name = f'slice.{len(self.chunks):03d}.gz'
        self.path = os.path.join(self.dir, name)
        self.f = open(self.path, 'wb')
        self.gz = gzip.GzipFile(fileobj=self.f, mode='wb', compresslevel=9, mtime=0)
        self.n = 0
        self._name = name

    def _close(self):
        if self.gz is None:
            return
        self.gz.close()
        gz_bytes = self.f.tell()
        self.f.close()
        self.chunks.append({'file': self._name, 'raw_bytes': self.n, 'gz_bytes': gz_bytes})
        self.gz = None

    def write(self, data):
        if self.gz is None:
            self._open()
        self.gz.write(data)
        self.n += len(data)
        self.total_raw += len(data)
        if self.n >= self.limit:   # rotate only between complete messages
            self._close()

    def finish(self):
        self._close()
        return self.chunks


def main():
    src, outdir, symarg = sys.argv[1], sys.argv[2], sys.argv[3]
    cap = int(float(sys.argv[4]) * 1e6) if len(sys.argv) > 4 else 240_000_000
    cut = sys.argv[5] if len(sys.argv) > 5 else '09:10'
    chunk_limit = int(float(sys.argv[6]) * 1e6) if len(sys.argv) > 6 else 56_000_000
    end = sys.argv[7] if len(sys.argv) > 7 else '20:00'   # stop keeping at this time
    hh, mm = cut.split(':')
    cut_ns = (int(hh) * 3600 + int(mm) * 60) * 10**9
    eh, em = end.split(':')
    end_ns = (int(eh) * 3600 + int(em) * 60) * 10**9
    syms = top_symbols(int(symarg[3:])) if symarg.upper().startswith('TOP') \
        else set(symarg.split(','))

    os.makedirs(outdir, exist_ok=True)
    ch = Chunker(outdir, chunk_limit)
    keep = bytearray(65536)          # locate -> keep flag
    stock8 = [b' ' * 8] * 65536      # locate -> 8-char padded ticker
    kept_syms = set()
    orders = {}                      # ref -> [locate, side, shares, price] (pre-cut book)
    snapped = kept = scanned = 0
    span_from = span_to = 0
    stop = False

    def snapshot(ts_bytes):
        n = 0
        for ref, (loc, side, shares, price) in sorted(orders.items()):
            body = (b'A' + struct.pack('>HH', loc, 0) + ts_bytes
                    + struct.pack('>Q', ref) + side + struct.pack('>I', shares)
                    + stock8[loc] + struct.pack('>I', price))
            ch.write(struct.pack('>H', len(body)) + body)
            n += 1
        orders.clear()
        return n

    buf = b''
    with open(src, 'rb') as f:
        while ch.total_raw < cap and not stop:
            block = f.read(READ)
            if not block:
                break
            buf = buf + block if buf else block
            n = len(buf)
            i = 0
            while i + 2 <= n and ch.total_raw < cap:
                length = (buf[i] << 8) | buf[i + 1]
                end = i + 2 + length
                if length == 0 or end > n:
                    break
                b = buf[i + 2:end]
                t = b[0]
                loc = (b[1] << 8) | b[2]
                if t == 0x52:  # 'R': keep whole directory
                    ticker = b[11:19].decode('ascii', 'replace').strip()
                    if ticker in syms:
                        keep[loc] = 1
                        stock8[loc] = b[11:19]
                        kept_syms.add(ticker)
                    ch.write(buf[i:end])
                    kept += 1
                elif keep[loc]:
                    ts = int.from_bytes(b[5:11], 'big')
                    if orders is None and ts >= end_ns:   # reached the close — done
                        stop = True
                        break
                    if ts >= cut_ns or orders is None:
                        if orders is not None:
                            snapped = snapshot(b[5:11])
                            kept += snapped
                            orders = None  # switch to copy-through mode
                            span_from = ts
                        ch.write(buf[i:end])
                        kept += 1
                        span_to = ts
                    else:
                        if t in (0x41, 0x46):    # A / F add
                            ref = int.from_bytes(b[11:19], 'big')
                            orders[ref] = [loc, b[19:20],
                                           int.from_bytes(b[20:24], 'big'),
                                           int.from_bytes(b[32:36], 'big')]
                        elif t in (0x45, 0x43, 0x58):  # E / C exec, X cancel
                            ref = int.from_bytes(b[11:19], 'big')
                            o = orders.get(ref)
                            if o:
                                o[2] -= int.from_bytes(b[19:23], 'big')
                                if o[2] <= 0:
                                    del orders[ref]
                        elif t == 0x44:          # D delete
                            orders.pop(int.from_bytes(b[11:19], 'big'), None)
                        elif t == 0x55:          # U replace
                            old = int.from_bytes(b[11:19], 'big')
                            o = orders.pop(old, None)
                            if o:
                                new = int.from_bytes(b[19:27], 'big')
                                orders[new] = [o[0], o[1],
                                               int.from_bytes(b[27:31], 'big'),
                                               int.from_bytes(b[31:35], 'big')]
                i = end
            scanned += i
            buf = buf[i:]
            if scanned % (1 << 30) < READ:
                print(f'  scanned {scanned / 1e9:.1f} GB, kept {ch.total_raw / 1e6:.1f} MB',
                      file=sys.stderr)

    chunks = ch.finish()
    tid, label = tape_id(src)
    meta = {
        'id': tid, 'label': label, 'cut': cut,
        'raw_bytes': ch.total_raw, 'messages': kept, 'snapshot_adds': snapped,
        'span': {'from': hms(span_from), 'to': hms(span_to)},
        'symbols': sorted(kept_syms),
        'chunks': chunks,
    }
    with open(os.path.join(outdir, 'meta.json'), 'w') as m:
        json.dump(meta, m, indent=1)

    gz_total = sum(c['gz_bytes'] for c in chunks)
    print(f'{outdir}: {kept:,} messages ({snapped:,} snapshot adds), '
          f'{ch.total_raw / 1e6:.1f} MB raw -> {gz_total / 1e6:.1f} MB gz in '
          f'{len(chunks)} chunks, span {meta["span"]["from"]}-{meta["span"]["to"]}, '
          f'{len(kept_syms)} symbols')


if __name__ == '__main__':
    main()
