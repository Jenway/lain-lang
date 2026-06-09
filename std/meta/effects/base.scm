(meta-source "effects/base")

(register-effect-constructor! '|IO| 0 '|io-effect|)
(register-effect-constructor! '|Alloc| 0 '|alloc-effect|)
(register-effect-constructor! '|RawPtrRead| 0 '|raw-ptr-read-effect|)
(register-effect-constructor! '|RawPtrWrite| 0 '|raw-ptr-write-effect|)

(register-raw-pointer-effect! '|read| '|RawPtrRead|)
(register-raw-pointer-effect! '|write| '|RawPtrWrite|)

