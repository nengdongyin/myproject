import json, subprocess, re, shlex, os

db = json.load(open('build/compile_commands.json', encoding='utf-8'))

# 使用这些宏（或其别名）的源文件才需要重新做语法检查
MACRO_MARKERS = ('PARAM_MODULE_DEFINE', 'PARAM_APP_MODULE_DEFINE',
                 'IP_DRIVER_DEFINE', 'PARAM_IP_MODULE_DEFINE',
                 'PARAM_MODULE_INIT', 'IP_DRIVER_INIT')

targets = []
for e in db:
    f = e['file']
    if not os.path.exists(f):
        continue
    try:
        src = open(f, encoding='utf-8', errors='replace').read()
    except Exception:
        continue
    if any(m in src for m in MACRO_MARKERS):
        targets.append(e)

print('files using module/define macros: %d' % len(targets))

fail = 0
for e in targets:
    f = e['file']
    cmd = e['command']
    cmd = re.sub(r'-o\s+\S+\.obj\s+-c\b', '-fsyntax-only', cmd)
    argv = shlex.split(cmd, posix=False)
    r = subprocess.run(argv, cwd=e['directory'], capture_output=True)
    name = f.replace('\\', '/').rsplit('/', 1)[-1]
    if r.returncode != 0:
        fail += 1
        err = (r.stderr or r.stdout or b'').decode('gbk', errors='replace')
        print('FAIL %s' % name)
        print(err[:2000])
    else:
        print('ok   %s' % name)

print('---')
print('checked %d, failed %d' % (len(targets), fail))
