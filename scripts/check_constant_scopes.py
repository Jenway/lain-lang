#!/usr/bin/env python3
"""Check constant namespaces and duplicate declaration diagnostics by execution."""
from pathlib import Path
import subprocess
import sys
import tempfile
from toolchain import seed_exe

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    ('separate_modules', 'let left:Module=std::module { @export let value:i64=41; }; let right:Module=std::module { @export let value:i64=43; }; let main=std::func()->i64 { if left.value==41 && right.value==43 { return 42; } return 0; };', None),
    ('duplicate_root', 'let value:i64=1; let value:i64=2; let main=std::func()->i64 { return value; };', 5113),
    ('duplicate_module', 'let box:Module=std::module { let value:i64=1; let value:i64=2; }; let main=std::func()->i64 { return 42; };', 3013),
)

def main():
    subprocess.run([sys.executable, ROOT/'scripts/build_lain_compiler.py'], cwd=ROOT, check=True, capture_output=True)
    with tempfile.TemporaryDirectory(prefix='constant-scopes-', dir=ROOT/'build') as directory:
        work = Path(directory)
        for name, text, diagnostic in CASES:
            source = work/(name+'.lain'); source.write_text(text, encoding='utf-8')
            output = work/(name+'.l1')
            r = subprocess.run([seed_exe('lainir-seed'), ROOT/'build/bootstrap/lainc.l1', 'compiler_compile_library', output, source, ROOT/'scripts/fixtures/empty_source.lain'], cwd=ROOT, capture_output=True, text=True, timeout=60)
            if diagnostic is not None:
                actual = output.read_text() if output.exists() else r.stderr
                if not r.returncode or not (f'(error {diagnostic})' in actual or r.stderr.strip() == f'bootstrap compiler returned status {diagnostic}'):
                    raise RuntimeError(f'{name}: expected diagnostic {diagnostic}: {actual}')
            else:
                if r.returncode:
                    raise RuntimeError(f'{name}: {r.stderr}: {output.read_text() if output.exists() else "missing"}')
                v = subprocess.run([seed_exe('lainir-seed'), 'run', output, 'main'], cwd=ROOT, capture_output=True, text=True, timeout=30)
                if v.returncode or v.stdout.strip() != '42':
                    raise RuntimeError(f'{name}: expected 42: {v.stdout} {v.stderr}')
            print(f'PASS constant scope {name}')
    return 0

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
