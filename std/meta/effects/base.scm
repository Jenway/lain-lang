(meta-source "effects/base")

(register-effect-algebra! 'base-row-normalize 'row-normalize)
(register-effect-algebra! 'base-row-subsumes 'row-subsumes?)

(register-rule! 'lower-effect-row 'lower-effect-row)
(define-rule! 'lower-effect-row
  '(rule (row)
     (if (effect.row-empty? row)
       (effect.empty-row)
       (lower-effect-list (effect.row-effects row)))))

(register-rule! 'lower-effect-list 'lower-effect-list)
(define-rule! 'lower-effect-list
  '(rule (effects)
     (if (list.empty? effects)
       (effect.empty-row)
       (begin
         (lower-effect (list.first effects))
         (lower-effect-list (list.rest effects))))))

(register-rule! 'lower-effect 'lower-effect)
(define-rule! 'lower-effect
  '(rule (effect)
     (let ((name (effect.name effect)))
       (if (registry.effect-lowering-registered? name)
         unit
         (diagnostic.error
           "missing std/meta effect lowering recipe")))))

(register-effect-constructor! 'IO 0 'io-effect)
(register-effect-constructor! 'Alloc 0 'alloc-effect)
(register-effect-constructor! 'RawPtrRead 0 'raw-ptr-read-effect)
(register-effect-constructor! 'RawPtrWrite 0 'raw-ptr-write-effect)

(register-raw-pointer-effect! read 'RawPtrRead)
(register-raw-pointer-effect! write 'RawPtrWrite)
