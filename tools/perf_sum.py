#!/usr/bin/env python3
import re, sys
def scan(path):
    r = {}
    for m in re.finditer(r'perf: screen=(\S+) render=(\d+)ms', open(path).read()):
        r[m.group(1)] = int(m.group(2))
    return r
a, b = scan(sys.argv[1]), scan(sys.argv[2])
names = sorted(set(a) | set(b))
print(f'{"screen":20s} {"before":>8s} {"after":>8s} {"delta":>8s}')
for n in names:
    x, y = a.get(n), b.get(n)
    d = (y - x) if x is not None and y is not None else None
    print(f'{n:20s} {str(x)+"ms":>8s} {str(y)+"ms":>8s} {str(d)+"ms":>8s}')
