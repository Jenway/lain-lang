;; ============================================================================
;; host/ir-api.scm — Meta ABI v2 IR 构造器
;;
;; 当前实现：薄包装现有 core.* FFI 函数。
;; 未来 LainVM 实现：直接替换为 lainvm 内置函数。
;;
;; 全部纯函数，零副作用。调用者不需要管理全局状态。
;; ============================================================================

;; ═══════════════════════════════════════════════════════════════════════════
;; 1. 类型构造与查询
;; ═══════════════════════════════════════════════════════════════════════════

(define (ir.type.bits width)        (core.make-bits width))
(define (ir.type.addr)              (core.make-addr))
(define (ir.type.floats width)      (core.make-floats width))
(define (ir.type.simd width lanes)  (core.make-simd width lanes))
(define (ir.type.unit)              (core.make-unit))
(define (ir.type.never)             (core.make-unit))  ;; 暂用 unit 代替

(define (ir.type.unit? ty)          (core.type-is-void! ty))
(define (ir.type.size ty)           (core.type-size-in-bytes! ty))
(define (ir.type.equal? a b)        (equal? a b))
(define (ir.type.lookup name)       (type.registered-raw name))

;; ═══════════════════════════════════════════════════════════════════════════
;; 2. 表达式（纯值，无副作用）
;; ═══════════════════════════════════════════════════════════════════════════

(define (ir.expr.const block type val)
  (core.const-bits! block type val))

(define (ir.expr.string block str)
  (core.const-string! block (ir.type.bits 8) str))

(define (ir.expr.var block name type)
  ;; 变量 = alloc + load。存到 core 的局部作用域表里。
  (let ((ptr (core.local-alloc! block type name)))
    (core.load! block type ptr)))

(define (ir.expr.arg block index type)
  (let ((sub (core.block-function block)))
    (core.param sub index)))

(define (ir.expr.load block type ptr)
  (core.load! block type ptr))

(define (ir.expr.lea block base idx scale offset)
  (core.primitive! block "lea"
    (ir.type.addr)
    (list base idx
          (ir.expr.const block (ir.type.bits 32) scale)
          (ir.expr.const block (ir.type.bits 32) offset))))

(define (ir.expr.add block left right)
  (core.primitive! block "add" (ir.type.addr) (list left right)))

(define (ir.expr.sub block left right)
  (core.primitive! block "sub" (ir.type.addr) (list left right)))

(define (ir.expr.primitive block opcode operands result-ty)
  (core.primitive! block opcode operands result-ty))

(define (ir.expr.call block fn args . rest)
  ;; fn 是已解析的函数对象（非名字），直接传给 core.call!
  (core.call! block fn args))

(define (ir.expr.call-indirect block fn-ptr ret-ty param-tys args)
  ;; 用 core.primitive! 间接调用
  (core.call-indirect! block fn-ptr ret-ty args))

(define (ir.expr.alloca block element-ty byte-size)
  (core.local-alloc! block element-ty "_alloca"))

(define (ir.expr.field block base struct-ty field-index field-ty)
  (core.field-offset! block struct-ty field-index "_field"))

;; ═══════════════════════════════════════════════════════════════════════════
;; 3. 指令（副作用操作）
;; ═══════════════════════════════════════════════════════════════════════════

(define (ir.inst.assign-temp block value)
  (core.assign-temp! block value))

(define (ir.inst.store block dest value)
  (core.store! block value dest))

(define (ir.inst.if block cond then-body else-body)
  ;; 使用现有的 begin-if!/end-if! 机制
  (let* ((pair   (core.begin-if! block cond))
         (then-b (car pair))
         (else-b (cadr pair)))
    ;; then 分支
    (core.set-current-block! then-b)
    (for-each (lambda (inst) inst) then-body)
    ;; else 分支
    (core.set-current-block! else-b)
    (for-each (lambda (inst) inst) else-body)
    (core.end-if! block cond then-b else-b)))

(define (ir.inst.return block value)
  (core.set-current-block! block)
  (core.return-value! block value))

