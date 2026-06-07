(meta-source "effects/throws")

(register-type-constructor! 'ThrowsCarrier 1 'throws-carrier-type)
(register-effect-constructor! 'Throws 1 'throws-effect)
(register-effect-lowering-recipe! 'Throws 'early-exit-result-recipe)
