(meta-source "control/lower")

;; ── Helper: detect if ret-ty is a product type ──
(define (core.return-is-product? ret-ty)
  (let* ((fields (type.product-field-types ret-ty)))
    (not (null? fields))))

;; ── Helper: get the value field type from a throws product (field index 1) ──
(define (core.product-value-type ret-ty)
  (let* ((fields (type.product-field-types ret-ty)))
    (if (and (not (null? fields)) (not (null? (cdr fields))))
        (car (cdr fields))
        ret-ty)))

;; ── Helper: get field type by index ──
(define (core.product-field-type-at ret-ty idx)
  (let* ((fields (type.product-field-types ret-ty))
         (len (length fields)))
    (if (< idx len) (list-ref fields idx) ret-ty)))

;; ── Helper: wrap a value as throws aggregate {flag:0, value, error:0} ──
;; Uses fixed i8-flag layout. Dead code until type.product-field-types is fixed.
(define (core.wrap-throws-return block ret-ty value-expr)
  (let* ((flag-ty (core.make-bits 8))
         (error-ty (core.make-bits 32))
         (flag-zero (core.const-bits! block flag-ty 0))
         (error-zero (core.const-bits! block error-ty 0))
         ;; Default layout: i8@0, i64@8, i32@16 — total 20 bytes
         (total-size 20)
         (pairs (list (cons 0 flag-zero)
                      (cons 8 value-expr)
                      (cons 16 error-zero))))
    (core.aggregate-layout! block total-size pairs)))

;; ── 语句降级: pipeline stage = core-stmt-lowerer ──

