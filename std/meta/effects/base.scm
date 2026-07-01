(meta-source "effects/base")

(register-effect-ctor! '|IO| 0)
(register-effect-constructor! '|IO| 0 '|io-effect|)
(register-effect-ctor! '|Alloc| 0)
(register-effect-constructor! '|Alloc| 0 '|alloc-effect|)
(register-effect-ctor! '|RawPtrRead| 0)
(register-effect-constructor! '|RawPtrRead| 0 '|raw-ptr-read-effect|)
(register-effect-ctor! '|RawPtrWrite| 0)
(register-effect-constructor! '|RawPtrWrite| 0 '|raw-ptr-write-effect|)

(register-raw-pointer-effect! '|read| '|RawPtrRead|)
(register-raw-pointer-effect! '|write| '|RawPtrWrite|)

;; perform/resume/handle 解析已移入 surface/tree.scm 的 tree-lower-expr
