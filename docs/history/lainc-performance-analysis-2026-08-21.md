# Lainc bootstrap performance analysis — 2026-08-21

## Scope

This report profiles the bootstrap path that uses the frozen
`src/lainir/lainc.l1` compiler to compile `src/lainc/lainc.lain`.

Two measurements were used:

1. subprocess stage timing for gen1, gen2, gen3, verification, and the
   tokenizer+syntax archive slice;
2. opt-in counters inside the seed interpreter, grouped by LAIN-IR procedure,
   instruction kind, and expression kind.

The counter trace writes once at process exit. It does not print per
instruction and does not change the generated compiler artifact.

## Stage timing

| Stage | Seconds | Output bytes |
| --- | ---: | ---: |
| compile gen1 | 225.322 | 196,835 |
| compile gen2 | 229.562 | 153,380 |
| compile gen3 | 228.267 | 153,380 |
| compile tokenizer+syntax archive slice | 12.286 | 10,858 |
| all four verifier runs | 0.124 | — |

The measured subprocess total was 695.560 seconds. Compilation consumed
695.436 seconds (99.982%); verification consumed 0.018%.

Gen2 and gen3 were byte-identical. Their elapsed times differed by 0.6%, so
the long runtime is a stable execution path rather than a single anomalous
bootstrap generation. Gen1 emitted 28% more bytes than gen2/gen3 but took
roughly the same time, which rules out artifact size and final output writing
as the dominant cost.

## Internal counter result

One gen1 compilation executed 3,085,745,724 seed-interpreter steps.

Top LAIN-IR procedures by exclusive interpreter steps:

| Procedure | Calls | Steps | Share of all steps |
| --- | ---: | ---: | ---: |
| `meta_env_lookup_span` | 566,295 | 2,025,094,673 | 65.63% |
| `meta_env_binding_is_nil` | 83,809,466 | 586,666,262 | 19.01% |
| `meta_env_lookup_path` | 566,295 | 76,602,797 | 2.48% |
| `meta_atom_equal` | 3,172,766 | 67,204,038 | 2.18% |
| `tool_byte_at` | 7,007,462 | 35,037,310 | 1.14% |
| `raw_node_kind` | 4,688,046 | 23,440,230 | 0.76% |
| `raw_node_length` | 3,909,463 | 19,547,315 | 0.63% |
| `meta_env_lookup_text` | 7,344 | 17,731,632 | 0.57% |

`meta_env_lookup_span` and `meta_env_binding_is_nil` account for 84.64% of
all interpreter steps. The result is sufficiently concentrated to identify
the primary bottleneck.

The 566,295 span lookups caused 83,809,466 calls to
`meta_env_binding_is_nil`: about 148 binding checks per lookup. The current
implementation walks every binding in each environment as a linked list,
compares name length, and then compares source bytes. Parent environments are
walked the same way. Lookup cost therefore grows linearly with the number of
bindings and is repeatedly paid for the same names.

Relevant aggregate interpreter counts:

| Operation | Count |
| --- | ---: |
| expression `var` | 478,933,629 |
| expression `const` | 570,144,542 |
| expression `load` | 285,883,117 |
| expression `lea` | 285,374,630 |
| expression `call` | 127,051,831 |
| instruction `if` | 287,951,132 |
| instruction `set` | 176,868,797 |
| instruction `return` | 127,165,427 |

These counts expose a second amplification layer in the C interpreter:

- every `EXPR_VAR` calls `interp_lookup_local`, which linearly scans the
  current frame's local array by string comparison;
- every interpreted procedure call constructs a frame and dynamically
  allocates argument/local storage; there are 127 million call expressions;
- small LAIN-IR accessors such as `meta_env_binding_is_nil` are therefore
  expensive both because they execute hundreds of millions of IR steps and
  because each call crosses the generic interpreter call machinery.

The traced run took 348.616 seconds versus the 225.322-second untraced gen1
baseline. Most trace overhead comes from locating and incrementing the active
procedure counter on 127 million calls. Counts remain useful; traced wall time
must not be used as the baseline.

