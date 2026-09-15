#!/usr/bin/env python3
"""Verify public compilation never reports malformed physical IR as success."""
from pathlib import Path
import subprocess
import sys
import tempfile
from toolchain import seed_exe

ROOT=Path(__file__).resolve().parents[1]

def main():
    with tempfile.TemporaryDirectory(prefix='compiler-artifact-verification-',dir=ROOT/'build') as directory:
        work=Path(directory)
        for name,text,valid in (
            ('valid','let main=std::func()->i32 { return 42; };',True),
            ('unresolved_member','let main=std::func()->i32 { return absent.run(); };',False),
            ('invalid_declaration','fn main() { return 42; }',False),
        ):
            source=work/(name+'.lain');source.write_text(text,encoding='utf-8')
            artifact=work/(name+'.l1')
            result=subprocess.run([sys.executable,ROOT/'scripts/run_lain_compiler.py','--library','-o',artifact,source],cwd=ROOT,capture_output=True,text=True,timeout=60)
            if valid:
                if result.returncode:
                    raise RuntimeError(result.stderr)
                run=subprocess.run([seed_exe('lainir-seed'),'run',artifact,'main'],cwd=ROOT,capture_output=True,text=True,timeout=30)
                if run.returncode or run.stdout.strip()!='42':
                    raise RuntimeError(f'valid program: {run.stdout} {run.stderr}')
            elif not result.returncode or artifact.exists():
                raise RuntimeError(f'invalid program published: {result.stdout} {result.stderr}')
            print(f'PASS compiler artifact verification {name}')
    return 0

if __name__=='__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError,subprocess.SubprocessError) as error:
        print(error,file=sys.stderr)
        raise SystemExit(1)
