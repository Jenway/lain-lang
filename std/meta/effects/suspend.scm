(meta-source "effects/suspend")

(register-effect-constructor! 'Suspend 1 'suspend-effect)
(register-effect-lowering-recipe! 'Suspend 'resume-state-machine-recipe)