## Diagnosis

The primary compiler bottleneck is Meta environment name resolution implemented
as repeated linked-list scans. The seed interpreter magnifies that algorithmic
cost through linear local-variable lookup and allocation-heavy procedure
frames.

The verifier, artifact writer, and bootstrap generation number are ruled out
by direct measurement.

## Implemented optimizations and result

Three optimizations were implemented:

1. The frozen frontend's Meta environments now keep a 256-bucket name index.
   The original linked list remains in place for deterministic iteration, and
   bucket chains preserve insertion order.
2. `src/lainc` keeps its original textual Meta row format but stores row count
   and the six decoded fields in a side index in the unused upper half of each
   1 MiB table buffer. `meta_row_field` is now one direct load instead of a
   scan from the beginning of the text table.
3. Decimal quotient calculation now subtracts 10000/1000/100/10 chunks rather
   than subtracting 10 for the complete value.

Final timings:

| Stage | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| compile gen1 | 225.322 s | 56.664 s | 74.9% |
| compile gen2 | 229.562 s | 8.873 s | 96.1% |
| compile gen3 | 228.267 s | 8.915 s | 96.1% |
| three compile stages | 683.151 s | 74.452 s | 89.1% |
| tokenizer+syntax archive slice | 12.286 s | 1.186 s | 90.3% |

The final optimized gen2 trace contains 110,305,039 interpreter steps versus
3,442,146,528 before the field side index: a 96.8% reduction. `meta_field`,
previously responsible for 2.49 billion exclusive steps, disappeared from the
hot list. `sb_byte_at` fell from 746.3 million to 15.1 million exclusive steps.

An attempted per-frame string hash index in the seed interpreter was rejected:
gen1 regressed from 225.322 seconds to 273.341 seconds because computing a
hash on 478.9 million variable reads cost more than scanning the typically
small local arrays. The experiment was removed.

The final compiler passes the byte-for-byte gen2/gen3 fixed point, all three
artifacts pass `lainir-print`, the M1 executable returns 42, and the stage-E
tokenizer+syntax archive chain remains verifier-clean.

The broader `tests/lainir_lain/run_all.py` currently stops at
`run_lainc_module.py`: a module member parameter named `left` remains in the
shared parameter-types table and makes a later `left.add(...)` module call look
like a local field access. This occurs before Meta lookup and comes from the
pre-existing parameter-registration work in the dirty worktree; it is separate
from the table side index. It should be fixed by giving parameter type entries
function scope, not by changing module lookup precedence.

## Optimization order

### P0 — index Meta environment bindings

Add a hash index to each Meta environment while preserving the existing linked
list for deterministic iteration. A binding should store a name hash alongside
its source span; lookup should use hash and length before byte comparison.
Parent environments still form the lexical-scope chain.

This directly targets 84.64% of interpreter steps. The acceptance measurement
is the average binding checks per lookup, currently about 148. It should fall
close to one for hits and remain bounded for misses.

Mutation semantics matter: `meta_env_bind` must update the index immediately,
and duplicate/shadowing behavior must continue to return the same binding as
the current linked-list contract.

### P1 — index interpreter frame locals

Replace or augment `interp_lookup_local` with a per-frame name-to-slot index.
The current linear scan is invoked by 478.9 million variable expressions.
Because local names are immutable after insertion and `set` updates values in
place, an open-addressed index can preserve semantics with little complexity.

### P2 — reduce interpreted call-frame allocation

Use small inline argument/local buffers in `LainirFrame`, or a run-lifetime
frame arena, before falling back to heap allocation. Tiny accessors dominate
the call count, so avoiding `calloc`/`realloc`/`free` on their hot path should
reduce constant overhead.

Inlining selected generated accessors may help later, but it should follow the
environment index: inlining alone leaves the O(binding-count) lookup algorithm
unchanged.

## Verification plan

