(meta-source "effects/base")

(register-effect-ctor! '|IO| 0)
(register-effect-constructor! '|IO| 0 '|io-effect|)
(register-effect-ctor! '|Alloc| 0)
(register-effect-constructor! '|Alloc| 0 '|alloc-effect|)
(register-effect-ctor! '|RawPtrRead| 0)
(register-effect-constructor! '|RawPtrRead| 0 '|raw-ptr-read-effect|)
(register-effect-ctor! '|RawPtrWrite| 0)
(register-effect-constructor! '|RawPtrWrite| 0 '|raw-ptr-write-effect|)

(register-raw-pointer-effect! '|read| '|RawPtrRead|)
(register-raw-pointer-effect! '|write| '|RawPtrWrite|)

;; ===========================================================================
;; 效果系统语法解析 — 从 common.scm 迁移到自治文件
;; ===========================================================================

;; ── perform 表达式: perform call_expr ──
(define (syntax.parse-perform-expr cursor)
  (lain-quote `(perform ,(syntax.parse-call-or-path cursor))))

;; ── resume 表达式: resume(expr?) ──
(define (syntax.parse-resume-expr cursor)
  (let* ((group (syntax.cursor-expect-group! cursor '|paren|))
         (body (syntax.group-cursor group))
         (value (if (syntax.cursor-eof? body)
                    (optional.none)
                    (optional.some (syntax.parse-expr body))))
         (_eof (syntax.cursor-expect-eof! body)))
    (lain-quote `(resume ,value))))

;; ── handle 表达式: handle EffectName<type-args> with handler { body } ──
(define (syntax.parse-handle-expr cursor)
  (let* ((effect-name (syntax.cursor-expect-ident! cursor))
         (effect-app (syntax.cursor-match-punct! cursor '|<|))
         (effect-args (if (optional.none? effect-app)
                         (list)
                         (syntax.parse-type-args cursor (list))))
         (_with (syntax.cursor-expect-ident! cursor))
         (handler-path (syntax.parse-path cursor))
         (handler-args (syntax.cursor-match-group! cursor '|paren|))
         (handler-value (if (optional.none? handler-args)
                           (lain-quote `(path ,@handler-path))
                           (lain-quote
                            `(call (path ,@handler-path)
                               ,@(syntax.parse-expr-args
                                  (optional.value handler-args))))))
         (first-body (syntax.cursor-expect-group! cursor '|brace|))
         (second-body (syntax.cursor-match-group! cursor '|brace|)))
    (if (optional.none? second-body)
        ;; handler-value 形式
        (lain-quote
         `(handle ,effect-name ,effect-args
            ,handler-value
            ,(syntax.parse-block first-body)))
        ;; handler-inline 形式
        (let* ((ops-cursor (syntax.group-cursor first-body))
               (operations
                (syntax.parse-inline-handler-operations
                 ops-cursor (list))))
          (lain-quote
           `(handle ,effect-name ,effect-args
              (handler-inline ,handler-path ,operations)
              ,(syntax.parse-block
                (optional.value second-body))))))))

;; ── inline handler 操作 ──
(define (syntax.parse-inline-handler-operation cursor)
  (let* ((name (syntax.cursor-expect-ident! cursor))
         (_colon (syntax.cursor-expect-punct! cursor '|:|))
         (_fn (syntax.cursor-expect-ident! cursor))
         (params (syntax.parse-params
                   (syntax.cursor-expect-group! cursor '|paren|)))
         (arrow (syntax.cursor-match-punct! cursor '|->|))
         (ret (if (optional.none? arrow)
                 (optional.none)
                 (optional.some (syntax.parse-type cursor))))
         (body (syntax.parse-block
                 (syntax.cursor-expect-group! cursor '|brace|))))
    (lain-quote `(handler-operation ,name ,params ,ret ,body))))

(define (syntax.parse-inline-handler-operations cursor acc)
  (if (syntax.cursor-eof? cursor)
      (list.reverse acc)
      (let* ((operation (syntax.parse-inline-handler-operation cursor))
             (_comma (syntax.cursor-match-punct! cursor '|,|)))
        (syntax.parse-inline-handler-operations
          cursor (list.cons operation acc)))))

;; ── 注册到语法分发总线 ──
(register-expr-parser! '|perform| syntax.parse-perform-expr)
(register-expr-parser! '|resume| syntax.parse-resume-expr)
(register-expr-parser! '|handle| syntax.parse-handle-expr)
