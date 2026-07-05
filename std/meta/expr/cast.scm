(meta-source "expr/cast")

;; ═══════════════════════════════════════════════════
;; cast 表达式: expr as Type
;; ═══════════════════════════════════════════════════

;; ── 规范化 ──

(define-pass* 'middle-normalizer '|expr.cast| (lambda (raw-expr)
  (let* ((payload (raw.payload raw-expr))
         (inner (optional.value (record.get payload '|expr|)))
         (target-ty (optional.value (record.get payload '|ty|))))
    (middle.node! '|cast.as|
      (record '|cast.as|
        (record.field '|expr| (middle.normalize-expr inner))
        (record.field '|target| (middle.normalize-type target-ty)))))))

;; ── 降级: 跨 addr↔bits 边界时插入转换 primitive ──

;; 判断 normalized type 是否是 addr 类（&T, opaque, struct, raw-ptr）
(define (norm-type-is-addr? norm-ty)
  (let ((kind (middle.kind norm-ty)))
    (or (symbol=? kind '|types.ref|)
        (symbol=? kind '|types.raw-ptr|)
        (and (symbol=? kind '|types.path|)
             (let* ((payload (middle.payload norm-ty))
                    (name (optional.value (record.get payload '|name|))))
               (or (symbol=? name '|addr|)
                   (symbol=? name '|opaque|)
                   (symbol=? name '|CStr|)
                   (struct-registered? name)))))))

;; 将 lowered type 转成 C cpointer（struct-type pair → type.addr）
(define (type-to-cpointer lowered-ty)
  (if (struct-type? lowered-ty) (type.addr) lowered-ty))

(define-pass* 'core-expr-lowerer '|cast.as| (lambda (block expr expected-ty locals)
  (let* ((payload (middle.payload expr))
         (inner (optional.value (record.get payload '|expr|)))
         (target-norm-ty (optional.value (record.get payload '|target|)))
         (target-ir-ty (core.lower-type target-norm-ty))
         ;; 用 normalized type 判断 addr/bits（避免 struct-type pair 问题）
         (source-is-addr (core.type-is-addr!
                           (core.infer-expr-type inner locals)))
         (target-is-addr (norm-type-is-addr? target-norm-ty)))
    (if (eq? source-is-addr target-is-addr)
        ;; 同种类 — 无需转换，只用目标类型来降级
        (core.lower-expr block inner target-ir-ty locals)
         ;; 跨边界 — 先降级内部表达式，再包装转换 primitive
         (let* ((inner-ir (core.lower-expr block inner
                        (type-to-cpointer
                          (core.infer-expr-type inner locals))
                        locals)))
           (if target-is-addr
               (ir.expr.int2ptr block inner-ir)
               (ir.expr.ptr2int block inner-ir)))))))

;; ── 类型推导: 返回目标类型 ──

(define-pass* 'core-expr-inferer '|cast.as| (lambda (expr locals)
  (let* ((payload (middle.payload expr))
         (target (optional.value (record.get payload '|target|))))
    (core.lower-type target))))