For every optimization:

1. run the focused compiler fixture that exercises Meta lookup;
2. run gen1 timing three times and report median elapsed time;
3. capture the internal trace and compare total steps, lookup calls, and
   binding checks per lookup;
4. require `lainir-print` success;
5. require gen2 and gen3 byte equality;
6. run the tokenizer+syntax archive slice and the current stage-E fixture.

The first optimization is successful only if it reduces both lookup work and
wall time while preserving the fixed point. A wall-time-only improvement is
too noisy; a counter-only improvement is insufficient if interpreter overhead
moves elsewhere.

The modified seed builds successfully. With tracing disabled, explicit
`lainir-seed run` smoke checks for both a direct `return_42` fixture and an
indirect-call fixture returned 42. The older
`run_execution_differential.py` invocation currently uses the historical
implicit CLI shape and stops at CLI usage parsing; it does not exercise the
interpreter. That test entry should be migrated to the explicit `run`
subcommand before it is used as a regression gate.

## 2026-08-24 non-empty-path follow-up

The one-character API probe initially appeared to run indefinitely. The
failure was state corruption rather than a stable performance measurement:

- `GeneratedSyntax.begin` pushed a nested `Unit` value through the pointer
  vector ABI, leaving its `nodes` field null;
- `Syntax.fresh_hygiene` wrote the increment to offset 0 and overwrote the
  units vector;
- `Modules.declare` wrote `next_id` to offset 0 and then used the corrupted
  vector as a `Vec`;
- `CompileRequest.sources` is a `Span` (`data` pointer plus length), but the
  generated loop indexed the span record itself instead of loading its data
  pointer first.

After manually applying those four runtime fixes to the diagnostic artifact,
the same one-character request returned a diagnostic in 0.1 seconds instead
of hanging. This separates the remaining semantic gaps from the ordinary
interpreter/scan cost. The source-level fixes are now in `src/lainc` and the
archive syntax/elaborator sources; a fresh full self-compile is still pending.

## Reproduction

Build the seed:

```text
cd seed
zig build
```

Stage profile:

```text
python scripts/profile_lainc_bootstrap.py --include-archive
```

Internal gen1 profile:

```text
python scripts/profile_lainc_bootstrap.py --trace-gen1 --stop-after-gen1
```

Each run writes `profile.json`, `analysis.md`, stdout/stderr logs, and—when
requested—`gen1-internal.tsv` under `build/profiles/lainc-<timestamp>/`.

## 2026-08-24 source-self-compile timing

The first bounded self-compile profile used a five-million-step cap.  All
steps were in `tool_source_new`: its cached fingerprint was calculated by
walking every byte of the 326,965-byte `src/lainc/lainc.lain` source.  That
made source creation itself linear in the full file size, before lexing.

`src/lainir/lainc.l1` now stores a constant-size fingerprint made from the
source length and the first/last bytes.  It is used only as a cheap filter for
captured-environment lookup; the later span/name lookup remains authoritative.
With that change, the same five-million-step cap reached the lexer, and a
20-million-step run still showed the expected lexer/advance work rather than
fingerprint construction:

| cap | elapsed | leading procedures |
| ---: | ---: | --- |
| 5M | 12.1s | `tool_lexer_next` 1.25M, `tool_advance_to` 1.04M |
| 20M | 65.2s | `tool_lexer_next` 4.82M, `tool_advance_to` 3.97M |
| 50M | 175.8s | `tool_lexer_next` 11.26M, `tool_advance_to` 9.34M |

The 50M run reached raw parsing (`raw_parse_children` 0.73M) but hit the
explicit step cap before producing an artifact.  This confirms that the long
wall time is currently dominated by the seed interpreter repeatedly executing
lexer loops and accessor calls.  It is a bounded bootstrap cost, not evidence
that the archive parser is stuck in an infinite loop.  A native/JIT execution
path or a dedicated byte-scan primitive will have a larger effect than more
Meta indexing at this stage.
