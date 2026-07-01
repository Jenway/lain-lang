;; ===========================================================================
;; std/meta/driver.scm — Meta 系统加载驱动器
;; ===========================================================================

;; ═══ 0. 基础设施 ═══
(load "std/meta/prelude/base.scm")
(load "std/meta/prelude/quote.scm")
(load "std/meta/prelude/check.scm")

;; ═══ 0.5. 编译器全局状态 ═══
(load "std/meta/compiler-state.scm")

;; ═══ 1. Pipeline ═══
(load "std/meta/pipeline/driver.scm")
(load "std/meta/pipeline/lower.scm")
(load "std/meta/eval.scm")

;; ═══ 2. C Pratt Parser → 结构化树 ═══
(load "std/meta/canonicalize.scm")

;; ═══ 3. 结构化树 API + 解析工具 ═══
(load "std/meta/surface/tree.scm")
(load "std/meta/surface/expr.scm")

;; ═══ 4. IR API ═══
(load "std/meta/ir-api.scm")

;; ═══ 5. 表达式层 (normalize / lower / infer) ═══
(load "std/meta/expr/hooks.scm")
(load "std/meta/path/lower.scm")
(load "std/meta/types/normalize.scm")
(load "std/meta/types/register.scm")
(load "std/meta/expr/cast.scm")
(load "std/meta/expr/normalize.scm")
(load "std/meta/expr/lower.scm")
(load "std/meta/expr/infer.scm")

;; ═══ 6. 内存 / 指针 ═══
(load "std/meta/memory/normalize.scm")
(load "std/meta/memory/lower.scm")
(load "std/meta/memory/infer.scm")

;; ═══ 7. 语言功能模块 ═══

(load "std/meta/struct/parse.scm")
(load "std/meta/struct/registry.scm")
(load "std/meta/struct/normalize.scm")
(load "std/meta/struct/lower.scm")

(load "std/meta/fn/parse.scm")
(load "std/meta/fn/normalize.scm")
(load "std/meta/fn/lower.scm")

(load "std/meta/control/parse.scm")
(load "std/meta/control/normalize.scm")
(load "std/meta/control/helpers.scm")
(load "std/meta/control/lower.scm")
(load "std/meta/control/infer.scm")
(load "std/meta/control/match.scm")

(load "std/meta/effects/form.scm")
(load "std/meta/effects/base.scm")
(load "std/meta/effects/normalize.scm")
(load "std/meta/effects/layout.scm")
(load "std/meta/effects/lower.scm")
(load "std/meta/effects/merge.scm")
(load "std/meta/effects/propagate.scm")
(load "std/meta/effects/infer.scm")
(load "std/meta/effects/throws.scm")
(load "std/meta/effects/suspend.scm")
(load "std/meta/effects/spawn.scm")

(load "std/meta/enum/enum.scm")

;; ═══ 8. Let normalizer 分发 ═══
(load "std/meta/let/normalize.scm")

(load "std/meta/operators/integer.scm")
(load "std/meta/operators/question.scm")

;; ═══ 9. Interface / Import / Build ═══
(load "std/meta/interface/impl_body.scm")
(load "std/meta/interface/impl.scm")
(load "std/meta/interface/parse.scm")

(load "std/meta/module/parse.scm")
(load "std/meta/import/parse.scm")
(load "std/meta/import/lower.scm")
(load "std/meta/build/scan.scm")
(load "std/meta/build/sort.scm")
(load "std/meta/literals/lower.scm")
(load "std/meta/io/println.scm")
(load "std/meta/compile.scm")
