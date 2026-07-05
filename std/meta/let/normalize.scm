(meta-source "let/normalize")

;; ===========================================================================
;; 统一 let normalizer — 所有顶层声明 (fn/struct/enum/interface/impl)
;; 都产出一个 |let| raw node (type-kind 区分具体类型)。
;; 本文件是唯一注册 raw-normalizer |let| 的地方，
;; 根据 type-kind 分发给对应的 normalize 逻辑。
;; ===========================================================================

(define-pass* 'raw-normalizer '|let| (lambda (decl)
  (let* ((raw (decl.payload decl))
         (payload (raw.payload raw))
         (name (optional.value (record.get payload '|name|)))
         (type-kind (optional.value (record.get payload '|type-kind|)))
         (inner-payload (optional.value (record.get payload '|payload|))))
    (cond
      ;; ── fn / foreign-fn ──
      ((symbol=? type-kind '|fn|)
       (fn.normalize name inner-payload
         (if (middle.attrs-has? (optional.value (record.get inner-payload '|attrs|)) '|foreign|)
             '|middle.foreign-fn| '|middle.fn|)))
      ;; ── struct ──
      ((symbol=? type-kind '|struct|)
       (middle.node! '|middle.struct|
         (record '|middle.struct|
           (record.field '|name| name)
           (record.field '|attrs| (optional.value (record.get inner-payload '|attrs|)))
           (record.field '|generics| (optional.value (record.get inner-payload '|generics|)))
           (record.field '|where| (optional.value (record.get inner-payload '|where|)))
           (record.field '|fields|
             (struct.normalize-fields
               (optional.value (record.get inner-payload '|fields|)) (list))))))
      ;; ── enum ──
      ((symbol=? type-kind '|enum|)
       (middle.normalize-plain-item name inner-payload '|middle.enum|))
      ;; ── interface ──
      ((symbol=? type-kind '|interface|)
       (middle.node! '|middle.interface|
         (record '|middle.interface|
           (record.field '|name| name)
           (record.field '|payload| inner-payload))))
      ;; ── import binding ──
      ((symbol=? type-kind '|import-binding|)
       (middle.node! '|middle.import|
         (record '|middle.import|
           (record.field '|name| name)
           (record.field '|payload| inner-payload))))
      ;; ── signature ──
      ((symbol=? type-kind '|signature|)
       (middle.node! '|middle.signature|
         (record '|middle.signature|
           (record.field '|name| name)
           (record.field '|payload| inner-payload))))
      ;; ── module ──
      ((symbol=? type-kind '|module|)
       (middle.node! '|middle.module|
         (record '|middle.module|
           (record.field '|name| name)
           (record.field '|payload| inner-payload))))
      ;; ── imported module/signature alias ──
      ((symbol=? type-kind '|meta-alias|)
       (middle.node! '|middle.meta-alias|
         (record '|middle.meta-alias|
           (record.field '|name| name)
           (record.field '|payload| inner-payload))))
      ;; ── impl ──
      ((symbol=? type-kind '|impl|)
       (middle.normalize-plain-item name inner-payload '|middle.impl|))
      ;; ── expr binding: let name = <expr>; ──
      ((symbol=? type-kind '|expr-binding|)
       (let* ((value-raw (optional.value (record.get inner-payload '|value|)))
              (value-middle (middle.normalize-expr value-raw)))
         (middle.node! '|middle.let-binding|
           (record '|middle.let-binding|
             (record.field '|name| name)
             (record.field '|value| value-middle)))))
      ;; ── unknown ──
      (else
       (type.unsupported '|unknown-let-type|))))))
