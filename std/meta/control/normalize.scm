(meta-source "control/normalize")

(define (middle.normalize-expr raw-expr)
  (let* ((kind (raw.kind raw-expr))
         (normalizer (pipeline.rule '|middle-normalizer| kind)))
    (normalizer raw-expr)))

(define (middle.normalize-exprs raw-exprs acc)
  (if (list.empty? raw-exprs) (list.reverse acc)
      (let* ((expr (middle.normalize-expr (list.first raw-exprs))))
        (middle.normalize-exprs (list.rest raw-exprs) (list.cons expr acc)))))

(define (middle.normalize-stmt raw-stmt)
  (let* ((kind (raw.kind raw-stmt)) (payload (raw.payload raw-stmt)))
    (cond
      ((symbol=? kind '|stmt.return|)
       (middle.node! '|control.return| (record '|control.return|
         (record.field '|value| (let* ((value (optional.value (record.get payload '|value|))))
           (if (optional.none? value) (optional.none) (optional.some (middle.normalize-expr (optional.value value)))))))))
      ((symbol=? kind '|stmt.tail|)
       (middle.node! '|control.tail| (record '|control.tail|
         (record.field '|expr| (middle.normalize-expr (optional.value (record.get payload '|expr|)))))))
      ((symbol=? kind '|stmt.expr|)
       (middle.node! '|control.expr| (record '|control.expr|
         (record.field '|expr| (middle.normalize-expr (optional.value (record.get payload '|expr|)))))))
      ((symbol=? kind '|stmt.let|)
       (middle.node! '|let.bind| (record '|let.bind|
         (record.field '|mutable| (optional.value (record.get payload '|mutable|)))
         (record.field '|shared| (let* ((shared (record.get payload '|shared|))) (if (optional.none? shared) #f (optional.value shared))))
         (record.field '|name| (optional.value (record.get payload '|name|)))
         (record.field '|type| (let* ((ty (optional.value (record.get payload '|type|))))
           (if (optional.none? ty) (optional.none) (optional.some (middle.normalize-type (optional.value ty))))))
         (record.field '|value| (middle.normalize-expr (optional.value (record.get payload '|value|)))))))
      ((symbol=? kind '|stmt.assign|)
       (middle.node! '|stmt.assign| (record '|stmt.assign|
         (record.field '|target| (middle.normalize-expr (optional.value (record.get payload '|target|))))
         (record.field '|value| (middle.normalize-expr (optional.value (record.get payload '|value|)))))))
      (else (middle.node! '|middle.stmt.unknown| (record '|middle.stmt.unknown| (record.field '|raw-kind| kind)))))))

;; ── if 表达式规范化 (从 core/call.scm 迁移) ──

(define-pass (middle-normalizer |expr.if| raw-expr)
  (let* ((payload (raw.payload raw-expr)))
    (middle.node! '|control.if|
      (record '|control.if|
        (record.field '|condition|
          (middle.normalize-expr
            (optional.value
              (record.get payload '|condition|))))
        (record.field '|then|
          (middle.normalize-block
            (optional.value
              (record.get payload '|then|))))
        (record.field '|else|
          (middle.normalize-block
            (optional.value
              (record.get payload '|else|))))))))

(define (middle.normalize-stmts raw-stmts acc)
  (if (list.empty? raw-stmts) (list.reverse acc)
      (let* ((stmt (middle.normalize-stmt (list.first raw-stmts))))
        (middle.normalize-stmts (list.rest raw-stmts) (list.cons stmt acc)))))

(define (middle.normalize-block raw-block)
  (let* ((payload (raw.payload raw-block)) (items (optional.value (record.get payload '|items|))))
    (middle.node! '|middle.block| (record '|middle.block| (record.field '|items| (middle.normalize-stmts items (list)))))))

(define (middle.normalize-optional-body body)
  (if (optional.none? body) (optional.none) (optional.some (middle.normalize-block (optional.value body)))))

(define (middle.attrs-has? attrs name)
  (if (list.empty? attrs) #f
      (let* ((attr (list.first attrs)) (payload (raw.payload attr)) (attr-name (optional.value (record.get payload '|name|))))
        (if (symbol=? attr-name name) #t (middle.attrs-has? (list.rest attrs) name)))))

(define (middle.normalize-plain-decl decl middle-kind)
  (let* ((name (decl.name decl)) (raw (decl.payload decl)) (payload (raw.payload raw)))
    (middle.normalize-plain-item name payload middle-kind)))

(define (middle.normalize-plain-item name payload middle-kind)
  (middle.node! middle-kind (record middle-kind (record.field '|name| name) (record.field '|payload| payload))))
