;; Scheme host contract smoke.
;;
;; This file must not depend on a concrete Scheme implementation.
;; A candidate host passes this smoke if it can run the portable subset that
;; std/meta is allowed to use.

(define (fail name)
  (display "FAIL ")
  (display name)
  (newline)
  (exit 1))

(define (check name value)
  (if value
      #t
      (fail name)))

(define escaped '|middle.fn|)
(check "escaped-symbol-is-symbol" (symbol? escaped))
(check "escaped-symbol-equals-string->symbol"
       (equal? escaped (string->symbol "middle.fn")))

(define punct '|->|)
(check "punct-symbol" (equal? punct (string->symbol "->")))

(define (fact n)
  (letrec ((loop (lambda (i acc)
                   (if (= i 0)
                       acc
                       (loop (- i 1) (* acc i))))))
    (loop n 1)))
(check "letrec" (= (fact 5) 120))

(define pair-value (cons 'a (cons 'b '())))
(check "pairs" (and (pair? pair-value)
                    (equal? (car pair-value) 'a)
                    (equal? (cadr pair-value) 'b)))

(define and-or-value (or #f (and #t 42)))
(check "and-or" (= and-or-value 42))

(cond
 ((not (= (+ 1 2) 3)) (fail "arithmetic"))
 ((not (string? "lain")) (fail "strings"))
 (else #t))

(display "OK scheme-host-contract")
(newline)
