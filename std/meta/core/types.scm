(meta-source "core/types")

(register-type-constructor! '|unit| 0 '|core-unit-type|)
(register-type-constructor! '|never| 0 '|core-never-type|)
(register-type-constructor! '|bits| 1 '|core-bits-type|)
(register-type-constructor! '|float| 1 '|core-float-type|)
(register-type-constructor! '|addr| 0 '(alias addr))
(register-string-literal-type! '|addr|)

(register-type-constructor! '|Ordering| 0 '(alias bits 8))
(register-memory-ordering-type! '|Ordering|)

(register-type-constructor! '|Array| 1 '(array))
(register-array-type! '|Array|)

(register-type-constructor! '|i8| 0 '(alias bits 8))
(register-type-constructor! '|i16| 0 '(alias bits 16))
(register-type-constructor! '|i32| 0 '(alias bits 32))
(register-type-constructor! '|i64| 0 '(alias bits 64))
(register-type-constructor! '|u8| 0 '(alias bits 8))
(register-type-constructor! '|u16| 0 '(alias bits 16))
(register-type-constructor! '|u32| 0 '(alias bits 32))
(register-type-constructor! '|u64| 0 '(alias bits 64))
(register-type-constructor! '|usize| 0 '(alias bits 64))
(register-type-constructor! '|bool| 0 '(alias bits 1))
(register-condition-type! '|bool|)
(register-type-constructor! '|f32| 0 '(alias float 32))
(register-type-constructor! '|f64| 0 '(alias float 64))

(register-type-constructor! '|Option| 1 '(nominal))
(register-type-constructor! '|Result| 2 '(nominal))

