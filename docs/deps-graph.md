# `lainc` 模块依赖图（实测生成）

由 `.\build\lainc_stage2.exe src/driver/cli.lain --dump-deps` 直接导出（71 个模块节点 `m0`~`m70`）：

```mermaid
flowchart LR
  m0["src/driver/cli.lain"]
  m1["std/os.lain"]
  m2["std/option.lain"]
  m3["std/cli.lain"]
  m4["std/str.lain"]
  m5["src/driver/state.lain"]
  m6["src/driver/host_rt.lain"]
  m7["std/trace.lain"]
  m8["src/driver/err.lain"]
  m9["std/fmt.lain"]
  m10["std/io.lain"]
  m11["std/mem.lain"]
  m12["src/host/symtab.lain"]
  m13["src/meta/ast_out.lain"]
  m14["src/meta/ast.lain"]
  m15["src/host/source_host.lain"]
  m16["src/frontend/reader.lain"]
  m17["src/host/ir_host.lain"]
  m18["src/ir/ir_codes.lain"]
  m19["src/ir/verify/model.lain"]
  m20["src/ir/verify/checks.lain"]
  m21["src/backend/c99.lain"]
  m22["src/backend/c99/writer.lain"]
  m23["src/backend/c99/inst.lain"]
  m24["src/backend/c99/module.lain"]
  m25["src/host/vm_host.lain"]
  m26["src/vm/vspace.lain"]
  m27["src/vm/image.lain"]
  m28["src/vm/engine.lain"]
  m29["src/vm/engine/state.lain"]
  m30["src/vm/engine/run.lain"]
  m31["src/vm/engine/ops.lain"]
  m32["src/ir/emit_text.lain"]
  m33["src/ir/opt.lain"]
  m34["src/sema/eval.lain"]
  m35["src/sema/types.lain"]
  m36["src/driver/sym.lain"]
  m37["src/sema/env.lain"]
  m38["src/sema/agg.lain"]
  m39["src/sema/ir_emit.lain"]
  m40["src/sema/ast.lain"]
  m41["src/sema/ctx.lain"]
  m42["src/sema/modules.lain"]
  m43["src/sema/builtins.lain"]
  m44["src/sema/tyreg.lain"]
  m45["src/comptime/ctypes.lain"]
  m46["src/sema/tbuild.lain"]
  m47["src/comptime/ctfn.lain"]
  m48["src/comptime/ctfor.lain"]
  m49["src/comptime/ctprim.lain"]
  m50["src/sema/core.lain"]
  m51["src/comptime/comptime.lain"]
  m52["src/comptime/reflect.lain"]
  m53["src/sema/lval.lain"]
  m54["src/sema/anon.lain"]
  m55["src/sema/calls.lain"]
  m56["src/sema/drop.lain"]
  m57["src/sema/flow.lain"]
  m58["src/sema/stmt.lain"]
  m59["src/sema/witness.lain"]
  m60["src/sema/effects.lain"]
  m61["src/sema/effects/row.lain"]
  m62["src/sema/effects/infer.lain"]
  m63["src/sema/effects/lower.lain"]
  m64["src/sema/member.lain"]
  m65["src/sema/refval.lain"]
  m66["src/sema/generics.lain"]
  m67["src/sema/genunify.lain"]
  m68["src/sema/decl.lain"]
  m69["src/syntax/expand.lain"]
  m70["src/driver/inspect.lain"]
  m1 --> m2
  m0 --> m1
  m3 --> m2
  m3 --> m1
  m3 --> m4
  m0 --> m3
  m0 --> m4
  m6 --> m1
  m7 --> m1
  m6 --> m7
  m9 --> m10
  m11 --> m1
  m9 --> m11
  m8 --> m9
  m6 --> m8
  m12 --> m11
  m12 --> m9
  m12 --> m1
  m6 --> m12
  m13 --> m11
  m14 --> m11
  m13 --> m14
  m13 --> m12
  m6 --> m13
  m15 --> m12
  m15 --> m16
  m15 --> m13
  m15 --> m14
  m15 --> m11
  m6 --> m15
  m17 --> m18
  m19 --> m18
  m17 --> m19
  m20 --> m18
  m20 --> m19
  m17 --> m20
  m21 --> m19
  m21 --> m12
  m22 --> m18
  m22 --> m19
  m22 --> m12
  m22 --> m1
  m21 --> m22
  m23 --> m18
  m23 --> m19
  m23 --> m1
  m23 --> m22
  m21 --> m23
  m24 --> m18
  m24 --> m19
  m24 --> m7
  m24 --> m22
  m24 --> m23
  m21 --> m24
  m17 --> m21
  m17 --> m11
  m17 --> m12
  m6 --> m17
  m25 --> m18
  m25 --> m19
  m25 --> m26
  m27 --> m18
  m27 --> m19
  m27 --> m26
  m25 --> m27
  m28 --> m26
  m28 --> m27
  m29 --> m18
  m29 --> m19
  m29 --> m26
  m29 --> m27
  m28 --> m29
  m30 --> m18
  m30 --> m19
  m30 --> m26
  m30 --> m27
  m30 --> m29
  m31 --> m18
  m31 --> m19
  m31 --> m26
  m31 --> m27
  m31 --> m29
  m30 --> m31
  m28 --> m30
  m25 --> m28
  m25 --> m17
  m6 --> m25
  m6 --> m28
  m32 --> m18
  m32 --> m19
  m32 --> m20
  m32 --> m12
  m32 --> m11
  m6 --> m32
  m6 --> m21
  m6 --> m19
  m33 --> m18
  m33 --> m19
  m6 --> m33
  m6 --> m11
  m5 --> m6
  m0 --> m5
  m34 --> m5
  m35 --> m18
  m36 --> m6
  m35 --> m36
  m34 --> m35
  m37 --> m5
  m37 --> m35
  m38 --> m5
  m38 --> m35
  m39 --> m6
  m39 --> m5
  m39 --> m35
  m39 --> m18
  m38 --> m39
  m38 --> m18
  m37 --> m38
  m34 --> m37
  m34 --> m39
  m34 --> m18
  m34 --> m36
  m40 --> m6
  m40 --> m5
  m40 --> m35
  m34 --> m40
  m41 --> m5
  m41 --> m35
  m41 --> m37
  m41 --> m39
  m41 --> m18
  m41 --> m36
  m41 --> m40
  m41 --> m38
  m42 --> m5
  m42 --> m35
  m42 --> m39
  m42 --> m6
  m42 --> m9
  m41 --> m42
  m43 --> m5
  m43 --> m35
  m43 --> m39
  m43 --> m18
  m43 --> m38
  m43 --> m36
  m43 --> m40
  m43 --> m42
  m43 --> m6
  m41 --> m43
  m44 --> m5
  m44 --> m35
  m44 --> m38
  m44 --> m42
  m44 --> m39
  m44 --> m36
  m41 --> m44
  m45 --> m5
  m45 --> m35
  m45 --> m38
  m45 --> m44
  m45 --> m39
  m41 --> m45
  m46 --> m5
  m46 --> m35
  m46 --> m36
  m46 --> m38
  m46 --> m39
  m46 --> m44
  m46 --> m40
  m41 --> m46
  m47 --> m5
  m47 --> m6
  m47 --> m40
  m47 --> m36
  m47 --> m35
  m47 --> m42
  m47 --> m44
  m47 --> m38
  m47 --> m45
  m47 --> m46
  m48 --> m5
  m48 --> m35
  m48 --> m40
  m48 --> m36
  m48 --> m38
  m48 --> m39
  m48 --> m42
  m48 --> m44
  m47 --> m48
  m49 --> m5
  m49 --> m40
  m49 --> m36
  m49 --> m42
  m49 --> m44
  m49 --> m45
  m49 --> m46
  m49 --> m48
  m47 --> m49
  m41 --> m47
  m41 --> m48
  m34 --> m41
  m50 --> m5
  m50 --> m35
  m50 --> m37
  m50 --> m39
  m50 --> m18
  m50 --> m36
  m50 --> m40
  m50 --> m41
  m50 --> m38
  m50 --> m42
  m50 --> m43
  m51 --> m5
  m51 --> m6
  m50 --> m51
  m52 --> m5
  m52 --> m35
  m52 --> m37
  m52 --> m44
  m52 --> m42
  m52 --> m39
  m52 --> m18
  m52 --> m36
  m52 --> m40
  m52 --> m41
  m50 --> m52
  m50 --> m44
  m50 --> m45
  m53 --> m5
  m53 --> m35
  m53 --> m37
  m53 --> m39
  m53 --> m18
  m53 --> m36
  m53 --> m40
  m53 --> m38
  m53 --> m42
  m53 --> m45
  m53 --> m44
  m53 --> m48
  m50 --> m53
  m54 --> m5
  m54 --> m35
  m54 --> m36
  m54 --> m40
  m54 --> m41
  m54 --> m38
  m54 --> m42
  m54 --> m45
  m54 --> m44
  m50 --> m54
  m55 --> m5
  m55 --> m35
  m55 --> m39
  m55 --> m40
  m55 --> m41
  m55 --> m38
  m55 --> m42
  m55 --> m43
  m55 --> m37
  m55 --> m44
  m55 --> m46
  m55 --> m48
  m56 --> m5
  m56 --> m35
  m56 --> m37
  m56 --> m39
  m56 --> m18
  m56 --> m36
  m56 --> m40
  m56 --> m38
  m56 --> m42
  m56 --> m41
  m55 --> m56
  m50 --> m55
  m57 --> m5
  m57 --> m35
  m57 --> m37
  m57 --> m39
  m57 --> m18
  m57 --> m36
  m57 --> m40
  m57 --> m41
  m57 --> m38
  m50 --> m57
  m58 --> m5
  m58 --> m35
  m58 --> m37
  m58 --> m39
  m58 --> m18
  m58 --> m36
  m58 --> m40
  m58 --> m41
  m58 --> m38
  m58 --> m53
  m58 --> m57
  m58 --> m44
  m58 --> m45
  m58 --> m48
  m58 --> m46
  m59 --> m5
  m59 --> m35
  m59 --> m40
  m59 --> m36
  m59 --> m38
  m59 --> m42
  m59 --> m44
  m59 --> m45
  m59 --> m48
  m59 --> m47
  m59 --> m41
  m58 --> m59
  m58 --> m42
  m58 --> m47
  m58 --> m49
  m60 --> m5
  m60 --> m35
  m60 --> m42
  m61 --> m5
  m61 --> m35
  m61 --> m36
  m61 --> m40
  m61 --> m41
  m61 --> m38
  m61 --> m42
  m61 --> m44
  m61 --> m48
  m61 --> m47
  m61 --> m46
  m60 --> m61
  m62 --> m5
  m62 --> m39
  m62 --> m36
  m62 --> m40
  m62 --> m42
  m62 --> m61
  m60 --> m62
  m63 --> m5
  m63 --> m35
  m63 --> m39
  m63 --> m18
  m63 --> m36
  m63 --> m40
  m63 --> m41
  m63 --> m38
  m63 --> m42
  m63 --> m44
  m63 --> m48
  m63 --> m47
  m63 --> m46
  m63 --> m37
  m63 --> m55
  m63 --> m56
  m63 --> m6
  m63 --> m61
  m63 --> m62
  m60 --> m63
  m58 --> m60
  m58 --> m56
  m58 --> m6
  m50 --> m58
  m64 --> m5
  m64 --> m35
  m64 --> m37
  m64 --> m39
  m64 --> m18
  m64 --> m36
  m64 --> m40
  m64 --> m41
  m64 --> m38
  m64 --> m42
  m64 --> m43
  m64 --> m55
  m65 --> m5
  m65 --> m35
  m65 --> m37
  m65 --> m39
  m65 --> m18
  m65 --> m36
  m65 --> m40
  m65 --> m41
  m65 --> m38
  m65 --> m42
  m65 --> m43
  m65 --> m44
  m65 --> m45
  m65 --> m55
  m65 --> m48
  m65 --> m56
  m64 --> m65
  m66 --> m5
  m66 --> m35
  m66 --> m40
  m66 --> m36
  m66 --> m39
  m66 --> m42
  m66 --> m44
  m66 --> m46
  m66 --> m48
  m66 --> m49
  m66 --> m47
  m66 --> m41
  m66 --> m55
  m66 --> m59
  m67 --> m5
  m67 --> m40
  m67 --> m36
  m67 --> m38
  m67 --> m44
  m67 --> m45
  m67 --> m48
  m67 --> m49
  m67 --> m47
  m66 --> m67
  m66 --> m60
  m66 --> m6
  m64 --> m66
  m64 --> m44
  m64 --> m48
  m64 --> m46
  m64 --> m47
  m64 --> m45
  m64 --> m59
  m50 --> m64
  m50 --> m65
  m50 --> m48
  m50 --> m47
  m50 --> m59
  m50 --> m66
  m50 --> m60
  m34 --> m50
  m68 --> m5
  m68 --> m35
  m68 --> m37
  m68 --> m39
  m68 --> m18
  m68 --> m36
  m68 --> m40
  m68 --> m41
  m68 --> m50
  m68 --> m58
  m68 --> m64
  m68 --> m38
  m68 --> m42
  m68 --> m54
  m68 --> m45
  m68 --> m44
  m68 --> m46
  m68 --> m48
  m68 --> m66
  m68 --> m59
  m68 --> m60
  m68 --> m56
  m68 --> m7
  m68 --> m6
  m34 --> m68
  m34 --> m38
  m34 --> m42
  m34 --> m43
  m34 --> m52
  m34 --> m51
  m34 --> m45
  m34 --> m44
  m34 --> m54
  m34 --> m48
  m34 --> m46
  m34 --> m47
  m34 --> m59
  m34 --> m66
  m69 --> m5
  m69 --> m40
  m69 --> m36
  m69 --> m39
  m69 --> m51
  m69 --> m6
  m69 --> m10
  m34 --> m69
  m34 --> m60
  m34 --> m58
  m34 --> m56
  m34 --> m7
  m34 --> m6
  m0 --> m34
  m0 --> m69
  m0 --> m39
  m0 --> m7
  m0 --> m6
  m0 --> m8
  m70 --> m6
  m70 --> m5
  m70 --> m42
  m70 --> m38
  m70 --> m40
  m70 --> m66
  m70 --> m10
  m70 --> m1
  m70 --> m11
  m0 --> m70
```
