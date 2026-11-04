# Mutation round for the 37th-round pulse counter.
# ASCII-only by design: the console here is GBK, and non-ASCII literals inside a
# stdin-fed python script get re-decoded into mojibake (that is exactly why the
# first attempt raised AssertionError on its own anchor).
import io, re, subprocess

SRC = 'AVE_stepper.c'
orig = io.open(SRC, encoding='utf-8').read()

INC = '\ts_stats.pulses_total[a]++;\n'
GUARD = re.compile(r'\t//[^\n]*\n\tif \(!st->pos_active\)\n\t\{.*?\n\t\}\n', re.S)

assert orig.count(INC) == 1, 'increment anchor not unique'
assert len(GUARD.findall(orig)) == 1, 'guard block not unique'
assert orig.count('\tst->running = 0U;') == 1, 'pwm_stop anchor not unique'

guard_txt = GUARD.search(orig).group(0)

MUTS = [
    ('M1 move the ++ to AFTER the pos_active guard (the placement bug)',
     INC + '\n' + guard_txt, guard_txt + INC),
    ('M2 delete the increment entirely',
     INC, ''),
    ('M3 count the wrong axis (always index 0)',
     INC, '\ts_stats.pulses_total[0]++;\n'),
    ('M4 zero the counter in pwm_stop (i.e. give it CNT semantics)',
     '\tst->running = 0U;',
     '\ts_stats.pulses_total[axis] = 0U;\n\tst->running = 0U;'),
]

for name, old, new in MUTS:
    assert orig.count(old) == 1, 'anchor not unique: ' + name
    io.open(SRC, 'w', encoding='utf-8', newline='').write(orig.replace(old, new, 1))
    subprocess.run(['D:/mingw64/bin/gcc.exe', '-std=c99', '-I.', SRC, 'test_pt.c',
                    '-o', 'm.exe'], stderr=subprocess.DEVNULL)
    r = subprocess.run(['./m.exe'], capture_output=True)
    out = r.stdout.decode('utf-8', 'replace')   # child prints UTF-8; do NOT let
    bad = re.findall(r'\[FAIL\] (.*)', out)     # subprocess decode it as GBK
    n_ok, n_bad = out.count('[ok  ]'), out.count('[FAIL]')
    if n_ok == 0 and n_bad == 0:
        verdict = 'CRASHED rc=%d' % r.returncode
    else:
        verdict = '%d pass / %d fail' % (n_ok, n_bad)
    print('%-52s %-22s %s' % (name, verdict, 'CAUGHT' if bad else '*** SURVIVED ***'))
    for b in bad[:2]:
        print('        -> ' + b.strip().encode('ascii', 'replace').decode())

io.open(SRC, 'w', encoding='utf-8', newline='').write(orig)
print('\nsource restored')
