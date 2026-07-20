#!/usr/bin/env python3
"""Debt-aware checks for Lain compiler layer boundaries.

Default mode fails only when a rule has more matches than its known debt count.
Strict mode fails on every match.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parents[3]

CANONICALIZE_ALLOWED_FIXED_TAGS = frozenset(
    {
        "ident",
        "number",
        "string",
        "sep",
        "juxt",
        "prefix",
        "postfix",
        "group",
        "paren",
        "brace",
        "bracket",
        "root",
    }
)


@dataclasses.dataclass(frozen=True)
class Rule:
    ident: str
    description: str
    paths: tuple[str, ...]
    pattern: str
    allowed_count: int = 0
    rationale: str = ""


RULES: tuple[Rule, ...] = (
    Rule(
        ident="A2_CANONICALIZE_SEMANTIC_AST",
        description="canonicalize must not emit semantic AstTree nodes",
        paths=("std/meta/canonicalize.scm",),
        pattern=r"\(list 'call\b|raw\.node!|middle\.node!|\btype-app\b",
        allowed_count=0,
        rationale="Postfix paren topology must remain postfix/group, not call.",
    ),
    Rule(
        ident="S1_SURFACE_TREE_DOMAIN_PARSER",
        description="surface/tree must not host domain parsers or lowerers",
        paths=("std/meta/surface/tree.scm",),
        pattern=r"raw\.node!|middle\.node!|^\(define \(tree-(parse|lower)-",
        allowed_count=0,
        rationale="Bootstrap surface/tree must remain topology-only.",
    ),
    Rule(
        ident="C1_LAINAST_SOURCE_KEYWORDS",
        description="RawAst parser must not recognize source-level Lain keywords",
        paths=("src/lainast/*.c", "src/lainast/*.h"),
        pattern=r'"(fn|struct|effect|interface|import|let|comptime|return|match|handle|perform|resume)"',
        allowed_count=0,
        rationale="RawAst may tokenize identifiers, but it must not identify Lain keywords.",
    ),
    Rule(
        ident="C1_NATIVE_RUNTIME_IMPORT_SCAN",
        description="C runtime must not scan Lain source text for import declarations",
        paths=("src/compiler/native_runtime.c",),
        pattern=r'"import[ \t]',
        allowed_count=2,
        rationale="Known debt: build graph scanner currently searches source text for import.",
    ),
    Rule(
        ident="C1_NATIVE_COMPILER_LEGACY_ARTIFACT_API",
        description="normal CLI must use one structured compiler request/result ABI",
        paths=("src/compiler/native_compiler.c",),
        pattern=r"compiler_compile_text|compiler_compile_workspace_text|compiler_(?:workspace_)?diagnostic_",
        allowed_count=0,
        rationale=(
            "The host calls compiler_compile once and projects output or diagnostics "
            "from that owned result; it must not re-run compilation."
        ),
    ),
    Rule(
        ident="C1_BUILDER_STD_TYPE_NAMES",
        description="builder FFI must not own std/source-level type aliases",
        paths=("src/compiler/builder_ffi.c",),
        pattern=r'"CStr"|\"opaque\"',
        allowed_count=1,
        rationale="Known debt: CStr/opaque aliases are hardcoded in C type lookup.",
    ),
    Rule(
        ident="C2_BUILDER_AUTO_EXTERN_STUB",
        description="builder FFI must not synthesize undeclared extern functions",
        paths=("src/compiler/builder_ffi.c",),
        pattern=r"Auto-create stub|Default signature",
        allowed_count=2,
        rationale="Known debt: missing function lookup creates a default extern stub.",
    ),
    Rule(
        ident="C6_STRUCTURED_UNIT_NO_SOURCE_POLICY",
        description="structured L1 storage must not recognize source-language forms",
        paths=("src/compiler/structured_unit.c",),
        pattern=r'"(let|fn|module|require|struct|interface|effect|comptime)"',
        allowed_count=0,
        rationale="The structured host ABI stores physical nodes and frames only.",
    ),
    Rule(
        ident="M8_LAIN_INTERPRETER_NO_HOST_EXECUTOR",
        description="Lain interpreter/comptime must not delegate evaluation to the C executor",
        paths=(
            "packages/lain/compiler/l1_interpreter.lain",
            "packages/lain/compiler/compiler.lain",
        ),
        pattern=r"core\.execute-lainir|core\.eval!|host_unit_execute",
        allowed_count=0,
        rationale="M7/M8 evaluation policy must remain in the Lain interpreter.",
    ),
    Rule(
        ident="M4_LAINIR_SOURCE_MODULE_DEBT",
        description="LAIN-IR must not own source-level module/export state",
        paths=("src/lainir/*.c", "src/lainir/*.h"),
        pattern=(
            r"native_declare_module|native_mark_export|"
            r"native_has_explicit_exports|native_is_export_marked|"
            r"g_export_names_head|g_declared_module_names_head|"
            r"g_declared_signature_names_head|g_has_explicit_exports|"
            r"L1ExportName|lainir_reset_module_state"
        ),
        allowed_count=36,
        rationale=(
            "Known debt: old .lci/interface bridge stores source module/export "
            "metadata in LAIN-IR support code."
        ),
    ),
    Rule(
        ident="M5_LCI_LEGACY_ARTIFACT_DEBT",
        description=".lci must remain a legacy meta bridge artifact, not the module design",
        paths=(
            "src/compiler/*.c",
            "src/compiler/*.h",
            "std/meta/*.scm",
            "std/meta/**/*.scm",
            "tests/bootstrap-core/*.py",
        ),
        pattern=(
            r"\.lci|--emit-interface|core\.read-interface!|"
            r"core\.emit-interface!|host\.read-interface|"
            r"native_emit_interface|compile_interface|lci-v1|"
            r"interface emission"
        ),
        allowed_count=20,
        rationale=(
            "Known debt: .lci is the old bootstrap bridge. New module policy "
            "should move to meta-owned ModuleSummary-like artifacts."
        ),
    ),
    Rule(
        ident="V1_CHIBI_HEADER_BACKEND_ONLY",
        description="Chibi headers must be included only by the Chibi backend",
        paths=("src/compiler/*.c", "src/compiler/*.h"),
        pattern=r"^\s*#include\s+<chibi/eval\.h>",
        allowed_count=1,
        rationale="Only src/compiler/vm_chibi.c may include chibi/eval.h.",
    ),
    Rule(
        ident="V2_GAUCHE_HEADER_BACKEND_ONLY",
        description="Gauche headers must be included only by the Gauche backend",
        paths=("src/compiler/*.c", "src/compiler/*.h"),
        pattern=r"^\s*#include\s+<gauche(?:\.h|/)",
        allowed_count=9,
        rationale="Only src/compiler/vm_gauche.c may include Gauche headers.",
    ),
    Rule(
        ident="V3_SCHEME_COMPAT_SEXP_DEBT",
        description="compiler core still uses sexp-shaped compatibility names",
        paths=("src/compiler/native_runtime.c", "src/compiler/builder_ffi.c"),
        pattern=r"\bsexp\b|sexp_|SEXP_",
        allowed_count=923,
        rationale=(
            "Known debt: vm_compat.h keeps old FFI code compiling while backend APIs are split; "
            "M3-M15 and user Meta add physical RawAst, stable parsed/generated "
            "syntax-unit storage, compiler-storage, "
            "artifact orchestration, and structured-L1 queries at this pre-existing "
            "compatibility boundary."
        ),
    ),
    Rule(
        ident="M1_META_NO_DEFINE_SYNTAX",
        description="meta library must not depend on Scheme implementation macro systems",
        paths=("polyfills.scm", "std/meta/*.scm", "std/meta/**/*.scm"),
        pattern=r"\bdefine-syntax\b|\bsyntax-rules\b",
        allowed_count=0,
        rationale="Pass registration should be ordinary Scheme or an explicit host contract.",
    ),
    Rule(
        ident="M2_META_NO_DEFINE_PASS_MACRO_FORM",
        description="meta passes must use define-pass* directly",
        paths=("polyfills.scm", "std/meta/*.scm", "std/meta/**/*.scm"),
        pattern=r"^\s*\(define-pass\s",
        allowed_count=0,
        rationale="define-pass macro syntax hides a non-portable macro dependency.",
    ),
    Rule(
        ident="M3_META_NO_NAMED_LET",
        description="meta library must stay inside the explicit Scheme host subset",
        paths=("polyfills.scm", "std/meta/*.scm", "std/meta/**/*.scm"),
        pattern=r"\(let\s+[A-Za-z_][-A-Za-z0-9_.?!]*\s*\(",
        allowed_count=0,
        rationale="Use let plus set! plus lambda until named let is added to the contract.",
    ),
    Rule(
        ident="B1_NO_LAINC_BUILD_CLI",
        description="lainc CLI must not expose the legacy C-side build driver",
        paths=("src/compiler/native_compiler.c",),
        pattern=r'"--build"|build_mode|native_build_with_funcs',
        allowed_count=0,
        rationale="Build orchestration belongs in build.lain / meta libraries.",
    ),
    Rule(
        ident="P1_PIPELINE_SILENT_NOOP",
        description="pipeline must not silently ignore missing required passes",
        paths=("std/meta/pipeline/driver.scm",),
        pattern=r"lambda args unit|静默跳过",
        allowed_count=0,
        rationale="A required pass lookup must fail with its stage and kind.",
    ),
    Rule(
        ident="P2_PIPELINE_FAKE_IMPLICIT_MAIN",
        description="unknown forms must not silently become fake main",
        paths=("std/meta/pipeline/driver.scm",),
        pattern=r"parse-as-implicit-main|decl\.define! '\|fn\| '\|main\||\(number 0\)",
        allowed_count=0,
        rationale="Unknown top-level forms must fail during dispatch.",
    ),
)


@dataclasses.dataclass(frozen=True)
class Match:
    rule: Rule
    path: pathlib.Path
    line_no: int
    text: str


def iter_paths(pattern: str) -> list[pathlib.Path]:
    return sorted(p for p in ROOT.glob(pattern) if p.is_file())


def find_matches(rule: Rule) -> list[Match]:
    regex = re.compile(rule.pattern)
    matches: list[Match] = []
    for path_pattern in rule.paths:
        for path in iter_paths(path_pattern):
            try:
                lines = path.read_text(encoding="utf-8").splitlines()
            except UnicodeDecodeError:
                lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            for idx, line in enumerate(lines, start=1):
                if regex.search(line):
                    matches.append(Match(rule, path, idx, line.strip()))
    return matches


def rel(path: pathlib.Path) -> str:
    return path.relative_to(ROOT).as_posix()


def check_canonicalize_fixed_tags(strict: bool) -> bool:
    """Reject new fixed AstTree tags emitted by canonicalize.scm.

    Operator nodes such as `(+ left right)` are produced through
    `(string->symbol op-text)`, so this check only covers quoted structural tags.
    """

    path = ROOT / "std/meta/canonicalize.scm"
    if not path.exists():
        print("OK A1_CANONICALIZE_FIXED_TAG_WHITELIST: bootstrap-only")
        print("    Scheme canonicalizer is maintained on bootstrap/stage0")
        return True
    tag_regex = re.compile(r"\((?:list|cons)\s+'([A-Za-z][A-Za-z0-9_.-]*)\b")
    matches: list[tuple[int, str, str]] = []
    lines = path.read_text(encoding="utf-8").splitlines()
    for idx, line in enumerate(lines, start=1):
        for match in tag_regex.finditer(line):
            tag = match.group(1)
            if tag not in CANONICALIZE_ALLOWED_FIXED_TAGS:
                matches.append((idx, tag, line.strip()))

    status = "OK" if not matches else "FAIL"
    print(f"{status} A1_CANONICALIZE_FIXED_TAG_WHITELIST: {len(matches)} violation(s)")
    print("    canonicalize may emit only topology-level fixed AstTree tags")
    print(
        "    Operator tags are allowed only through dynamic infix topology, "
        "not quoted semantic tags."
    )

    if matches:
        for line_no, tag, text in matches:
            print(f"    {rel(path)}:{line_no}: fixed tag '{tag}' in {text}")
        return False

    if strict:
        print(
            "    Allowed fixed tags: "
            + ", ".join(sorted(CANONICALIZE_ALLOWED_FIXED_TAGS))
        )
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--strict",
        action="store_true",
        help="fail on known debt too, not only new violations",
    )
    args = parser.parse_args()

    failed = False
    print("Boundary lint")
    print("=============")

    if not check_canonicalize_fixed_tags(args.strict):
        failed = True

    for rule in RULES:
        matches = find_matches(rule)
        count = len(matches)
        allowed = 0 if args.strict else rule.allowed_count
        status = "OK" if count <= allowed else "FAIL"
        print(f"{status} {rule.ident}: {count} match(es), allowed {allowed}")
        print(f"    {rule.description}")
        if rule.rationale:
            print(f"    {rule.rationale}")

        if count > allowed:
            failed = True
            for match in matches[allowed:]:
                print(f"    {rel(match.path)}:{match.line_no}: {match.text}")
        elif count < rule.allowed_count and not args.strict:
            print(
                "    Debt count decreased. Consider lowering allowed_count "
                f"from {rule.allowed_count} to {count}."
            )

    if failed:
        print("\nBoundary lint failed.")
        return 1
    print("\nBoundary lint passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
