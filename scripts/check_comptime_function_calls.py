#!/usr/bin/env python3
"""Execute ordinary compile-time calls, dependency closure and input capture."""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile
from toolchain import seed_exe

ROOT = Path(__file__).resolve().parents[1]
CASES = {'transitive': 'let add=std::func(x:i32)->i32 { return x + 1; }; let '
               'helper=std::func(x:i32)->i32 { return add(x); }; let '
               'calculate=std::func()->i32 { return helper(41); }; let '
               'answer:i32=calculate(); let main=std::func()->i32 { return answer; };',
 'recursive': 'let sum=std::func(n:i32)->i32 { if n==0 { return 0; } return n + sum(n - '
              '1); }; let calculate=std::func()->i32 { return sum(8) + 6; }; let '
              'answer:i32=calculate(); let main=std::func()->i32 { return answer; };',
 'repeated': 'let add=std::func(x:i32)->i32 { return x + 1; }; let '
             'calculate=std::func(x:i32)->i32 { return add(x); }; let '
             'first:i32=calculate(20); let second:i32=calculate(20); let '
             'main=std::func()->i32 { return first + second; };',
 'narrow': 'let helper=std::func(value:i8)->i8 { return value; }; let '
           'calculate=std::func()->i8 { return helper(300); }; let '
           'answer:i8=calculate(); let main=std::func()->i8 { return answer; };',
 'wide': 'let helper=std::func(value:i64)->i64 { return value + 1; }; let '
         'calculate=std::func()->i64 { return helper(41); }; let answer:i64=calculate(); '
         'let main=std::func()->i64 { return answer; };',
 'mutual_recursion': 'let even=std::func(n:i32)->i32 { if n==0 { return 1; } return '
                     'odd(n - 1); }; let odd=std::func(n:i32)->i32 { if n==0 { return 0; '
                     '} return even(n - 1); }; let calculate=std::func()->i32 { return '
                     'even(8) + 41; }; let answer:i32=calculate(); let '
                     'main=std::func()->i32 { return answer; };',
 'declared-input': 'let helper=std::func() ?{bias:i32}->i32 { return bias; }; let '
                   'calculate=std::func() ?{bias:i32}->i32 { return helper(); }; let '
                   'box:Module=std::module { let bias:i32=42; @export let '
                   'answer:i32=calculate(); }; let main=std::func()->i32 { return '
                   'box.answer; };',
 'unused_invalid': 'let f=std::func() ?{bias:i32}->i32 { return absent; }; let '
                   'main=std::func()->i32 { return 42; };',
 'two_input_environments': 'let helper=std::func() ?{bias:i32}->i32 { return bias; }; '
                           'let calculate=std::func() ?{bias:i32}->i32 { return '
                           'helper(); }; let left:Module=std::module { let bias:i32=41; '
                           '@export let answer:i32=calculate(); }; let '
                           'right:Module=std::module { let bias:i32=43; @export let '
                           'answer:i32=calculate(); }; let main=std::func()->i32 { if '
                           'left.answer==41 && right.answer==43 { return 42; } return 0; '
                           '};',
 'undeclared_input_forward': 'let helper=std::func() ?{bias:i32}->i32 { return bias; }; '
                             'let calculate=std::func()->i32 { return helper(); }; let '
                             'box:Module=std::module { let bias:i32=42; @export let '
                             'answer:i32=calculate(); }; let main=std::func()->i32 { '
                             'return box.answer; };'}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', type=Path, help='Explicit isolated compiler bundle')
    args = parser.parse_args()
    compiler = args.compiler.resolve() if args.compiler else ROOT/'build/bootstrap/lainc.l1'
    if not args.compiler:
        subprocess.run([sys.executable, ROOT/'scripts/build_lain_compiler.py'], cwd=ROOT, check=True, capture_output=True)
    with tempfile.TemporaryDirectory(prefix='comptime-calls-', dir=ROOT/'build') as directory:
        work = Path(directory)
        for name, text in CASES.items():
            source = work/(name+'.lain'); source.write_text(text, encoding='utf-8')
            output = work/(name+'.l1')
            result = subprocess.run([seed_exe('lainir-seed'), compiler, 'compiler_compile_library', output, source, ROOT/'scripts/fixtures/empty_source.lain'], cwd=ROOT, capture_output=True, text=True, timeout=60)
            if name in ('unused_invalid', 'undeclared_input_forward'):
                detail = output.read_text() if output.exists() else result.stderr
                if not result.returncode or '(error 5108)' not in detail.splitlines():
                    raise RuntimeError(f'{name}: expected 5108: {detail}')
            else:
                if result.returncode:
                    raise RuntimeError(f'{name}: {result.stderr}')
                verify = subprocess.run([seed_exe('lainir-print'), output], cwd=ROOT, capture_output=True, text=True, timeout=30)
                if verify.returncode:
                    raise RuntimeError(f'{name}: {verify.stderr}')
                run = subprocess.run([seed_exe('lainir-seed'), 'run', output, 'main'], cwd=ROOT, capture_output=True, text=True, timeout=30)
                expected = '44' if name == 'narrow' else '42'
                if run.returncode or run.stdout.strip() != expected:
                    raise RuntimeError(f'{name}: expected {expected}: {run.stdout} {run.stderr}')
            print(f'PASS compile-time call {name}')
    return 0

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
