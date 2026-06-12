;; =============================================================================
;; std/meta/driver.sld — Meta 系统加载驱动器 (R7RS 模块化)
;;
;; 替代原 driver.scm。通过 (import (std meta driver)) 一键加载全部管线。
;; 依赖 (lain ffi) 获取 C FFI 和 pipeline 基础设施。
;; 内部通过 load 按拓扑顺序加载所有 .scm 文件。
;; =============================================================================

(define-library (std meta driver)
  (import (scheme base) (scheme load) (std ffi))
  (export
    compile-group-to-core
    pipeline.rule driver.lookup-form-parser)
  (begin
    ;; ── 按依赖顺序加载所有 meta 模块 ──
    ;; 每个 (load "...") 在当前库作用域内执行，
    ;; 因为 driver.sld 导入了 (lain ffi)，所有 FFI 和 define-pass 均可用。

    ;; 0. 预置桩 (register-* stubs)
    (load "std/meta/core/prelude.scm")

    ;; 0.5 AST 引号构造器 (lain-quote, 仅依赖 C FFI)
    (load "std/meta/core/quote.scm")

    ;; 1. 核心基础库
    (load "std/meta/core/list.scm")
    (load "std/meta/core/record.scm")
    (load "std/meta/core/literals.scm")
    (load "std/meta/core/types.scm")
    (load "std/meta/core/memory.scm")
    (load "std/meta/core/effects.scm")

    ;; 2. 语法和中间表示
    (load "std/meta/syntax/common.scm")
    (load "std/meta/middle/common.scm")

    ;; 3. 语言构造
    (load "std/meta/lang/import.scm")
    (load "std/meta/lang/struct.scm")
    (load "std/meta/lang/enum.scm")
    (load "std/meta/lang/effect.scm")
    (load "std/meta/lang/fn.scm")
    (load "std/meta/lang/impl_body.scm")
    (load "std/meta/lang/impl.scm")
    (load "std/meta/lang/interface.scm")

    ;; 4. 运算符
    (load "std/meta/operators/integer.scm")
    (load "std/meta/operators/question.scm")

    ;; 5. 调用和降级
    (load "std/meta/core/call.scm")
    (load "std/meta/core/lower.scm")

    ;; 6. 效应系统
    (load "std/meta/effects/base.scm")
    (load "std/meta/effects/throws.scm")
    (load "std/meta/effects/suspend.scm")
    (load "std/meta/effects/spawn.scm")

    ;; 7. IO
    (load "std/meta/io/println.scm")

    ;; 8. 编译管线驱动器
    (load "bootstrap/bootstrap_driver.scm")))
