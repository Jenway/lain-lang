from pathlib import Path
import subprocess, tempfile, sys

root = Path(__file__).resolve().parents[2]
bin_dir = root / "seed" / "zig-out" / "bin"
sources = sorted((root / "src" / "compiler-archive").glob("*.lain")) + [
    root / "std" / "memory_model.lain", root / "std" / "allocation.lain",
    root / "std" / "bounds.lain", root / "std" / "effect.lain",
    root / "std" / "core" / "vec.lain", root / "std" / "core" / "string.lain",
    root / "std" / "core" / "arena_min.lain", root / "std" / "core" / "slice.lain",
    root / "std" / "core" / "source.lain", root / "std" / "core" / "memory.lain",
]
if len(sys.argv) > 2 and sys.argv[2] == "stdonly":
    sources = [path for path in sources if path.parent.name == "core" or path.name in {"memory_model.lain", "allocation.lain", "bounds.lain", "effect.lain"}]
if len(sys.argv) > 2 and sys.argv[2] == "workspace":
    names = {"tokenizer.lain", "syntax.lain", "source_workspace.lain"}
    sources = [path for path in sources if path.name in names or path.parent.name in {"core"} or path.name in {"memory_model.lain", "allocation.lain", "bounds.lain", "effect.lain"}]
if len(sys.argv) > 2 and sys.argv[2] == "sourceonly":
    names = {"source.lain", "memory.lain"}
    sources = [path for path in sources if path.name in names or path.name in {"allocation.lain", "bounds.lain", "effect.lain"}]
if len(sys.argv) > 2 and sys.argv[2].startswith("prefix"):
    limit = int(sys.argv[2][6:])
    sources = sources[:limit]
if len(sys.argv) > 2 and sys.argv[2].startswith("part-"):
    pass
fixture = Path(sys.argv[1])
if not fixture.is_absolute() and fixture.parent.name != "build":
    fixture = root / "tests" / "lainir_lain" / "fixtures" / fixture
output = root / "build" / "probe_factory.l1"
result = subprocess.run(
        [str(bin_dir / "lainir-seed.exe"), str(root / "src" / "lainir" / "lainc.l1"),
         "compiler_compile", str(output), str(fixture), *map(str, sources)],
        cwd=root, capture_output=True, text=True, timeout=180,
    )
print(fixture.name, result.returncode, output.stat().st_size if output.exists() else 0)
print(result.stdout[-500:])
print(result.stderr[-500:])
