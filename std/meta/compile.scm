;; ===========================================================================
;; std/meta/compile.scm — Pure compile entry point
;;
;; Takes source text (read by C), lexes it in pure Scheme, runs pipeline.
;; Returns 0 on success, signals error on failure.
;;
;; Feature flag: use-new-parser
;;   #f (default) — 使用旧 Scheme lexer (meta.lex-source!)
;;   #t           — 使用新 C Pratt Parser + canonicalize.scm
;; ===========================================================================

(meta-source "compile")

(define use-new-parser #t)

(define (lex-source src len)
  (if use-new-parser
      (parse-and-canonicalize src len)
      (meta.lex-source! src len)))

(define (compile source-text output-path)
  ;; 1. Lex source text → list of form trees
  (let* ((forms (lex-source source-text (string-length source-text))))
    (if (null? forms)
        (error "compile: empty or invalid source")
        (begin
          ;; 2. Reset pipeline state
          (compiler-state.reset!)
          ;; 3. Parse all forms
          (for-each driver.parse-and-declare forms)
          ;; 4. Run full pipeline (normalize → declare → lower)
          (let* ((all-decls (reverse (declarations.all)))
                 (all-middle (driver.normalize-decls-from all-decls))
                 (_ (driver.declare-core all-middle))
                 (_ (driver.lower-core all-middle)))
            ;; 5. Check errors
            (if (> (errors.total) 0)
                (error (string-append "compilation failed with "
                                      (number->string (errors.total))
                                      " error(s)"))
                0))))))
