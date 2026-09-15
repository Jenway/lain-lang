#!/usr/bin/env python3
"""Execute type namespace, alias shadowing, and rejection regressions."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
from toolchain import seed_exe

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    ('prebound_members', 'let box:Module=std::module { @export let Item:std::type=i32; @export let value:i32=41; @export let run=std::func()->Item { return value+1; }; }; let main=std::func()->i32 { return box.run(); };', None),
    ('nested_members', 'let box:Module=std::module { @export let Item:std::type=i32; @export let inner:Module=std::module { @export let value:i32=41; @export let run=std::func()->i32 { return value+1; }; }; }; let main=std::func()->i32 { return box.inner.run(); };', None),
    ('repeated_members', 'let box:Module=std::module { @export let Item:std::type=i32; @export let value:i32=21; @export let run=std::func()->Item { return value; }; }; let main=std::func()->i32 { return box.run()+box.run(); };', None),
    ('forward_module', 'let outer:Module=std::module { let Alias:std::type=types.Item; @export let run=std::func()->Alias { return 42; }; }; let types:Module=std::module { @export let Item:std::type=i32; }; let main=std::func()->i32 { return outer.run(); };', None),
    ('forward_shadow', 'let Middle:std::type=i32; let outer:Module=std::module { let Alias:std::type=Middle; let Middle:std::type=Later; let Later:std::type=i8; @export let run=std::func()->Alias { return 300; }; }; let main=std::func()->i64 { if outer.run()==44 { return 42; } return 0; };', None),
    ('forward_chain', 'let outer:Module=std::module { let Alias:std::type=Middle; let Middle:std::type=Later; let Later:std::type=i32; @export let run=std::func()->Alias { return 42; }; }; let main=std::func()->i32 { return outer.run(); };', None),
    ('cyclic_alias', 'let outer:Module=std::module { let First:std::type=Second; let Second:std::type=First; @export let run=std::func()->First { return 42; }; }; let main=std::func()->i32 { return outer.run(); };', 5117),
    ('forward_local', 'let outer:Module=std::module { let Alias:std::type=Later; let Later:std::type=i32; @export let run=std::func()->Alias { return 42; }; }; let main=std::func()->i32 { return outer.run(); };', None),
    ('unknown_root', 'let Alias:std::type=Missing; let main=std::func()->Alias { return 42; };', 5117),
    ('qualified_root', 'let types:Module=std::module { @export let Item:std::type=i32; }; let Alias:std::type=types.Item; let main=std::func()->Alias { return 42; };', None),
    ('qualified_alias', 'let types:Module=std::module { @export let Item:std::type=i32; }; let outer:Module=std::module { let dep:Module=types; let Alias:std::type=dep.Item; @export let run=std::func()->Alias { return 42; }; }; let main=std::func()->i32 { return outer.run(); };', None),
    ('dual_width', 'let idf=std::func(a:T) ?{T} -> T { return a; }; let narrow:Module=std::module { let T:std::type=i8; @export let run=std::func()->i8 { return idf(300); }; }; let wide:Module=std::module { let T:std::type=i32; @export let run=std::func()->i32 { return idf(300); }; }; let main=std::func()->i64 { if narrow.run()==44 && wide.run()==300 { return 42; } return 0; };', None),
    ('alias_shadow', 'let idf=std::func(a:T) ?{T} -> T { return a; }; let Base:std::type=i32; let outer:Module=std::module { let Base:std::type=i8; let T:std::type=Base; @export let run=std::func()->i8 { return idf(300); }; }; let main=std::func()->i64 { if outer.run()==44 { return 42; } return 0; };', None),
    ('private_sibling', 'let left:Module=std::module { let Private:std::type=i32; }; let right:Module=std::module { let T:std::type=Private; @export let run=std::func()->T { return 42; }; }; let main=std::func()->i64 { return right.run(); };', 5117),
    ('duplicate', 'let T:std::type=i8; let T:std::type=i32; let main=std::func()->i64 { return 42; };', 5117),
)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', type=Path)
    args = parser.parse_args()
    if args.compiler is None:
        subprocess.run([sys.executable, ROOT/'scripts/build_lain_compiler.py'], cwd=ROOT, capture_output=True, check=True)
    compiler = (args.compiler or ROOT/'build/bootstrap/lainc.l1').resolve()
    with tempfile.TemporaryDirectory(prefix='type-scopes-', dir=ROOT/'build') as directory:
        work = Path(directory)
        for name, text, error in CASES:
            source = work/(name+'.lain'); source.write_text(text, encoding='utf-8')
            artifact = work/(name+'.l1')
            result = subprocess.run([seed_exe('lainir-seed'), compiler, 'compiler_compile_library', artifact, source, ROOT/'scripts/fixtures/empty_source.lain'], cwd=ROOT, capture_output=True, text=True, timeout=60)
            if error is not None:
                diagnostic = artifact.read_text() if artifact.exists() else result.stderr
                if not result.returncode or not diagnostic.startswith(f'(error {error})\n'):
                    raise RuntimeError(f'{name}: expected {error}: {diagnostic}')
            else:
                if result.returncode:
                    raise RuntimeError(f'{name}: compile failed: {artifact.read_text() if artifact.exists() else result.stderr}')
                run = subprocess.run([seed_exe('lainir-seed'), 'run', artifact, 'main'], cwd=ROOT, capture_output=True, text=True, timeout=30)
                if run.returncode or run.stdout.strip() != '42':
                    raise RuntimeError(f'{name}: expected 42: {run.stdout} {run.stderr}')
            print(f'PASS type scope {name}')
        leaf = work/'type_scope_leaf.lain'
        leaf.write_text('let type_scope_leaf:Module=std::module { @export let Item:std::type=i32; @export let value:i32=21; @export let run=std::func()->Item { return value; }; };', encoding='utf-8')
        source = work/'type_scope_main.lain'
        source.write_text('let first:Module=import("type_scope_leaf"); let second:Module=import("type_scope_leaf"); let Alias:std::type=first.Item; let main=std::func()->Alias { return first.run()+second.run(); };', encoding='utf-8')
        artifact = work/'imported.l1'
        result = subprocess.run([seed_exe('lainir-seed'), compiler, 'compiler_compile_library', artifact, source, leaf, ROOT/'scripts/fixtures/empty_source.lain'], cwd=ROOT, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(f'repeated_import: {artifact.read_text() if artifact.exists() else result.stderr}')
        run = subprocess.run([seed_exe('lainir-seed'), 'run', artifact, 'main'], cwd=ROOT, capture_output=True, text=True, timeout=30)
        if run.returncode or run.stdout.strip() != '42':
            raise RuntimeError(f'repeated_import: expected 42: {run.stdout} {run.stderr}')
        print('PASS type scope repeated_import')
    return 0

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
