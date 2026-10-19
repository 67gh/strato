#!/usr/bin/env python3
"""Host regression tests using extracted production bodies, with a fake scheduler.

Not an Android build or a guest CPU test. Set SYNC_TEST_OUTPUT for retained logs.
Run --tsan separately from the default address/undefined sanitizer build.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SRC = ROOT / 'app/src/main/cpp/skyline'
OUT = Path(os.environ.get('SYNC_TEST_OUTPUT', tempfile.mkdtemp(prefix='strato-sync-'))).resolve()
OUT.mkdir(parents=True, exist_ok=True)

def block(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

sources = {n: (SRC / n).read_text() for n in [
    'kernel/types/KProcess.cpp', 'kernel/types/KProcess.h',
    'kernel/types/KThread.cpp', 'kernel/types/KThread.h',
    'kernel/types/KSyncObject.cpp', 'kernel/svc.cpp', 'nce.cpp', 'common/spin_lock.h']}
ph, pc = sources['kernel/types/KProcess.h'], sources['kernel/types/KProcess.cpp']
assert 'std::recursive_mutex &waiterMutex' in sources['kernel/types/KThread.h']
assert 'waiterMutex(parent->syncWaiterMutex)' in sources['kernel/types/KThread.cpp']
assert ph.index('std::recursive_mutex syncWaiterMutex') < ph.index('std::vector<std::shared_ptr<KThread>> threads')

handle_generic = block(ph, '            std::shared_ptr<KObject> GetHandle<KObject>')
# GCC rejects the existing in-class explicit specialization accepted by Clang.
handle_generic = 'template<>\n' + handle_generic.replace('GetHandle<KObject>', 'KProcess::GetHandle<KObject>', 1)
handles = '\n'.join([
    'template<typename objectClass = KObject>\n' + block(ph, '            std::shared_ptr<objectClass> GetHandle('),
    'template<typename objectClass>\n' + block(ph, '            KHandle InsertItem('),
    block(ph, '            void CloseHandle(')])
methods = []
for name in ['ClearHandleTable', 'MutexLock', 'MutexUnlock', 'ConditionVariableWait', 'ConditionVariableSignal', 'WaitForAddress', 'SignalToAddress']:
    ret = 'void' if name in ['ClearHandleTable', 'MutexUnlock', 'ConditionVariableSignal'] else 'Result'
    methods.append(block(pc, f'    {ret} KProcess::{name}('))
methods.append(block(sources['kernel/types/KThread.cpp'], '    void KThread::UpdatePriorityInheritance('))
for name, ret in [('Signal', 'void'), ('ResetSignal', 'bool')]:
    methods.append(block(sources['kernel/types/KSyncObject.cpp'], f'    {ret} KSyncObject::{name}('))
svcs = '\n'.join(block(sources['kernel/svc.cpp'], '    void ' + name + '(') for name in ['ResetSignal', 'ClearEvent', 'WaitSynchronization', 'SetThreadPriority', 'GetThreadId'])
nce = sources['nce.cpp']
start = nce.index('                try {\n                    (svc.function)')
dispatch = block(nce[start:], '                try {') + ' ' + block(nce[start:], 'catch (const kernel::type::InvalidHandleException &e)')
spin = sources['common/spin_lock.h']
spin_classes = block(spin, '    class SpinLock {') + ';\n' + block(spin, '    class RecursiveSpinLock {') + ';\n'
template = (HERE / 'host.cpp').read_text()
for marker, value in {
    'SPIN': spin_classes,
    'HANDLE_EXCEPTION': block(ph, '        class InvalidHandleException') + ';',
    'HANDLES': handles,
    'GENERIC_HANDLE': handle_generic,
    'METHODS': '\n'.join(methods),
    'SVCS': svcs,
    'DISPATCH': dispatch,
}.items():
    assert template.count('/*@' + marker + '@*/') == 1
    template = template.replace('/*@' + marker + '@*/', value)
cpp = OUT / 'extracted.cpp'
cpp.write_text(template)
mode = 'thread' if '--tsan' in sys.argv else 'address,undefined'
cmd = [os.environ.get('CXX', 'g++'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Wconversion',
       '-D_GLIBCXX_DEBUG', '-fno-omit-frame-pointer', '-no-pie', '-pthread',
       '-fsanitize=' + mode, '-fno-sanitize-recover=all', str(cpp), '-o', str(OUT / 'host')]
build = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
(OUT / 'build.log').write_text(build.stdout + build.stderr)
result = {'sanitizer': mode, 'build_returncode': build.returncode, 'command': cmd,
          'sources_sha256': {n: hashlib.sha256((SRC / n).read_bytes()).hexdigest() for n in sources},
          'limitations': ['fake scheduler and DeviceState', 'host slow spin fallback', 'no Android/ARM/GPU/guest game', 'no leak sanitizer']}
if build.returncode == 0:
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', TSAN_OPTIONS='halt_on_error=1')
    run = subprocess.run([str(OUT / 'host')], capture_output=True, text=True, env=env, timeout=60)
    (OUT / 'tests.log').write_text(run.stdout + run.stderr)
    result.update(test_returncode=run.returncode, stdout=run.stdout, stderr=run.stderr)
(OUT / 'RESULTATS.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
print(json.dumps(result, ensure_ascii=False, indent=2))
if build.returncode:
    print(build.stderr, file=sys.stderr)
    sys.exit(build.returncode)
sys.exit(result['test_returncode'])