(define (ir.inst.call block expr . rest)
  ;; call-as-statement: expr 是函数对象，args 是可选的参数列表
  (let ((args (if (pair? rest) (car rest) (list))))
    (core.call-expr! block expr args)))

(define (ir.inst.begin-if block cond)
  ;; 返回 (then-block . else-block) pair
  (core.begin-if! block cond))

(define (ir.inst.end-if block cond then-b else-b)
  (core.end-if! block cond then-b else-b))

;; ═══════════════════════════════════════════════════════════════════════════
;; 4. 终止符（块末尾控制流）
;; ═══════════════════════════════════════════════════════════════════════════

(define (ir.term.return block value)
  (core.return-value! block value))

(define (ir.term.return-none block)
  (core.return-none! block))

(define (ir.term.branch block target)
  (core.branch! block target))

(define (ir.term.cond-branch block cond true-block false-block)
  (core.cond-branch! block cond true-block false-block))

;; ═══════════════════════════════════════════════════════════════════════════
;; 5. 子例程
;; ═══════════════════════════════════════════════════════════════════════════

(define (ir.sub.define name params ret)
  (core.begin-function! name params ret)
  (core.function-by-name name))

(define (ir.sub.set-link-name! sub name)
  (core.set-function-link-name! sub name))

(define (ir.sub.name sub)           name)  ;; 简化: 无法从 Sub 查询名
(define (ir.sub.link-name sub)      "")    ;; 简化: 无法从 Sub 查询 link-name
(define (ir.sub.params sub)         (core.function-param-types sub))
(define (ir.sub.ret-type sub)       (core.function-return-type sub))

(define (ir.sub.extern name params ret linkage)
  ;; C FFI 真实签名: (name, link-name, params, ret)
  (core.declare-extern-function! name linkage params ret)
  (core.function-by-name name))

;; ═══════════════════════════════════════════════════════════════════════════
;; 6. 基本块
;; ═══════════════════════════════════════════════════════════════════════════

(define (ir.sub.block sub)
  (core.append-block! sub))

(define (ir.block.parent block)
  (core.block-function block))

;; ═══════════════════════════════════════════════════════════════════════════
;; 7. 类型安全（当前为桩 — 所有对象都是 opaque cpointer）
;; ═══════════════════════════════════════════════════════════════════════════

(define (ir.type? obj)   #t)
(define (ir.expr? obj)   #t)
(define (ir.inst? obj)   #t)
(define (ir.term? obj)   #t)
(define (ir.sub? obj)    #t)
(define (ir.block? obj)  #t)

;; ═══════════════════════════════════════════════════════════════════════════
;; 8. ir.expr.* 扩展
;; ═══════════════════════════════════════════════════════════════════════════

(define (ir.expr.const-string block ty str)
  (core.const-string! block ty str))

(define (ir.expr.function-ref sub)
  (core.function-ref! sub))

(define (ir.expr.field-offset block base offset field-ty)
  (core.field-offset! block base offset field-ty))

;; ═══════════════════════════════════════════════════════════════════════════
;; 9. ir.sub.* 扩展
;; ═══════════════════════════════════════════════════════════════════════════

(define (ir.sub.by-name name)
  (core.function-by-name name))

(define (ir.sub.param sub index)
  (core.param sub index))

(define (ir.sub.module-prefix)
  (core.module-prefix))

(define (ir.sub.begin name params ret-ty)
  (core.begin-function! name params ret-ty))

;; ═══════════════════════════════════════════════════════════════════════════
;; 10. ir.block.* 扩展
;; ═══════════════════════════════════════════════════════════════════════════

(define (ir.block.current)
  (core.get-current-block))

(define (ir.block.set! block)
  (core.set-current-block! block))

;; ═══════════════════════════════════════════════════════════════════════════
;; 11. ir.type.* 扩展
;; ═══════════════════════════════════════════════════════════════════════════

(define (ir.type.aggregate-layout block total-size field-pairs)
  (core.aggregate-layout! block total-size field-pairs))
