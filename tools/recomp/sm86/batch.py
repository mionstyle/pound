#!/usr/bin/env python3
"""Translates every captured shader and compiles the GLSL with glslangValidator.

batch.py <dump dir> <out dir> [glslangValidator]   -> coverage report on stdout
"""
import collections, glob, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from translate import Translator, Unsupported

src, out = sys.argv[1], sys.argv[2]
glslang = sys.argv[3] if len(sys.argv) > 3 else 'glslangValidator'
os.makedirs(out, exist_ok=True)
stats = collections.Counter()
unsupported = collections.Counter()
errors = collections.Counter()
for f in sorted(glob.glob(os.path.join(src, '*.code'))):
    h = os.path.basename(f)[:-5]
    t = Translator(open(f, 'rb').read())
    try:
        g = t.translate()
    except Unsupported as e:
        stats[t.kind + ' unsupported'] += 1
        unsupported[str(e)] += 1
        continue
    ext = {'vertex': 'vert', 'fragment': 'frag', 'compute': 'comp'}[t.kind]
    path = os.path.join(out, '%s.%s' % (h, ext))
    open(path, 'w').write(g)
    r = subprocess.run([glslang, '-V', '--target-env', 'vulkan1.2', '-o', path + '.spv', path], capture_output=True, text=True)
    if r.returncode == 0:
        stats[t.kind + ' ok'] += 1
    else:
        stats[t.kind + ' compile error'] += 1
        msg = [l for l in r.stdout.splitlines() if 'ERROR' in l]
        errors[(msg[0].split(':', 3)[-1].strip()[:90] if msg else r.stdout[:90])] += 1
for k in sorted(stats):
    print('%-28s %d' % (k, stats[k]))
print('unsupported:', unsupported.most_common(25))
print('compile errors:', errors.most_common(12))
