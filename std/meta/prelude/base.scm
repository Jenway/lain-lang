(meta-source "prelude/base")

;; ═══════════════════════════════════════════════════
;; 基础设施: define-pass, raw.*, middle.*, record, optional, list
;; 所有功能模块的第一个依赖
;; ═══════════════════════════════════════════════════

;; ===========================================================================
;; std/meta/core/prelude.scm — 元编译器预置桩函数
;;
;; 所有 register-* 桩在此显式定义，替代 C 侧 pre_declare_variables 的文本扫描。
;; ===========================================================================

(define (register-type-constructor! . args) #f)
(define (register-string-literal-type! . args) #f)
(define (register-memory-ordering-type! . args) #f)
(define (register-array-type! . args) #f)
(define (register-condition-type! . args) #f)

(define (register-raw-pointer-type! . args) #f)
(define (register-raw-pointer-index-type! . args) #f)
(define (register-raw-pointer-effect! . args) #f)

(define (register-effect-constructor! . args) #f)

(define (register-operator! . args) #f)
(define (register-expression-macro! . args) #f)

(define (register-constraint! . args) #f)
(define (register-interface-rule! . args) #f)


(meta-source "core/list")

(define (list.map items f)
  (let loop ((rest items) (acc (list)))
    (if (list.empty? rest)
        (list.reverse acc)
        (loop (list.rest rest)
              (list.cons (f (list.first rest)) acc)))))

(define (list.fold items seed f)
  (let loop ((rest items) (acc seed))
    (if (list.empty? rest)
        acc
        (loop (list.rest rest)
              (f acc (list.first rest))))))

(define (list.find items predicate)
  (let loop ((rest items))
    (if (list.empty? rest)
        (optional.none)
        (let* ((item (list.first rest)))
          (if (predicate item)
              (optional.some item)
              (loop (list.rest rest)))))))


(meta-source "core/record")

(define (record.make kind fields)
  (apply record (list.cons kind fields)))

(define (record.required payload name)
  (optional.value (record.get payload name)))

(define (record.optional payload name)
  (record.get payload name))

