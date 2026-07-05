(meta-source "effects/propagate")

;; ═══════════════════════════════════════════════════════════
;; Effect Propagation — collects effects during lowering
;; and validates against declared effects at fn boundaries.
;; ═══════════════════════════════════════════════════════════

;; ── Global mutable effect tracker ──
;; Each fn lowering clears this, then perform/call lowerers
;; add to it. At fn end, we compare with declared effects.

(define *propagate-current-effects* '())

;; ── Function effect registry: maps fn-name → (effect-name ...) ──
(define *fn-effect-table* '())

(define (propagate.register-fn-effects! fn-name effect-names)
  (let* ((existing (effect.assoc fn-name *fn-effect-table*)))
    (if existing
        (set-cdr! existing effect-names)
        (set! *fn-effect-table*
              (cons (cons fn-name effect-names) *fn-effect-table*)))))

(define (propagate.lookup-fn-effects fn-name)
  (let* ((entry (effect.assoc fn-name *fn-effect-table*)))
    (if entry (cdr entry) '())))

(define (propagate.clear!)
  (set! *propagate-current-effects* '()))

(define (propagate.record! effect-name)
  (let ((loop #f))
  (set! loop (lambda (remaining)
    (if (null? remaining)
        (set! *propagate-current-effects*
              (cons effect-name *propagate-current-effects*))
        (if (eq? (car remaining) effect-name)
            #f  ;; already present
            (loop (cdr remaining))))))
  (loop *propagate-current-effects*)))

(define (propagate.record-effects! effect-names)
  "Record multiple effects at once."
  (let ((loop #f))
  (set! loop (lambda (remaining)
    (if (not (null? remaining))
        (begin
          (propagate.record! (car remaining))
          (loop (cdr remaining))))))
  (loop effect-names)))

(define (propagate.consume! effect-name)
  "Remove an effect from the collected set (used by handle to absorb effects)."
  (set! *propagate-current-effects*
        (let ((filter #f))
  (set! filter (lambda (remaining acc)
          (if (null? remaining)
              (reverse acc)
              (let ((eff (car remaining)))
                (if (eq? eff effect-name)
                    (filter (cdr remaining) acc)
                    (filter (cdr remaining) (cons eff acc)))))))
  (filter *propagate-current-effects* '()))))

(define (propagate.collected-effects)
  *propagate-current-effects*)

;; ── Validation: check collected vs declared ──
(define (propagate.validate-collected! collected declared-names fn-name)
  "Check that all collected effects are present in declared-names.
   If not, signal an error."
  (let ((check #f))
  (set! check (lambda (remaining)
    (if (null? remaining)
        #f  ;; all good
        (let* ((eff (car remaining))
               (found (propagate.name-in-list? eff declared-names)))
          (if found
              (check (cdr remaining))
              (error (string-append "effect '"
                                    (symbol->string eff)
                                    "' performed in function '"
                                    (symbol->string fn-name)
                                    "' but not declared in signature")))))))
  (check collected)))

;; ── Legacy validate (kept for backward compat) ──
(define (propagate.validate! declared-effects)
  (let* ((decl-list (optional.value declared-effects))
         (decl-names (propagate.extract-names decl-list)))
    (propagate.validate-collected! *propagate-current-effects* decl-names '|unknown|)))

;; ── Helpers ──

(define (propagate.effects-to-string effects)
  (if (null? effects)
      ""
      (let* ((first (symbol->string (car effects)))
             (rest (propagate.effects-to-string (cdr effects))))
        (if (string=? rest "")
            first
            (string-append first ", " rest)))))

(define (propagate.name-in-list? name lst)
  (if (null? lst)
      #f
      (if (eq? name (car lst))
          #t
          (propagate.name-in-list? name (cdr lst)))))

(define (propagate.extract-names decl-list)
  "Extract effect names from a list of raw effect-name nodes.
   Each node: (effect-name NAME ARGS)"
  (if (null? decl-list)
      '()
      (let* ((first (car decl-list))
             (payload (raw.payload first))
             (name (optional.value (record.get payload '|name|))))
        (cons name (propagate.extract-names (cdr decl-list))))))
