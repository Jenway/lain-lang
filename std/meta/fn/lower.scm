(meta-source "fn/lower")

(define (fn.attr-named-string args name fallback)
  (if (list.empty? args) fallback
      (let* ((arg (list.first args)) (kind (raw.kind arg)) (payload (raw.payload arg)))
        (if (symbol=? kind '|attr.arg.named|)
            (let* ((arg-name-path (optional.value (record.get payload '|name|))) (arg-name (list.first arg-name-path)))
              (if (symbol=? arg-name name)
                  (let* ((value (optional.value (record.get payload '|value|))))
                    (if (symbol=? (raw.kind value) '|attr.arg.string|)
                        (optional.value (record.get (raw.payload value) '|raw|)) fallback))
                  (fn.attr-named-string (list.rest args) name fallback)))
            (fn.attr-named-string (list.rest args) name fallback)))))

(define (fn.foreign-link-name attrs fallback)
  (if (list.empty? attrs) fallback
      (let* ((attr (list.first attrs)) (payload (raw.payload attr)) (attr-name (optional.value (record.get payload '|name|))))
        (if (symbol=? attr-name '|foreign|)
            (fn.attr-named-string (optional.value (record.get payload '|args|)) '|link_name| fallback)
            (fn.foreign-link-name (list.rest attrs) fallback)))))

(define (fn.cfg-target-os attrs)
  (if (list.empty? attrs) (optional.none)
      (let* ((attr (list.first attrs)) (payload (raw.payload attr)) (attr-name (optional.value (record.get payload '|name|))))
        (if (symbol=? attr-name '|cfg|)
            (optional.some (fn.attr-named-string (optional.value (record.get payload '|args|)) '|target_os| '|unknown|))
            (fn.cfg-target-os (list.rest attrs))))))

(define (fn.cfg-enabled? attrs)
  (let* ((target-os (fn.cfg-target-os attrs)))
    (if (optional.none? target-os) #t (symbol=? (optional.value target-os) (cfg.target-os)))))

(define-pass (core-declarer |middle.fn| item)
  (let* ((payload (middle.payload item)) (body (optional.value (record.get (middle.payload item) '|body|))))
    (if (optional.none? body) unit
        (let* ((name (optional.value (record.get payload '|name|)))
               (params (optional.value (record.get payload '|params|)))
               (ret (core.lower-type (optional.value (record.get payload '|return|))))
               (param-types (core.lower-param-types params (list))))
          (core.begin-function! name param-types ret)))))

(define-pass (core-declarer |middle.foreign-fn| item)
  (let* ((payload (middle.payload item)) (name (optional.value (record.get payload '|name|)))
         (attrs (optional.value (record.get payload '|attrs|)))
         (params (optional.value (record.get payload '|params|)))
         (ret (core.lower-type (optional.value (record.get payload '|return|))))
         (param-types (core.lower-param-types params (list))) (link-name (fn.foreign-link-name attrs name)))
    (if (fn.cfg-enabled? attrs) (core.declare-extern-function! name link-name param-types ret) unit)))

(define-pass (core-lowerer |middle.fn| item)
  (let* ((payload (middle.payload item)) (name (optional.value (record.get payload '|name|)))
         (params (optional.value (record.get payload '|params|)))
         (ret-ty (core.lower-type (optional.value (record.get payload '|return|))))
         (body (optional.value (record.get payload '|body|))))
    (if (optional.none? body) unit
        (let* ((function (core.function-by-name name)) (block (core.append-block! function))
               (locals (core.bind-params function params 0 (list)))
               (body-payload (middle.payload (optional.value body))))
          (core.lower-stmts block (optional.value (record.get body-payload '|items|)) ret-ty locals)))))

(define-pass (core-type-lowerer |middle.ty.fn| ty) (type.unsupported (middle.kind ty)))