(define-pass (core-stmt-lowerer |middle.stmt.return| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt)) (value (optional.value (record.get payload '|value|))))
    (if (optional.none? value) (core.return-none! block)
        (let* ((inner-ty (if (core.return-is-product? ret-ty)
                             (core.product-value-type ret-ty)
                             ret-ty))
               (lowered (core.lower-expr block (optional.value value) inner-ty locals))
               (wrapped (if (core.return-is-product? ret-ty)
                            (core.wrap-throws-return block ret-ty lowered)
                            lowered)))
          (core.return-value! block wrapped)))
    locals))

(define-pass (core-stmt-lowerer |middle.stmt.tail| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt)) (expr (optional.value (record.get payload '|expr|))))
    (cond
      ((symbol=? (middle.kind expr) '|middle.expr.if|)
       (core.lower-if-tail-expr block expr ret-ty locals))
      ((symbol=? (middle.kind expr) '|middle.expr.handle|)
       (core.lower-handle-tail-expr block expr ret-ty locals))
      ((symbol=? (middle.kind expr) '|middle.expr.match|)
       (match.lower-tail-expr block expr ret-ty locals))
      (else
       (if (type.unit? ret-ty)
           (begin (core.lower-expr block expr ret-ty locals) (core.return-none! block))
           (if (core.is-effect-expr? expr)
               ;; Effect expressions (perform/resume/handle): use full product type, no wrapping
               (core.return-value! block (core.lower-expr block expr ret-ty locals))
               ;; Normal expressions: lower with inner type, then wrap
               (let* ((inner-ty (if (core.return-is-product? ret-ty)
                                    (core.product-value-type ret-ty)
                                    ret-ty))
                      (lowered (core.lower-expr block expr inner-ty locals))
                      (wrapped (if (core.return-is-product? ret-ty)
                                   (core.wrap-throws-return block ret-ty lowered)
                                   lowered)))
                 (core.return-value! block wrapped))))))
    locals))

(define-pass (core-stmt-lowerer |middle.stmt.expr| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt)) (expr (optional.value (record.get payload '|expr|))))
    (cond
      ((symbol=? (middle.kind expr) '|middle.expr.if|)
       (core.lower-if-tail-expr block expr ret-ty locals)
       (core.set-current-block! block))
      ((symbol=? (middle.kind expr) '|middle.expr.handle|)
       (core.lower-handle-tail-expr block expr ret-ty locals)
       (core.set-current-block! block))
      ((symbol=? (middle.kind expr) '|middle.expr.match|)
       (match.lower-tail-expr block expr ret-ty locals)
       (core.set-current-block! block))
      (else
       (core.lower-expr block expr (core.infer-expr-type expr locals) locals)))
    locals))

(define-pass (core-stmt-lowerer |middle.stmt.let| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt))
         (ty-option (optional.value (record.get payload '|type|)))
         (name (optional.value (record.get payload '|name|)))
         (mutable (optional.value (record.get payload '|mutable|)))
         (value-expr (optional.value (record.get payload '|value|)))
         (ty (if (optional.none? ty-option) (core.infer-expr-type value-expr locals)
                 (core.lower-type (optional.value ty-option))))
         ;; For call expressions, use call-expr! (no emit) + assign-temp!
         ;; to avoid double emission of the call instruction.
         ;; For non-call expressions (paths, constants), lower-expr is fine
         ;; because it doesn't emit side-effecting instructions.
         (expr-kind (middle.kind value-expr))
         (var (if (symbol=? expr-kind '|middle.expr.call|)
                  (let* ((call-payload (middle.payload value-expr))
                         (callee (optional.value (record.get call-payload '|callee|)))
                         (args (optional.value (record.get call-payload '|args|)))
                         (callee-payload (middle.payload callee))
                         (path (optional.value (record.get callee-payload '|path|)))
                         (fn-name (core.path-fn-name path))
                         (intrinsic (core.invoke-intrinsic! fn-name)))
                    (if (optional.some? intrinsic)
                        ;; Intrinsic: let core.lower-expr handle it (e.g. load/store)
                        (let* ((value (core.lower-expr block value-expr ty locals)))
                          (core.assign-temp! block value))
                        ;; Regular call: use call-expr! to avoid double emission
                        (let* ((function (core.function-by-name fn-name))
                               (lowered-args (core.lower-args block args
                                                (core.function-param-types function)
                                                locals (list)))
                               ;; Phase 2: propagate callee effects to caller
                               (_callee-effs (let ((effs (propagate.lookup-fn-effects fn-name)))
                                               (propagate.record-effects! effs)))
                               (call-expr (core.call-expr! block function lowered-args)))
                          (core.assign-temp! block call-expr))))
                  (let* ((value (core.lower-expr block value-expr ty locals)))
                    (core.assign-temp! block value)))))
    (list.cons (record '|local| (record.field '|name| name) (record.field '|type| ty)
                 (record.field '|mutable| mutable) (record.field '|value| var)) locals)))

(define-pass (core-stmt-lowerer |middle.stmt.assign| block stmt ret-ty locals)
  (let* ((payload (middle.payload stmt)) (target (optional.value (record.get payload '|target|)))
         (value-expr (optional.value (record.get payload '|value|))))
    (if (symbol=? (middle.kind target) '|middle.expr.path|)
        (let* ((target-payload (middle.payload target)) (path (optional.value (record.get target-payload '|path|)))
               (name (list.first path)))
          (if (core.local-mutable? locals name)
              (let* ((ty (core.local-type locals name)) (value (core.lower-expr block value-expr ty locals)))
                (list.cons (record '|local| (record.field '|name| name) (record.field '|type| ty)
                             (record.field '|mutable| #t) (record.field '|value| value)) locals))
              (type.unsupported '|immutable-assignment|)))
        ;; ── 字段赋值: p.x = v ──
        (if (symbol=? (middle.kind target) '|middle.expr.field|)
            (let* ((target-payload (middle.payload target))
                   (base-expr (optional.value (record.get target-payload '|base|)))
                   (field-name (optional.value (record.get target-payload '|field|)))
                   (base-ty (core.infer-expr-type base-expr locals))
                   (struct-name (struct-type-name base-ty))
                   (offset (struct-field-offset struct-name field-name))
                   (field-ty (struct-field-type struct-name field-name))
                   (base-ir (core.lower-expr block base-expr base-ty locals))
                   (dest-addr (core.lea! block base-ir offset))
                   (value-ir (core.lower-expr block value-expr field-ty locals)))
              (core.store! block dest-addr value-ir)
              locals)
            (type.unsupported '|assignment-target|)))))

(define (core.lower-stmt block stmt ret-ty locals)
  (let* ((kind (middle.kind stmt)) (lowerer (pipeline.lookup '|core-stmt-lowerer| kind)))
    (if lowerer (lowerer block stmt ret-ty locals) locals)))

(define (core.lower-stmts block stmts ret-ty locals)
  (if (list.empty? stmts) (core.return-none! block)
      (let* ((next-locals (core.lower-stmt block (list.first stmts) ret-ty locals))
             ;; Follow g_current_block — stmt lowerers may create branches and switch blocks
             (current (core.get-current-block)))
        (if (list.empty? (list.rest stmts)) unit
            (core.lower-stmts current (list.rest stmts) ret-ty next-locals)))))

;; ── Match expression lowering ──

;; Helper: lower match arms into cond_br chain
;; Each non-wildcard arm compares the tag discriminant and branches.
;; The wildcard arm (_) is the fallthrough default.
(define (match.lower-arms-helper block tag-val arms ret-ty locals)
  (if (list.empty? arms)
      (core.return-none! block)
      (let* ((arm (list.first arms))
             (pattern-kind (optional.value (record.get arm '|pattern-kind|)))
             (pattern-name (optional.value (record.get arm '|pattern-name|)))
             (body (optional.value (record.get arm '|body|)))
             (body-items (optional.value (record.get (middle.payload body) '|items|)))
             (rest-arms (list.rest arms)))
        (if (symbol=? pattern-kind '|match.pattern.wildcard|)
            ;; Wildcard/default arm — lower body directly in current block
            (core.lower-stmts block body-items ret-ty locals)
            ;; Named variant arm — compare tag to discriminant, branch
            (let* ((variant-info (enum.lookup-variant pattern-name)))
              (if (not variant-info)
                  (type.unsupported '|unknown-variant|)
                  (let* ((discriminant (cdr variant-info))
                         (function (core.block-function block))
                         (arm-block (core.append-block! function))
                         (next-block (core.append-block! function))
                         (disc-const (core.const-bits! block (core.make-bits 8) discriminant))
                         (is-match (core.primitive! block '|integer.eq| (list tag-val disc-const) (core.make-bits 1))))
                    (core.cond-branch! block is-match arm-block next-block)
                    ;; Arm body
                    (core.set-current-block! arm-block)
                    (core.lower-stmts arm-block body-items ret-ty locals)
                    ;; Continue with next arms
                    (core.set-current-block! next-block)
                    (match.lower-arms-helper next-block tag-val rest-arms ret-ty locals))))))))

;; Lower a match expression as a tail expression.
;; 1. Lower scrutinee as addr (enum struct)
;; 2. Extract tag field at offset 0
;; 3. Delegate to match.lower-arms-helper for cond_br chain
(define (match.lower-tail-expr block expr ret-ty locals)
  (let* ((payload (middle.payload expr))
         (scrutinee (optional.value (record.get payload '|scrutinee|)))
         (arms (optional.value (record.get payload '|arms|)))
         ;; Lower scrutinee as addr — enums are tagged union structs
         (scrutinee-val (core.lower-expr block scrutinee (type.addr) locals))
         ;; Extract tag field: offset 0, 8-bit discriminant
         (tag-ty (core.make-bits 8))
         (tag-val (core.field-offset! block scrutinee-val 0 tag-ty)))
    (match.lower-arms-helper block tag-val arms ret-ty locals)))

