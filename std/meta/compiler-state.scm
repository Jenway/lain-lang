;; ===========================================================================
;; compiler-state.scm — 编译器全局状态集中管理
;;
;; 本文件是唯一使用 define + set! 操作全局变量的地方。
;; 其他模块只通过 accessor/mutator 函数读写，不直接碰变量。
;; ===========================================================================

(meta-source "compiler-state")

;; ═══════════════════════════════════════════════════════════════════════════
;; 1. Compile-time constant table
;;    ((name type value) ...)
;;    写: core-lowerer |middle.let-binding| (fn/lower.scm)
;;    读: core-expr-lowerer |path.access| (expr/lower.scm)
;;    读: const.lookup (本文件)
;; ═══════════════════════════════════════════════════════════════════════════

(define *const-table* (list))

(define (const-table.register! name type val)
  (set! *const-table* (cons (list name type val) *const-table*)))

(define (const-table.lookup name)
  (let ((loop #f))
  (set! loop (lambda (table)
    (if (null? table)
        #f
        (let* ((entry (car table)))
          (if (symbol=? (car entry) name)
              entry
              (loop (cdr table)))))))
  (loop *const-table*)))

;; ═══════════════════════════════════════════════════════════════════════════
;; 2. Comptime function registry
;;    (symbol ...)
;;    写: core-declarer |middle.fn| (fn/lower.scm)
;;    读: core.call-or-eval (expr/lower.scm)
;; ═══════════════════════════════════════════════════════════════════════════

(define *comptime-fns* (list))

(define (comptime-fns.register! name)
  (if (not (comptime-fns.member? name))
      (set! *comptime-fns* (cons name *comptime-fns*))
      unit))

(define (comptime-fns.member? name)
  (let ((loop #f))
  (set! loop (lambda (fns)
    (if (null? fns) #f
        (if (symbol=? (car fns) name) #t
            (loop (cdr fns))))))
  (loop *comptime-fns*)))

;; ═══════════════════════════════════════════════════════════════════════════
;; 3. Error counter
;;    integer
;;    写: 各处 error handler
;;    读: compile (compile.scm)
;; ═══════════════════════════════════════════════════════════════════════════

(define *error-count* 0)

(define (errors.inc!)
  (set! *error-count* (+ *error-count* 1)))

(define (errors.reset!)
  (set! *error-count* 0))

(define (errors.total)
  *error-count*)

;; ═══════════════════════════════════════════════════════════════════════════
;; 4. Declaration accumulator
;;    (decl ...) — parsed raw declarations, consumed by normalize pass
;;    写: driver.parse-and-declare (pipeline/driver.scm)
;;    读: driver.normalize-decls-from (pipeline/driver.scm)
;; ═══════════════════════════════════════════════════════════════════════════

(define *declarations* (list))

(define (declarations.push! decl)
  (set! *declarations* (cons decl *declarations*)))

(define (declarations.all)
  *declarations*)

(define (declarations.reset!)
  (set! *declarations* (list)))

;; ═══════════════════════════════════════════════════════════════════════════
;; 5. Full reset — called at start of each compilation
;; ═══════════════════════════════════════════════════════════════════════════

(define (compiler-state.reset!)
  (set! *const-table* (list))
  (set! *comptime-fns* (list))
  (set! *error-count* 0)
  (set! *declarations* (list)))
