(meta-source "import/lower")

;; ═══════════════════════════════════════════════════════════
;; Import core-declarer / core-lowerer
;; 
;; Handles: reading imported .lain files, lexing, parsing
;; each form, and integrating their declarations into the
;; current compilation unit via *lain-declarations*.
;; ═══════════════════════════════════════════════════════════

;; ── Circular import guard ──

(define *import-in-progress* (list))

(define (import.already-imported? name)
  (let loop ((lst *import-in-progress*))
    (if (null? lst)
        #f
        (if (equal? (car lst) name)
            #t
            (loop (cdr lst))))))

;; ── Path resolution ──

;; Convert module path (compiler lexer) → "compiler/lexer.lain"
(define (import.resolve-path path)
  (let loop ((segments path) (acc ""))
    (if (null? segments)
        (string-append acc ".lain")
        (let* ((seg (symbol->string (car segments)))
               (new-acc (if (string=? acc "")
                            seg
                            (string-append acc "/" seg))))
          (loop (cdr segments) new-acc)))))

;; ── core-declarer: read file, lex, split forms, parse each ──
;;
;; Parses all forms in the imported file via driver.parse-and-declare.
;; Each form's declarations are pushed to *lain-declarations*.
;; The outer driver.collect-all-middle-items will pick them up,
;; normalize them, and trigger transitive imports naturally.

(define-pass (core-declarer |middle.import| item)
  (let* ((payload (middle.payload item))
         (name (optional.value (record.get payload '|name|)))
         ;; The raw import path is nested in the inner |payload| field
         (raw-inner (optional.value (record.get payload '|payload|)))
         (path (optional.value (record.get raw-inner '|path|))))
    ;; Skip if already being imported (circular dependency guard)
    (if (import.already-imported? name)
        unit
        (begin
          (set! *import-in-progress* (cons name *import-in-progress*))
          ;; C-side: read file → lex → split forms → return list of S-expressions
          (let* ((file-path (import.resolve-path path))
                 (forms (core.read-file-forms! file-path)))
            (if (not forms)
                (error (string-append "import: file not found: " file-path))
                ;; Parse each form — pushes declarations to *lain-declarations*
                (import.parse-forms forms)))
          (set! *import-in-progress* (cdr *import-in-progress*))))))

;; Iterate over form S-expressions, parse each one.
;; Skips import forms — those are handled by the outer driver after all
;; declarations are collected, not during recursive import processing.
(define (import.form-is-import? form)
  (and (pair? form)
       (pair? (cdr form))
       (pair? (cadr form))
       (equal? (car (cadr form)) 'import)))

(define (import.parse-forms forms)
  (if (null? forms)
      unit
      (let ((form (car forms)))
        (if (import.form-is-import? form)
            (import.parse-forms (cdr forms))
            (begin
              (driver.parse-and-declare form)
              (import.parse-forms (cdr forms)))))))

;; ── core-lowerer: no-op (imports produce declarations, not L1 code) ──

(define-pass (core-lowerer |middle.import| item)
  unit)
