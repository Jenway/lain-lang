;; std/meta/build/scan.scm — Import scanner for build system
;;
;; Scans .lain source files for `import` statements and extracts
;; the imported module paths. Used by the build driver to construct
;; the dependency graph for topological sorting.

(meta-source "build/scan")

;; ── Read and scan a source file for imports ──

(define (build.scan-imports source-path)
  ;; Read the file as S-expression forms via the Scheme lexer.
  ;; Returns a list of module paths, or raises error on failure.
  (let ((forms (core.read-file-forms! source-path)))
    (if (or (not forms) (not (pair? forms)))
        (error (string-append "build: cannot read source: " source-path))
        (build.extract-imports forms (list)))))

(define (build.extract-imports forms acc)
  (if (null? forms)
      (reverse acc)
      (let* ((form (car forms))
             (import-path (build.try-extract-import form)))
        (if import-path
            (build.extract-imports (cdr forms) (cons import-path acc))
            (build.extract-imports (cdr forms) acc)))))

;; ── Extract import path from a form S-expression ──
;;
;; Form format from the lexer:
;;   (root (ident "import") (ident "compiler") (punct "::") (ident "args") (punct ";"))
;; 
;; Extract the ident tokens between "import" and ";" to get the module path.

(define (build.try-extract-import form)
  ;; form = (root (ident tag) ...)
  (if (or (not (pair? form))
          (not (eq? (car form) 'root))
          (not (pair? (cdr form)))
          (not (pair? (cadr form)))
          (not (eq? (caadr form) 'ident))
          (not (string=? (cadr (cadr form)) "import")))
      #f
      ;; Found "import" — extract the path segments
      (build.extract-import-path (cddr form) (list))))

(define (build.extract-import-path tokens acc)
  ;; Collect ident tokens, skipping "::" punct tokens,
  ;; until we hit ";" or end of form.
  (if (null? tokens)
      (reverse acc)
      (let* ((token (car tokens)))
        (if (not (pair? token))
            (reverse acc)
            (let ((tag (car token)))
              (cond
                ((eq? tag 'ident)
                 (build.extract-import-path (cdr tokens)
                   (cons (string->symbol (cadr token)) acc)))
                ((eq? tag 'punct)
                 (if (string=? (cadr token) ";")
                     (reverse acc)
                     (build.extract-import-path (cdr tokens) acc)))
                (else
                 (reverse acc))))))))

;; ── Resolve import path to source file path ──

(define (build.resolve-source-path import-path)
  ;; Convert (compiler io) → "compiler/io.lain"
  (let loop ((segments import-path) (acc ""))
    (if (null? segments)
        (string-append acc ".lain")
        (let* ((seg (symbol->string (car segments)))
               (new-acc (if (string=? acc "")
                            seg
                            (string-append acc "/" seg))))
          (loop (cdr segments) new-acc)))))
