;; ===========================================================================
;; std/meta/compile.scm — Pure compile entry point
;;
;; Takes source text, runs pipeline. Returns 0 on success.
;; ===========================================================================

(meta-source "compile")

(define (compile source-text output-path)
  (let* ((forms (meta.lex-source! source-text (string-length source-text))))
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
