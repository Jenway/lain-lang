(meta-source "struct/parse")

;; ═══════════════════════════════════════════════════
;; Struct 语法解析
;; ═══════════════════════════════════════════════════

;; ── 声明字段解析: name: type; ──

(define (struct.parse-fields cursor acc)
  (let* ((name (syntax.cursor-match-ident! cursor)))
    (if (optional.none? name)
        (begin
          (syntax.cursor-expect-eof! cursor)
          (list.reverse acc))
        (let* ((_colon (syntax.cursor-expect-punct! cursor '|:|))
               (ty (parse-type cursor))
               (semi (syntax.cursor-match-punct! cursor '|;|))
               (_comma (if (optional.none? semi)
                           (syntax.cursor-match-punct! cursor '|,|)
                           unit))
               (field (raw.node! '|struct.field|
                        (record '|struct.field|
                          (record.field '|name| (optional.value name))
                          (record.field '|type| ty)))))
          (struct.parse-fields cursor (list.cons field acc))))))

(define-pass (form-parser |struct| form)
  (let* ((cursor (syntax.form-cursor form))
         (attrs (parse-attrs cursor (list)))
         (_kw (syntax.cursor-expect-ident! cursor))
         (name (syntax.cursor-expect-ident! cursor))
         (generics (parse-generic-params cursor))
         (where (parse-where cursor))
         (body (syntax.cursor-expect-group! cursor '|brace|))
         (_eof (syntax.cursor-expect-eof! cursor))
         (body-cursor (syntax.group-cursor body))
         (fields (struct.parse-fields body-cursor (list)))
         (inner-payload (record '|struct|
                          (record.field '|attrs| attrs)
                          (record.field '|generics| generics)
                          (record.field '|where| where)
                          (record.field '|fields| fields)))
         (unified (raw.node! '|let|
                    (record '|let|
                      (record.field '|name| name)
                      (record.field '|type-kind| '|struct|)
                      (record.field '|payload| inner-payload)))))
    (decl.define-dup-checked! '|let| name unified)))

;; ── 字面量字段解析: { field: value, ... } ──

(define (struct.parse-literal-fields cursor acc)
  (if (syntax.cursor-eof? cursor)
      (list.reverse acc)
      (let* ((name (syntax.cursor-expect-ident! cursor))
             (_colon (syntax.cursor-expect-punct! cursor '|:|))
             (value (parse-expr cursor))
             (_comma (syntax.cursor-match-punct! cursor '|,|))
             (field (lain-quote `(struct-field ,name ,value))))
        (struct.parse-literal-fields cursor (list.cons field acc)))))

;; ── struct 字面量表达式: Name { fields } ──

(define (struct.parse-literal-expr cursor path-head type-args body)
  (let* ((fields-cursor (syntax.group-cursor body))
         (fields (struct.parse-literal-fields fields-cursor (list))))
    (if (null? type-args)
        (lain-quote `(aggregate ,path-head ,@fields))
        (lain-quote `(aggregate ,path-head
                      :type-args ,type-args
                      ,@fields)))))

(*struct-literal-parser* struct.parse-literal-expr)

;; ── self 参数: &self: Type ──

(define (struct.parse-self-param group)
  (let* ((cursor (syntax.group-cursor group))
         (name (syntax.cursor-expect-ident! cursor))
         (_colon (syntax.cursor-expect-punct! cursor '|:|))
         (_amp (syntax.cursor-expect-punct! cursor '|&|))
         (ty (syntax.cursor-expect-ident! cursor))
         (_eof (syntax.cursor-expect-eof! cursor)))
    (lain-quote `(param-self-ref ,name ,ty))))
