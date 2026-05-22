(define (postfix-question operand)
  #{
    ,(operand)?
  })

(register-postfix-operator! "?" "postfix-question" postfix-question)
