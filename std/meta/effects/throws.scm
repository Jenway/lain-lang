(meta-source "effects/throws")

(register-type-ctor! '|ThrowsCarrier| 1)
(register-type-constructor! '|ThrowsCarrier| 1 '|throws-carrier-type|)
(register-effect-ctor! '|Throws| 1)
(register-effect-constructor! '|Throws| 1 '|throws-effect|)

