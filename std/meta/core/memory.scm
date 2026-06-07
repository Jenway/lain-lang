(meta-source "core/memory")

(register-type-constructor! 'Ptr 1 '(raw-ptr const))
(register-type-constructor! 'MutPtr 1 '(raw-ptr mut))
(register-type-constructor! 'Ref 1 'ref-type)
(register-type-constructor! 'MutRef 1 'mut-ref-type)
(register-type-constructor! 'Slice 1 'slice-type)

(register-raw-pointer-type! const 'Ptr)
(register-raw-pointer-type! mut 'MutPtr)
(register-raw-pointer-index-type! 'u64)

(register-intrinsic! 'load 'memory-load)
(register-intrinsic! 'store 'memory-store)
(register-intrinsic! 'raw-ptr-read 'raw-ptr-read)
(register-intrinsic! 'raw-ptr-write 'raw-ptr-write)
