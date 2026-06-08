(meta-source "effects/spawn")

(register-effect-constructor! 'Spawn 1 'spawn-effect)
(register-effect-lowering-recipe! 'Spawn 'capability-dispatch-recipe)

