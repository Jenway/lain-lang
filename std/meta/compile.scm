;; ===========================================================================
;; std/meta/compile.scm — Pure compile entry point
;;
;; Takes source text, runs pipeline. Returns 0 on success.
;; use-new-parser flag: #f = old Scheme lexer, #t = Pratt Parser
;; ===========================================================================

(meta-source "compile")

(define use-new-parser #f)

(define (compile source-text output-path)
  (let* ((lex-proc (if use-new-parser parse-and-canonicalize meta.lex-source!))
         (forms (lex-proc source-text (string-length source-text))))
    (if (null? forms)
        (error "compile: empty or invalid source")
        (begin
          (compiler-state.reset!)
          (for-each driver.parse-and-declare forms)
          (let* ((all-decls (reverse (declarations.all)))
                 (all-middle (driver.normalize-decls-from all-decls))
                 (_ (driver.declare-core all-middle))
                 (_ (driver.lower-core all-middle)))
            (if (> (errors.total) 0)
                (error (string-append "compilation failed with "
                                      (number->string (errors.total))
                                      " error(s)"))
                0))))))
