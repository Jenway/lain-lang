;; std/meta/build/sort.scm — Dependency graph construction and topological sort
;;
;; Given a root .lain source path, transitively scans imports,
;; builds a dependency graph (alist: path → import-paths),
;; and returns modules in topological order.
;;
;; Graph format: ((source-path . (import-path ...)) ...)

(meta-source "build/sort")

;; ── Build full dependency graph ──

(define (build.compute-order root-path)
  ;; Returns list of source paths in compilation order (deps first).
  ;; Graph is stored as: ((path . (dep-path ...)) ...)
  (let* ((graph (build.build-graph (list root-path) (list)))
         (sorted (build.topo-sort graph (list) (list))))
    ;; Return only the paths (strip graph structure)
    (map car sorted)))

;; ── Transitive graph construction (DFS) ──

(define (build.build-graph queue graph)
  ;; queue: paths to process
  ;; graph: alist of (path . deps) entries already processed
  (if (null? queue)
      graph
      (let* ((path (car queue))
             (rest-queue (cdr queue)))
        (if (build.graph-has? graph path)
            ;; Already processed — skip
            (build.build-graph rest-queue graph)
            ;; Scan this file for imports
            (let* ((import-paths (build.scan-imports path))
                   (source-paths (build.resolve-all import-paths (list)))
                   (entry (cons path source-paths))
                   (new-graph (cons entry graph))
                   ;; Add new dependencies to the queue
                   (new-queue (build.add-new-deps rest-queue source-paths graph)))
              (build.build-graph new-queue new-graph))))))

(define (build.graph-has? graph path)
  (if (null? graph)
      #f
      (if (string=? (caar graph) path)
          #t
          (build.graph-has? (cdr graph) path))))

(define (build.resolve-all import-paths acc)
  (if (null? import-paths)
      (reverse acc)
      (build.resolve-all (cdr import-paths)
        (cons (build.resolve-source-path (car import-paths)) acc))))

(define (build.add-new-deps queue new-paths graph)
  (if (null? new-paths)
      queue
      (let* ((p (car new-paths))
             (rest (cdr new-paths)))
        (if (or (build.graph-has? graph p)
                (build.queue-has? queue p))
            (build.add-new-deps queue rest graph)
            (build.add-new-deps (append queue (list p)) rest graph)))))

(define (build.queue-has? queue path)
  (if (null? queue)
      #f
      (if (string=? (car queue) path)
          #t
          (build.queue-has? (cdr queue) path))))

;; ── Topological sort (DFS-based, Kahn-style for simplicity) ──
;;
;; Since we're in Scheme without hash tables, use a simple
;; Kahn's algorithm: repeatedly find nodes with no remaining
;; incoming edges and remove them.

(define (build.topo-sort graph result visited)
  ;; graph: ((path . deps) ...)
  ;; result: sorted paths (built in reverse)
  ;; visited: paths already emitted
  (if (null? graph)
      (reverse result)
      ;; Find a node with all deps already in visited
      (let ((next (build.find-ready graph visited)))
        (if (not next)
            (error "build: circular dependency detected")
            (build.topo-sort
              (build.remove-node graph next)
              (cons next result)
              (cons next visited))))))

(define (build.find-ready graph visited)
  (if (null? graph)
      #f
      (let* ((entry (car graph))
             (path (car entry))
             (deps (cdr entry)))
        (if (build.all-visited? deps visited)
            path
            (build.find-ready (cdr graph) visited)))))

(define (build.all-visited? deps visited)
  (if (null? deps)
      #t
      (if (build.string-in-list? (car deps) visited)
          (build.all-visited? (cdr deps) visited)
          #f)))

(define (build.string-in-list? s lst)
  (if (null? lst)
      #f
      (if (string=? s (car lst))
          #t
          (build.string-in-list? s (cdr lst)))))

(define (build.remove-node graph path)
  (if (null? graph)
      (list)
      (if (string=? (caar graph) path)
          (cdr graph)
          (cons (car graph) (build.remove-node (cdr graph) path)))))

;; ── Entry point for CLI ──
;;
;; Called from C to compute the build order and return
;; a list of source paths as strings.

(define (build.compute-and-store! root-path)
  ;; Compute build order and return as a list
  (build.compute-order root-path))
