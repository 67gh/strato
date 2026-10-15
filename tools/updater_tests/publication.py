#!/usr/bin/env python3
"""Check raw-artifact wiring and execute the actual workflow shell steps locally.
Requires Python 3, PyYAML, Bash and Git. Does not upload or publish anything.
"""
from pathlib import Path
import os, re, subprocess, tempfile
import yaml
ROOT=Path(__file__).resolve().parents[2]
flows={n:yaml.safe_load((ROOT/'.github/workflows'/n).read_text()) for n in ['build.yml','pr_build.yml','release.yml']}
steps={n:next(iter(v['jobs'].values()))['steps'] for n,v in flows.items()}
def step(flow,name):return next(s for s in steps[flow] if s.get('name')==name)
def run(script,cwd,env,ok=True):
    r=subprocess.run(['bash','-e','-o','pipefail','-c',script],cwd=cwd,env={**os.environ,**env},capture_output=True,text=True)
    assert (r.returncode==0)==ok,(r.stdout,r.stderr,r.returncode)
    return r
for name,seq in steps.items():
    for s in seq:
        if s.get('uses','').startswith('actions/upload-artifact@'):
            assert s['uses']=='actions/upload-artifact@v7' and s['with']['archive'] is False
            assert len(s['with']['path'].splitlines())==1 and 'name' not in s['with']
        if 'run' in s:
            # Syntax only: interpolate safe placeholder text, never execute these generic blocks.
            body=re.sub(r'\$\{\{.*?\}\}','placeholder',s['run'])
            subprocess.run(['bash','-n'],input=body,text=True,check=True)
uploads=[s['with']['path'].split('/')[-1] for s in steps['build.yml'] if s.get('uses','').startswith('actions/upload-artifact@')]
downloads=[s for s in steps['release.yml'] if s.get('uses','').startswith('actions/download-artifact@')]
assert sorted(uploads)==sorted(s['with']['name'] for s in downloads)
assert all(s['uses']=='actions/download-artifact@v8' and s['with']['run-id']=='${{ steps.build.outputs.run_id }}' for s in downloads)
publish=step('release.yml','Create Release')['with']
assert publish['files']=='./artifacts/strato-mainline-release.apk'
assert publish['prerelease']=="${{ github.event_name != 'workflow_dispatch' || inputs.prerelease }}"
assert not any('zip -' in s.get('run','') or 'gh run download' in s.get('run','') for s in steps['release.yml'])
print('PASS: all uploads unarchived; v8 downloads share one run; release assets APK only; Bash syntax')
with tempfile.TemporaryDirectory(prefix='strato-publication-') as d:
    root=Path(d);env={'GITHUB_OUTPUT':str(root/'output'),'GH_REPO':'owner/repo'}
    subprocess.run(['git','init','-q'],cwd=root,check=True)
    subprocess.run(['git','-c','user.name=Test','-c','user.email=test@example.invalid','commit','--allow-empty','-qm','fixture'],cwd=root,check=True)
    sha=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
    apkdir=root/'app/build/outputs/apk/mainline/release';apkdir.mkdir(parents=True)
    prepare=step('build.yml','Prepare direct APK artifact')['run']
    run(prepare,root,env,False)
    (apkdir/'app-mainline-release.apk').write_bytes(b'APK fixture contents')
    run(prepare,root,env)
    assert (root/'direct-artifacts/strato-mainline-release.apk').read_bytes()==b'APK fixture contents'
    assert (root/'direct-artifacts/release-commit.txt').read_text().strip()==sha
    (apkdir/'second.apk').write_bytes(b'second');run(prepare,root,env,False)
    (root/'direct-artifacts').rename(root/'artifacts')
    inspect=step('release.yml','Inspect')['run'];run(inspect,root,env)
    assert 'build_sha='+sha in (root/'output').read_text()
    (root/'artifacts/release-commit.txt').write_text('invalid');run(inspect,root,env,False)
    (root/'artifacts/release-commit.txt').write_text(sha)
    notes=step('release.yml','Release notes')['run']
    title='APK direct $literal `literal`\nDeuxième ligne'
    run(notes,root,{**env,'RELEASE_TITLE':title,'BUILD_VERSION':'test','BUILD_SHA':sha})
    text=(root/'RELEASE_NOTES.md').read_text()
    assert title in text and sha in text and '${TITLE}' not in text and 'strato-mainline-release.apk' in text
    select=step('release.yml','Select successful build')['run']
    run(select,root,{**env,'TRIGGER_RUN_ID':'123'})
    assert 'run_id=123' in (root/'output').read_text()
    # A deterministic gh fixture for manual run selection; no GitHub connection.
    bindir=root/'bin';bindir.mkdir();gh=bindir/'gh'
    gh.write_text('#!/bin/sh\n[ "$1 $2" = "run list" ] || exit 99\nprintf "%s\\n" "$FAKE_RUN_ID"\n');gh.chmod(0o755)
    manual={**env,'PATH':str(bindir)+os.pathsep+os.environ['PATH'],'TRIGGER_RUN_ID':'','FAKE_RUN_ID':'456'}
    run(select,root,manual);assert 'run_id=456' in (root/'output').read_text()
    run(select,root,{**manual,'FAKE_RUN_ID':''},False)
print('PASS: actual preparation/inspection/notes/run-selection scripts; missing/multiple APK rejected; exact build SHA; manual/automatic run ID; no publication performed')
