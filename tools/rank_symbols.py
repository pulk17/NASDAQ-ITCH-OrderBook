#!/usr/bin/env python3
"""Rank every symbol in an ITCH 5.0 tape by total message bytes (full day).

Single buffered pass; prints a table sorted by bytes with cumulative size so
you can pick how many symbols fit a slice budget:

    python3 tools/rank_symbols.py S071321-v50.txt > tools/ranking.txt

Columns: rank, ticker, messages, MB, cumulative MB.
"""
import sys

CHUNK = 1 << 23  # 8 MB


def main(path):
    counts = [0] * 65536   # bytes per locate (incl. 2-byte framing)
    msgs = [0] * 65536
    names = {}
    buf = b''
    total = 0
    with open(path, 'rb') as f:
        while True:
            chunk = f.read(CHUNK)
            if not chunk:
                break
            buf = buf + chunk if buf else chunk
            n = len(buf)
            i = 0
            while i + 2 <= n:
                length = (buf[i] << 8) | buf[i + 1]
                end = i + 2 + length
                if length == 0 or end > n:
                    break
                loc = (buf[i + 3] << 8) | buf[i + 4]
                counts[loc] += 2 + length
                msgs[loc] += 1
                if buf[i + 2] == 0x52:  # 'R' stock directory
                    names[loc] = buf[i + 13:i + 21].decode('ascii', 'replace').strip()
                i = end
            total += i
            buf = buf[i:]
            if total % (1 << 30) < CHUNK:
                print(f'  scanned {total / 1e9:.1f} GB', file=sys.stderr)

    ranked = sorted(((counts[l], msgs[l], names.get(l, f'loc{l}'), l)
                     for l in range(1, 65536) if counts[l]), reverse=True)
    cum = 0
    print(f'{"#":>4} {"ticker":<9} {"messages":>12} {"MB":>9} {"cumMB":>9}')
    for r, (b, m, sym, loc) in enumerate(ranked, 1):
        cum += b
        print(f'{r:>4} {sym:<9} {m:>12,} {b / 1e6:>9.2f} {cum / 1e6:>9.2f}')
        if r >= 500:
            break


if __name__ == '__main__':
    main(sys.argv[1])
