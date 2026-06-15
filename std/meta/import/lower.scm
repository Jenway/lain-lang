(meta-source "import/lower")

;; ═══════════════════════════════════════════════════════════
;; Import core-declarer / core-lowerer (manifest-based)
;; 
;; Step 7: Reads .manifest files instead of recursive compilation.
;; Directly calls core.declare-extern-function! for each export.
;; Registers both the mangled C name (for linking) and the
;; import-path-derived name (for source-level calls).
;; ═══════════════════════════════════════════════════════════

;; ── Path resolution ──

;; Convert module path (compiler lexer) → "compiler/lexer.lain"
(define (import.resolve-path path)
  (let loop ((segments path) (acc ""))
    (if (null? segments)
        (string-append acc ".lain")
        (let* ((seg (symbol->string (car segments)))
               (new-acc (if (string=? acc "")
                            seg
                            (string-append acc "/" seg))))
          (loop (cdr segments) new-acc)))))

;; ── Type name → lowered type cpointer ──

;; Map L1 type names (addr, i32, u8, etc.) to lowered L1Type* cpointers
(define (manifest.type-name->lowered ty-name)
  ;; Create a middle.ty.path node and lower it
  (core.lower-type
    (middle.node! '|middle.ty.path|
      (record '|middle.ty.path|
        (record.field '|name| ty-name)))))

;; ── Helper: walk manifest S-expr and extract fn exports ──

;; Returns: ((fn-name (type...) . ret-type) ...)
(define (manifest.extract-exports parsed)
  ;; parsed = (module NAME (exports (fn ...) ...))
  (if (not (pair? parsed))
      (error "manifest: invalid format")
      (let* ((module-tag (car parsed)))
        (if (not (eq? module-tag 'module))
            (error "manifest: expected (module ...)")
            (let* ((rest (cdr parsed))
                   (exports-section (if (and (pair? rest) (pair? (cdr rest)))
                                       (cadr rest) #f)))
              (if (or (not (pair? exports-section))
                      (not (eq? (car exports-section) 'exports)))
                  (error "manifest: missing (exports ...)")
                  ;; exports-section = (exports (fn ...) ...)
                  (manifest.extract-fns (cdr exports-section) (list))))))))

(define (manifest.extract-fns entries acc)
  (if (null? entries)
      (reverse acc)
      (let* ((entry (car entries)))
        (if (and (pair? entry) (eq? (car entry) 'fn))
            ;; entry = (fn NAME (TYPES) -> RET)
            (let* ((fn-name (cadr entry))
                   (params (caddr entry))
                   ;; After params: (-> RET)
                   (tail (cdddr entry))
                   (ret-type (if (and (pair? tail) (pair? (cdr tail))
                                     (eq? (car tail) '->))
                                 (cadr tail)
                                 (error "manifest: malformed fn entry"))))
              (manifest.extract-fns (cdr entries)
                (cons (list fn-name params ret-type) acc)))
            (manifest.extract-fns (cdr entries) acc)))))

;; ── core-declarer: read manifest, declare extern functions directly ──

(define-pass (core-declarer |middle.import| item)
  (let* ((payload (middle.payload item))
         (name (optional.value (record.get payload '|name|)))
         ;; The raw import path is nested in the inner |payload| field
         (raw-inner (optional.value (record.get payload '|payload|)))
         (path (optional.value (record.get raw-inner '|path|))))
    ;; Read and parse the manifest file
    (let* ((source-path (import.resolve-path path))
           (parsed (core.read-manifest! source-path)))
      (if (not parsed)
          (error (string-append "import: manifest not found for: " source-path))
          (let ((exports (manifest.extract-exports parsed)))
            ;; Directly declare each export as an extern function.
            ;; Also register with the import-path-derived name so
            ;; source-level calls like simple_math_add() resolve correctly.
            (manifest.declare-exports! exports path))))))

;; Derive the caller-side function name from import path + mangled name.
;; The manifest exports mangled names like "math_add" (prefix from source file).
;; The caller expects names like "simple_math_add" (import path joined with _).
;; We register BOTH: name=caller_name, link_name=mangled_C_name.
;; This makes both simple_math_add(1, 2) and the actual C symbol math_add work.
(define (import.caller-fn-name import-path mangled-name)
  ;; Extract the basename of the mangled function (after the module prefix).
  ;; e.g., "math_add" → base = "add"
  ;; Then prepend the import path prefix: (simple math) → "simple_math_"
  (let* ((mangled-str (symbol->string mangled-name))
         (path-prefix (import.path-prefix-string import-path))
         ;; Find the FIRST underscore to split module prefix from function name.
         ;; Mangling format: moduleprefix_functionname (e.g., io_read_and_lex).
         ;; The function name may itself contain underscores, so we split at
         ;; the FIRST underscore, not the last.
         (underscore-idx (import.string-first-index mangled-str #\_)))
    (if (or (not underscore-idx) (= underscore-idx 0))
        ;; No underscore — just prepend path prefix
        (string->symbol (string-append path-prefix mangled-str))
        ;; Split: everything before first _ is the module prefix, after is the fn name
        (let* ((base-name (substring mangled-str (+ underscore-idx 1)
                                     (string-length mangled-str))))
          (string->symbol (string-append path-prefix base-name))))))

(define (import.path-prefix-string path)
  ;; Convert import path (simple math) to "simple_math_"
  (let loop ((segments path) (acc ""))
    (if (null? segments)
        (if (string=? acc "") acc (string-append acc "_"))
        (let* ((seg (symbol->string (car segments)))
               (new-acc (if (string=? acc "")
                            seg
                            (string-append acc "_" seg))))
          (loop (cdr segments) new-acc)))))

(define (import.string-last-index s ch)
  (let loop ((i (- (string-length s) 1)))
    (if (< i 0)
        #f
        (if (char=? (string-ref s i) ch)
            i
            (loop (- i 1))))))

(define (import.string-first-index s ch)
  (let loop ((i 0))
    (if (>= i (string-length s))
        #f
        (if (char=? (string-ref s i) ch)
            i
            (loop (+ i 1))))))

(define (manifest.declare-exports! exports import-path)
  (if (null? exports)
      unit
      (let* ((entry (car exports))
             (mangled-name (car entry))         ;; e.g., math_add
             (param-type-names (cadr entry))     ;; e.g., (i32 i32)
             (ret-type-name (caddr entry)))      ;; e.g., i32
        ;; Lower types to L1Type* cpointers
        (let* ((ret-ty (manifest.type-name->lowered ret-type-name))
               (param-tys (manifest.lower-param-types param-type-names))
               ;; Caller-side name derived from import path
               (caller-name (import.caller-fn-name import-path mangled-name)))
          ;; Register with caller-side name, link_name = actual C symbol
          ;; The C emitter now uses link_name for both forward decls and calls,
          ;; so the generated C will reference the correct mangled symbol.
          (core.declare-extern-function!
            caller-name            ;; source-level name (e.g., simple_math_add)
            (symbol->string mangled-name)  ;; C-level symbol (e.g., math_add)
            param-tys ret-ty)
          ;; Also register under the mangled name itself, for direct qualified calls
          (core.declare-extern-function!
            mangled-name           ;; mangled name (e.g., math_add)
            (symbol->string mangled-name)  ;; same as C symbol
            param-tys ret-ty))
        (manifest.declare-exports! (cdr exports) import-path))))

(define (manifest.lower-param-types type-names)
  (if (null? type-names)
      (list)
      (cons (manifest.type-name->lowered (car type-names))
            (manifest.lower-param-types (cdr type-names)))))

;; ── core-lowerer: no-op (imports produce declarations, not L1 code) ──

(define-pass (core-lowerer |middle.import| item)
  unit)
