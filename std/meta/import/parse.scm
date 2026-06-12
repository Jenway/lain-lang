(meta-source "import/parse")

;; Join a path (list of symbols) into a single symbol using "::" separator.
;; e.g., (std dynarray) -> std::dynarray
(define (import.path-name path)
  (let ((strs (import.path-name-strings path (list))))
    (string->symbol (import.string-join strs "::"))))

(define (import.path-name-strings path acc)
  (if (list.empty? path)
      (list.reverse acc)
      (import.path-name-strings
        (list.rest path)
        (list.cons (symbol->string (list.first path)) acc))))

(define (import.string-join strs sep)
  (if (list.empty? strs)
      ""
      (if (list.empty? (list.rest strs))
          (list.first strs)
          (string-append (list.first strs)
                         sep
                         (import.string-join (list.rest strs) sep)))))

(define-pass (form-parser |import| form)
  (let* ((cursor (syntax.form-cursor form))
         (attrs (syntax.parse-attrs cursor (list)))
         (_kw (syntax.cursor-expect-ident! cursor))
         (path (syntax.parse-path cursor))
         (_semi (syntax.cursor-expect-punct! cursor '|;|))
         (_eof (syntax.cursor-expect-eof! cursor))
         (node (raw.node! '|import|
                 (record '|import|
                   (record.field '|attrs| attrs)
                   (record.field '|path| path)))))
    (decl.define! '|import| (import.path-name path) node)))

(define-pass (raw-normalizer |import| decl)
  (middle.normalize-plain-decl decl '|middle.import|))

