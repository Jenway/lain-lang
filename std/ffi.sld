;; =============================================================================
;; std/ffi.sld — FFI 桥接库 + Pipeline 基础设施
;;
;; C 侧在加载本库后，通过 module-env 注入实际 FFI 实现。
;; 仅导出 C FFI 函数，Scheme 层封装（type.unit 等）由上层 .scm 文件提供。
;; =============================================================================

(define-library (std ffi)
  (import (scheme base))
  (export
    ;; ── Pipeline 基础设施 ──
    __lain-passes define-pass* define-pass

    ;; ── 核心 IR 发射 (C FFI) ──
    core.make-bits core.make-addr core.make-unit
    core.make-set core.make-proc
    core.const-bits! core.const-string!
    core.load! core.store!
    core.begin-function! core.function-by-name core.append-block!
    core.return-value! core.return-none!
    core.function-return-type core.function-param-types
    core.function-ref!
    core.call! core.call-expr! core.assign-temp!
    core.primitive! core.local-alloc!
    core.param core.block-function
    core.branch! core.cond-branch! core.phi!
    core.type-is-void!
    core.declare-struct-name! core.declare-struct!
    core.struct-type core.struct-field-type
    core.struct-field-type-from-type
    core.struct-field-index core.struct-field-index-from-type
    core.aggregate! core.field!
    core.call-indirect! core.declare-extern-function!

    ;; ── 类型构造 (C FFI) ──
    type.registered type.raw-ptr type.raw-ptr-pointee
    type.product type.product-name type.product-field-types

    ;; ── 语法游标 (C FFI) ──
    syntax.group-cursor syntax.group-kind
    syntax.cursor-eof-raw? syntax.cursor-expect-eof-raw!
    syntax.cursor-match-punct-raw! syntax.cursor-expect-punct-raw!
    syntax.cursor-match-ident-raw! syntax.cursor-expect-ident-ffi!
    syntax.cursor-match-string-raw! syntax.cursor-match-number-raw!
    syntax.cursor-expect-number-raw!
    syntax.cursor-match-group-raw! syntax.cursor-expect-group-raw!
    syntax.cursor-get-index syntax.cursor-set-index!
    syntax.cursor-expect-ident! syntax.cursor-expect-eof!
    syntax.cursor-expect-number!

    ;; ── IO (C FFI) ──
    io.load-source-file!)

  (begin
    ;; Pipeline 注册表
    (define __lain-passes '())

    (define (define-pass* stage kind body)
      (set! __lain-passes
        (cons (cons stage (cons kind (cons body '()))) __lain-passes)))

    (define-syntax define-pass
      (syntax-rules ()
        ((_ (stage kind arg ...) body ...)
         (define-pass* 'stage 'kind (lambda (arg ...) body ...)))))))
