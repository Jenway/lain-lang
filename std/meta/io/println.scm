(meta-source "io/println")

(register-expression-macro! '|println| '|io.println|)

(define (io.raw-path name)
  (raw.node! '|expr.path|
    (record '|expr.path|
      (record.field '|path| (list name)))))

(define (io.raw-number raw)
  (raw.node! '|expr.number|
    (record '|expr.number|
      (record.field '|raw| raw))))

(define (io.raw-string raw)
  (raw.node! '|expr.string|
    (record '|expr.string|
      (record.field '|raw| raw))))

(define (io.println-string-arg arg)
  (let* ((kind (raw.kind arg)))
    (if (symbol=? kind '|expr.string|)
        (optional.value
          (record.get
            (raw.payload arg)
            '|raw|))
        (diag.raise!
          '|macro.println.expected_string|
          (record '|macro.println.expected_string|)))))

(define-pass (expression-macro |println| args)
  (let* ((raw-message (io.println-string-arg (list.first args)))
         (message (string-append (symbol->string raw-message) "\n"))
         (message-symbol (string->symbol message))
         (len-symbol (string->symbol
                       (number->string
                         (string-byte-len message)))))
    (raw.node! '|expr.call|
      (record '|expr.call|
        (record.field '|callee| (io.raw-path '|__lain_println_raw|))
        (record.field '|args|
          (list
            (io.raw-string message-symbol)
            (io.raw-number len-symbol)))))))
