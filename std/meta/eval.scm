(meta-source "eval")

;; Compile-time-required positions in the current language design.
;; These constructs must resolve during compilation even though the
;; executed code is still ordinary lain lowered through the same pipeline.

(define *meta-static-position-kinds*
  (list '|middle.import|
        '|middle.module|
        '|middle.signature|
        '|middle.export|))

(define (meta.static-position? kind)
  (if (list.empty? *meta-static-position-kinds*)
      #f
      (list.member? *meta-static-position-kinds* kind)))

(define (meta.register-static-position! kind)
  (if (meta.static-position? kind)
      unit
      (set! *meta-static-position-kinds*
        (list.cons kind *meta-static-position-kinds*))))

(define (meta.ensure-static-position! kind)
  (if (meta.static-position? kind)
      unit
      (error (string-append "compile-time value required at: "
                            (symbol->string kind)))))

;; Entry point for future lainir execution during compilation.
;; Current implementation only defines the boundary. Specific entry names
;; can be exposed by later compiler work without changing meta call sites.
(define (meta.execute-lainir! entry args)
  (core.execute-lainir! entry args))

(define (meta.require-static-value! position entry args)
  (meta.ensure-static-position! position)
  (meta.execute-lainir! entry args))
