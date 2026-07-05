(meta-source "io/println")

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

(define-pass* 'expression-macro '|println| (lambda (args)
  (let* ((raw-message (io.println-string-arg (list.first args)))
         (message (string-append (symbol->string raw-message) "\n"))
         (message-symbol (string->symbol message))
         (len (string-byte-len message)))
    (lain-quote `(call __lain_println_raw
                   (string ,message-symbol)
                   (number ,len))))))
