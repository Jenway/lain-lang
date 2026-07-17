(meta-source "import/parse")

;; Join a path (list of symbols) into a single symbol using "::" separator.
;; e.g., (std dynarray) -> std::dynarray
(define (import.path-name path)
  (let ((strs (import.path-name-strings path (list))))
    (string->symbol (import.string-join strs "::"))))

(define (import.path-name-strings path acc)
  (if (list.empty? path)
      (list.reverse acc)
      (import.path-name-strings
        (list.rest path)
        (list.cons (symbol->string (list.first path)) acc))))

(define (import.string-join strs sep)
  (if (list.empty? strs)
      ""
      (if (list.empty? (list.rest strs))
          (list.first strs)
          (string-append (list.first strs)
                         sep
                         (import.string-join (list.rest strs) sep)))))

;; Extract symbol list from a :: path tree node.
;; Uses tree.flatten-path to get list of ident nodes, then extracts symbols.
(define (import.parse-path-tree path-node)
  (let ((nodes (tree.flatten-path path-node)))
    (import.extract-ident-syms nodes (list))))

(define (import.extract-ident-syms nodes acc)
  (if (list.empty? nodes)
      (list.reverse acc)
      (import.extract-ident-syms
        (list.rest nodes)
        (list.cons (tree.ident-sym (list.first nodes)) acc))))

(define-pass* 'form-parser '|import| (lambda (form)
  (let* ((tree (form.tree form))
         (parts (tree.flatten-juxt tree))
         ;; parts: ((ident import) path-node)
         ;; skip the keyword, take the path
         (path-node (list.first (list.rest parts)))
         (path (import.parse-path-tree path-node))
         (attrs (form.decorators form))
         (node (raw.node! '|import|
                 (record '|import|
                   (record.field '|attrs| attrs)
                   (record.field '|path| path)))))
    (decl.define! '|import| (import.path-name path) node)
    (interface.predeclare-path-types! path))))

(define-pass* 'raw-normalizer '|import| (lambda (decl)
  (middle.normalize-plain-decl decl '|middle.import|)))
