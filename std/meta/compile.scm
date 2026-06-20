;; ===========================================================================
;; std/meta/compile.scm — Pure compile entry point
;;
;; Takes source text (read by C), lexes it in pure Scheme, runs pipeline.
;; Returns 0 on success, signals error on failure.
;; ===========================================================================

(meta-source "compile")

(define (compile source-text output-path)
  ;; 1. Lex source text → list of form trees
  (let* ((forms (meta.lex-source! source-text (string-length source-text))))
    (if (null? forms)
        (error "compile: empty or invalid source")
        (begin
          ;; 2. Reset pipeline state
          (set! *lain-declarations* (list))
          (set! *error-count* 0)
          ;; 3. Parse all forms
          (for-each driver.parse-and-declare forms)
          ;; 4. Run full pipeline (normalize → declare → lower)
          (let* ((all-decls (reverse *lain-declarations*))
                 (all-middle (driver.normalize-decls-from all-decls))
                 (_ (driver.declare-core all-middle))
                 (_ (driver.lower-core all-middle)))
            ;; 5. Check errors
            (if (> *error-count* 0)
                (error (string-append "compilation failed with "
                                      (number->string *error-count*)
                                      " error(s)"))
                0))))))
