;; ===========================================================================
;; std/meta/driver.scm — Meta 系统加载驱动器 (竖切架构)
;; ===========================================================================

;; ═══ 0. 基础设施 ═══
(load "std/meta/prelude/base.scm")
(load "std/meta/prelude/quote.scm")

;; ═══ 1. pipeline 调度 + core 调度总线 ═══
(load "std/meta/pipeline/driver.scm")
(load "std/meta/pipeline/lower.scm")

;; ═══ 1.5 共享语法解析工具 ═══
(load "std/meta/syntax/parse.scm")
;; core/call.scm → 已迁移到 expr/normalize + expr/lower + expr/infer
;; core/memory.scm → 已迁移到 memory/normalize + memory/lower + memory/infer
;; core/lower.scm → 已迁移到 pipeline/lower.scm (调度总线)

;; ═══ 2. 表达式层 ═══
(load "std/meta/expr/hooks.scm")
(load "std/meta/path/parse.scm")
(load "std/meta/expr/atom.scm")
(load "std/meta/types/parse.scm")
(load "std/meta/types/normalize.scm")
(load "std/meta/types/register.scm")
(load "std/meta/operators/parse.scm")
(load "std/meta/expr/args.scm")
(load "std/meta/expr/prec.scm")
(load "std/meta/expr/call.scm")
(load "std/meta/expr/normalize.scm")
(load "std/meta/expr/lower.scm")
(load "std/meta/expr/infer.scm")

;; ═══ 3. 属性 / 泛型 ═══
(load "std/meta/attrs/parse.scm")
(load "std/meta/generics/parse.scm")

;; ═══ 4. 内存 / 指针 ═══
(load "std/meta/memory/normalize.scm")
(load "std/meta/memory/lower.scm")
(load "std/meta/memory/infer.scm")

;; ═══ 5. 语言功能模块 ═══

(load "std/meta/struct/parse.scm")
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

(load "std/meta/effects/parse.scm")
(load "std/meta/effects/form.scm")
(load "std/meta/effects/base.scm")
(load "std/meta/effects/normalize.scm")
(load "std/meta/effects/lower.scm")
(load "std/meta/effects/infer.scm")
(load "std/meta/effects/throws.scm")
(load "std/meta/effects/suspend.scm")
(load "std/meta/effects/spawn.scm")

(load "std/meta/enum/enum.scm")

(load "std/meta/operators/integer.scm")
(load "std/meta/operators/question.scm")

(load "std/meta/interface/impl_body.scm")
(load "std/meta/interface/impl.scm")
(load "std/meta/interface/parse.scm")

(load "std/meta/import/parse.scm")
(load "std/meta/literals/lower.scm")
(load "std/meta/io/println.scm")

;; ═══ 6. 编译管线入口 ═══
(load "bootstrap/bootstrap_driver.scm")
