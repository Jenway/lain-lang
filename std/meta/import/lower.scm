(meta-source "import/lower")

;; ═══════════════════════════════════════════════════════════
;; Import core-declarer / core-lowerer (interface-based)
;; 
;; Step 7: Reads compiled interface artifacts instead of recursive compilation.
;; Directly calls host.new-extern for each export.
;; Registers both the mangled C name (for linking) and the
;; import-path-derived name (for source-level calls).
;; ═══════════════════════════════════════════════════════════

;; ── Path resolution ──

;; Convert module path (compiler lexer) → "compiler/lexer.lain"
(define (import.resolve-path path)
  (let ((loop #f))
  (set! loop (lambda (segments acc)
    (if (null? segments)
        (string-append acc ".lain")
        (let* ((seg (symbol->string (car segments)))
               (new-acc (if (string=? acc "")
                            seg
                            (string-append acc "/" seg))))
          (loop (cdr segments) new-acc)))))
  (loop path "")))

;; ── Type name → lowered type cpointer ──

;; Map L1 type names (addr, i32, u8, etc.) to lowered L1Type* cpointers
(define (interface.type-name->lowered ty-name)
  ;; Create a types.path node and lower it
  (core.lower-type
    (middle.node! '|types.path|
      (record '|types.path|
        (record.field '|name| ty-name)))))

;; ── Helper: walk interface S-expr and extract fn exports ──

;; Returns: ((export-name link-name (type...) ret-type) ...)
(define (interface.extract-exports parsed)
  ;; parsed = (module NAME (exports (fn ...) ...))
  (if (not (pair? parsed))
      (error "interface: invalid format")
      (let* ((module-tag (car parsed)))
        (if (not (eq? module-tag 'module))
            (error "interface: expected (module ...)")
            (let* ((rest (cdr parsed))
                   (exports-section (interface.find-exports-section rest)))
              (if (or (not (pair? exports-section))
                      (not (eq? (car exports-section) 'exports)))
                  (error "interface: missing (exports ...)")
                  ;; exports-section = (exports (fn ...) ...)
                  (interface.extract-fns (cdr exports-section) (list))))))))

(define (interface.find-exports-section items)
  (if (null? items)
      #f
      (let* ((item (car items)))
        (if (and (pair? item) (eq? (car item) 'exports))
            item
            (interface.find-exports-section (cdr items))))))

(define (interface.extract-fns entries acc)
  (if (null? entries)
      (reverse acc)
      (let* ((entry (car entries)))
        (if (and (pair? entry) (eq? (car entry) 'fn))
            ;; Accept both legacy shape:
            ;;   (fn NAME (TYPES) -> RET)
            ;; and new interface shape:
            ;;   (fn (name NAME) (params (...)) (ret RET) (link_name "..."))
            (let* ((fn-name (interface.fn-entry-name entry))
                   (link-name (interface.fn-entry-link-name entry))
                   (params (interface.fn-entry-params entry))
                   (ret-type (interface.fn-entry-ret entry)))
              (interface.extract-fns (cdr entries)
                (cons (list fn-name link-name params ret-type) acc)))
            (interface.extract-fns (cdr entries) acc)))))

(define (interface.extract-entry-names entries tag acc)
  (if (null? entries)
      (reverse acc)
      (let* ((entry (car entries)))
        (if (and (pair? entry) (eq? (car entry) tag))
            (let* ((name-field (interface.assoc-field 'name (cdr entry))))
              (if (and name-field (pair? (cdr name-field)))
                  (interface.extract-entry-names
                    (cdr entries)
                    tag
                    (cons (cadr name-field) acc))
                  (error "interface: malformed entry missing name")))
            (interface.extract-entry-names (cdr entries) tag acc)))))

(define (interface.assoc-field key fields)
  (if (null? fields)
      #f
      (let* ((field (car fields)))
        (if (and (pair? field) (eq? (car field) key))
            field
            (interface.assoc-field key (cdr fields))))))

(define (interface.fn-entry-name entry)
  (let* ((tail (cdr entry)))
    (if (and (pair? tail) (symbol? (car tail)))
        ;; Legacy format: (fn NAME ...)
        (car tail)
        ;; New format: look up (name NAME)
        (let* ((name-field (interface.assoc-field 'name tail)))
          (if (and name-field (pair? (cdr name-field)))
              (cadr name-field)
              (error "interface: malformed fn entry missing name"))))))

(define (interface.fn-entry-params entry)
  (let* ((tail (cdr entry)))
    (if (and (pair? tail) (symbol? (car tail)))
        ;; Legacy format: (fn NAME (TYPES) -> RET)
        (cadr tail)
        ;; New format: (params (...))
        (let* ((params-field (interface.assoc-field 'params tail)))
          (if (and params-field (pair? (cdr params-field)))
              (cadr params-field)
              (error "interface: malformed fn entry missing params"))))))

(define (interface.fn-entry-ret entry)
  (let* ((tail (cdr entry)))
    (if (and (pair? tail) (symbol? (car tail)))
        ;; Legacy format: (fn NAME (TYPES) -> RET)
        (let* ((legacy-tail (cdddr entry)))
          (if (and (pair? legacy-tail) (pair? (cdr legacy-tail))
                   (eq? (car legacy-tail) '->))
              (cadr legacy-tail)
              (error "interface: malformed fn entry")))
        ;; New format: (ret RET)
        (let* ((ret-field (interface.assoc-field 'ret tail)))
          (if (and ret-field (pair? (cdr ret-field)))
              (cadr ret-field)
              (error "interface: malformed fn entry missing ret"))))))

(define (interface.fn-entry-link-name entry)
  (let* ((tail (cdr entry)))
    (if (and (pair? tail) (symbol? (car tail)))
        ;; Legacy format has no separate link_name field
        (car tail)
        (let* ((link-field (interface.assoc-field 'link_name tail)))
          (if (and link-field (pair? (cdr link-field)))
              (string->symbol (cadr link-field))
              (interface.fn-entry-name entry))))))

;; ── Import binding registry ──

(define *import-binding-registry* (list))

(define (import.path-tail-symbol path)
  (let ((loop #f))
  (set! loop (lambda (segments acc)
    (if (null? segments)
        (string->symbol acc)
        (let* ((seg (symbol->string (car segments)))
               (new-acc (if (string=? acc "")
                            seg
                            (string-append acc "_" seg))))
          (loop (cdr segments) new-acc)))))
  (loop path "")))

(define (import.export-leaf-name mangled-name)
  ;; Returns the name as-is. The caller-side leaf lookup uses
  ;; import.path-tail-symbol which joins the path segments with "_",
  ;; matching the .lci name field convention (moduleprefix_fnname).
  mangled-name)

(define (import.binding-exports exports acc)
  (if (null? exports)
      (reverse acc)
      (let* ((entry (car exports))
             (export-name (car entry))
             (leaf-name (import.export-leaf-name export-name)))
        (import.binding-exports
          (cdr exports)
          (cons (cons leaf-name export-name) acc)))))

(define (import.register-binding! alias import-path exports)
  (set! *import-binding-registry*
    (cons (list alias
                import-path
                (import.binding-exports exports (list))
                (list)
                (list))
          *import-binding-registry*)))

(define (import.register-binding-metadata! alias import-path
                                           fn-exports module-exports signature-exports)
  (set! *import-binding-registry*
    (cons (list alias import-path fn-exports module-exports signature-exports)
          *import-binding-registry*)))

(define (import.binding-fn-exports binding)
  (caddr binding))

(define (import.binding-module-exports binding)
  (cadddr binding))

(define (import.binding-signature-exports binding)
  (car (cddddr binding)))

(define (import.lookup-binding alias)
  (let ((loop #f))
  (set! loop (lambda (entries)
    (if (null? entries)
        #f
        (let* ((entry (car entries))
               (entry-alias (car entry)))
          (if (symbol=? entry-alias alias)
              entry
              (loop (cdr entries)))))))
  (loop *import-binding-registry*)))

(define (import.lookup-export exports leaf-name)
  (if (null? exports)
      #f
      (let* ((entry (car exports)))
        (if (symbol=? (car entry) leaf-name)
            (cdr entry)
            (import.lookup-export (cdr exports) leaf-name)))))

(define (import.resolve-qualified-symbol path)
  (if (or (null? path) (null? (cdr path)))
      #f
      (let* ((binding (import.lookup-binding (car path))))
        (if (not binding)
            #f
            (let* ((exports (import.binding-fn-exports binding))
                   (leaf-name (import.path-tail-symbol (cdr path))))
              (import.lookup-export exports leaf-name))))))

(define (import.lookup-name names leaf-name)
  (if (null? names)
      #f
      (if (symbol=? (car names) leaf-name)
          (car names)
          (import.lookup-name (cdr names) leaf-name))))

(define (import.resolve-qualified-module-name path)
  (if (or (null? path) (null? (cdr path)))
      #f
      (let* ((binding (import.lookup-binding (car path))))
        (if (not binding)
            #f
            (let* ((leaf-name (import.path-tail-symbol (cdr path))))
              (import.lookup-name
                (import.binding-module-exports binding)
                leaf-name))))))

(define (import.resolve-qualified-signature-name path)
  (if (or (null? path) (null? (cdr path)))
      #f
      (let* ((binding (import.lookup-binding (car path))))
        (if (not binding)
            #f
            (let* ((leaf-name (import.path-tail-symbol (cdr path))))
              (import.lookup-name
                (import.binding-signature-exports binding)
                leaf-name))))))

;; ── core-declarer: read interface, declare extern functions directly ──

(define-pass* 'core-declarer '|middle.import| (lambda (item)
  (meta.ensure-static-position! '|middle.import|)
  (let* ((payload (middle.payload item))
         (name (optional.value (record.get payload '|name|)))
         ;; The raw import path is nested in the inner |payload| field
         (raw-inner (optional.value (record.get payload '|payload|)))
         (path (optional.value (record.get raw-inner '|path|))))
    ;; Read and parse the compiled interface file
    (let* ((source-path (import.resolve-path path))
           (parsed (host.read-interface source-path)))
      (if (not parsed)
          (error (string-append "import: interface not found for: " source-path))
          (let* ((rest (cdr parsed))
                 (exports-section (interface.find-exports-section rest))
                 (export-entries (if exports-section (cdr exports-section) (list)))
                 (exports (interface.extract-exports parsed))
                 (module-exports (interface.extract-entry-names export-entries 'module (list)))
                 (signature-exports (interface.extract-entry-names export-entries 'signature (list)))
                 (fn-exports (import.binding-exports exports (list))))
            ;; Directly declare each export as an extern function.
            ;; Also register with the import-path-derived name so
            ;; source-level calls like simple_math_add() resolve correctly.
            (import.register-binding-metadata!
              name path fn-exports module-exports signature-exports)
            (interface.declare-exports! exports path)))))))

;; Derive the caller-side function name from import path + mangled name.
;; The interface exports mangled names like "fs_read" (moduleprefix_functionname).
;; The caller expects names like "std_fs_read" (import path joined with _).
;; We strip the source module prefix (last path segment + "_") and replace it
;; with the full import path prefix.
;; If the export name doesn't start with the module prefix, we prepend the
;; path prefix to the full export name.
(define (import.caller-fn-name import-path mangled-name)
  (let* ((mangled-str (symbol->string mangled-name))
         (path-prefix (import.path-prefix-string import-path))
         ;; Source module prefix = last import-path segment + "_"
         (source-module-str (if (null? import-path)
                                ""
                                (symbol->string (import.path-last-segment import-path))))
         (source-prefix (if (string=? source-module-str "")
                            ""
                            (string-append source-module-str "_")))
         (source-prefix-len (string-length source-prefix)))
    (if (and (> source-prefix-len 0)
             (>= (string-length mangled-str) source-prefix-len)
             (string=? (substring mangled-str 0 source-prefix-len) source-prefix))
        ;; Export name starts with module prefix — strip it, use path prefix
        (let* ((base-name (substring mangled-str source-prefix-len
                                     (string-length mangled-str))))
          (string->symbol (string-append path-prefix base-name)))
        ;; Export name doesn't match — just prepend path prefix
        (string->symbol (string-append path-prefix mangled-str)))))

(define (import.path-last-segment path)
  (if (null? (cdr path))
      (car path)
      (import.path-last-segment (cdr path))))

(define (import.path-prefix-string path)
  ;; Convert import path (simple math) to "simple_math_"
  (let ((loop #f))
  (set! loop (lambda (segments acc)
    (if (null? segments)
        (if (string=? acc "") acc (string-append acc "_"))
        (let* ((seg (symbol->string (car segments)))
               (new-acc (if (string=? acc "")
                            seg
                            (string-append acc "_" seg))))
          (loop (cdr segments) new-acc)))))
  (loop path "")))

(define (import.string-last-index s ch)
  (let ((loop #f))
  (set! loop (lambda (i)
    (if (< i 0)
        #f
        (if (char=? (string-ref s i) ch)
            i
            (loop (- i 1))))))
  (loop (- (string-length s) 1))))

(define (import.string-first-index s ch)
  (let ((loop #f))
  (set! loop (lambda (i)
    (if (>= i (string-length s))
        #f
        (if (char=? (string-ref s i) ch)
            i
            (loop (+ i 1))))))
  (loop 0)))

(define (interface.declare-exports! exports import-path)
  (if (null? exports)
      unit
      (let* ((entry (car exports))
             (export-name (car entry))           ;; e.g., LocalCount
             (link-name (cadr entry))            ;; e.g., provider_count
             (param-type-names (caddr entry))    ;; e.g., (i32 i32)
             (ret-type-name (cadddr entry)))     ;; e.g., i32
        ;; Lower types to L1Type* cpointers
        (let* ((ret-ty (interface.type-name->lowered ret-type-name))
               (param-tys (interface.lower-param-types param-type-names))
               ;; Caller-side name derived from import path
               (caller-name (import.caller-fn-name import-path export-name)))
          ;; Register with caller-side name, link_name = actual C symbol
          ;; The C emitter now uses link_name for both forward decls and calls,
          ;; so the generated C will reference the correct mangled symbol.
          (host.new-extern
            caller-name            ;; source-level name (e.g., simple_math_add)
            (symbol->string link-name)  ;; C-level symbol (e.g., math_add)
            param-tys ret-ty)
          ;; Also register under the exported interface name itself.
          (host.new-extern
            export-name
            (symbol->string link-name)
            param-tys ret-ty))
        (interface.declare-exports! (cdr exports) import-path))))

(define (interface.lower-param-types type-names)
  (if (null? type-names)
      (list)
      (cons (interface.type-name->lowered (car type-names))
            (interface.lower-param-types (cdr type-names)))))

;; ── core-lowerer: no-op (imports produce declarations, not L1 code) ──

(define-pass* 'core-lowerer '|middle.import| (lambda (item)
  unit))
