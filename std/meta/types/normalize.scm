(meta-source "types/normalize")

;; ═══════════════════════════════════════════════════
;; 类型中间表示规范化
;; ═══════════════════════════════════════════════════

(define (middle.normalize-type raw-ty)
  (let* ((kind (raw.kind raw-ty))
         (payload (raw.payload raw-ty)))
    (cond
      ((symbol=? kind '|type.path|)
       (middle.node! '|types.path|
         (record '|types.path|
           (record.field '|name|
             (optional.value
               (record.get payload '|name|))))))
      ((symbol=? kind '|type.unit|)
       (middle.node! '|types.unit|
         (record '|types.unit|)))
      ((symbol=? kind '|type.app|)
       (middle.node! '|types.app|
         (record '|types.app|
           (record.field '|name|
             (optional.value
               (record.get payload '|name|)))
           (record.field '|args|
             (middle.normalize-types
               (optional.value
                 (record.get payload '|args|))
               (list))))))
      ((symbol=? kind '|type.raw-ptr|)
       (middle.node! '|types.raw-ptr|
         (record '|types.raw-ptr|
           (record.field '|mutable|
             (optional.value
               (record.get payload '|mutable|)))
           (record.field '|pointee|
             (middle.normalize-type
               (optional.value
                 (record.get payload '|pointee|)))))))
      ((symbol=? kind '|type.ref|)
       (middle.node! '|types.ref|
         (record '|types.ref|
           (record.field '|mutable|
             (optional.value
               (record.get payload '|mutable|)))
           (record.field '|inner|
             (middle.normalize-type
               (optional.value
                 (record.get payload '|inner|)))))))
      ((symbol=? kind '|type.slice|)
       (middle.node! '|types.slice|
         (record '|types.slice|
           (record.field '|element|
             (middle.normalize-type
               (optional.value
                 (record.get payload '|element|)))))))
      ((symbol=? kind '|type.array|)
       (middle.node! '|types.array|
         (record '|types.array|
           (record.field '|element|
             (middle.normalize-type
               (optional.value
                 (record.get payload '|element|))))
           (record.field '|len|
             (optional.value
               (record.get payload '|len|))))))
      ((symbol=? kind '|type.fn|)
       (middle.node! '|types.fn|
         (record '|types.fn|
           (record.field '|params|
             (middle.normalize-types
               (optional.value
                 (record.get payload '|params|))
               (list)))
           (record.field '|return|
             (middle.normalize-type
               (optional.value
                 (record.get payload '|return|))))
           (record.field '|effects|
             (optional.value
               (record.get payload '|effects|))))))
      (else
       (middle.node! '|types.unknown|
         (record '|types.unknown|
           (record.field '|raw-kind| kind)))))))

(define (middle.normalize-types raw-types acc)
  (if (list.empty? raw-types)
      (list.reverse acc)
      (let* ((ty (middle.normalize-type (list.first raw-types))))
        (middle.normalize-types
          (list.rest raw-types)
          (list.cons ty acc)))))
