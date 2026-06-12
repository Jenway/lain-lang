;; ===========================================================================
;; std/meta/driver.scm — Meta 系统加载驱动器
;;
;; 按依赖顺序加载所有 meta 模块。
;; 替代 C 侧 g_meta_files[] + load_meta_system()。
;; ===========================================================================

;; 0. 预置桩 (替代 C 侧 pre_declare_variables)
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
(load "bootstrap/bootstrap_driver.scm")
