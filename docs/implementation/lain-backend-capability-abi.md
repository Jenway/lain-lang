# Lain backend capability ABI v1

This document defines the host boundary required by
`src/lainc/backend_c.lain`. It is a migration contract; it does not make the
current backend source part of the active compiler closure.

The backend receives canonical LAIN-IR source through a read-only source
capability and writes the generated C artifact through an artifact capability.
The backend must not call C symbols by spelling a host link name in source.
The provider or native driver binds these logical capability names to its own
implementation.

## Capability set

| Capability | Signature | Permission and lifetime |
| --- | --- | --- |
| `backend.source_count` | `() -> usize` | Read-only request-scoped source set |
| `backend.source_data` | `(usize) -> addr` | Borrowed pointer, valid until request end |
| `backend.source_length` | `(usize) -> usize` | Read-only request-scoped source set |
| `backend.allocate` | `(usize) -> addr` | Request-owned allocation; released with request |
| `backend.copy_bytes` | `(addr, addr, usize) -> unit` | Copies between request-owned/read-only buffers |
| `backend.artifact_begin` | `() -> unit` | Starts one artifact stream |
| `backend.artifact_write_byte` | `(i32) -> unit` | Appends one byte to the active stream |
| `backend.artifact_finish` | `() -> unit` | Closes the stream exactly once |

All integer widths are physical ABI types. `usize` is the compiler's address
width and `addr` is an opaque physical address. A provider must reject an
invalid source index, out-of-range copy, second `artifact_begin`, or write
after `artifact_finish` with a capability diagnostic; it must not silently
clamp or fabricate a value.

## Binding rule

The Lain source declares ordinary external procedures through the compiler's
capability declaration mechanism. A declaration carries the logical name
above; link names such as `bootstrap.allocate-pages` remain provider-side
details. The generated LAINIR contains `#extern #proc` declarations using the
logical names and records the capability set in its manifest.

The manifest must contain:

```json
{
  "schema": "lain-backend-capabilities-v1",
  "capabilities": [
    "backend.allocate",
    "backend.artifact_begin",
    "backend.artifact_finish",
    "backend.artifact_write_byte",
    "backend.copy_bytes",
    "backend.source_count",
    "backend.source_data",
    "backend.source_length"
  ]
}
```

The provider owns the mapping from these names to native functions. The
compiler core only validates declaration shape and capability presence; it
does not know the native function table.

## Migration gates

The source-level legacy inventory is checked independently with:

```text
python scripts/check_lain_backend_abi.py --report
```

It currently exits with failure while the seven `@foreign` declarations are
present. After migration it becomes the first zero-legacy gate.

1. Compile `backend_c.lain` with the active compiler and emit only logical
   `backend.*` externs.
2. Verify the artifact and manifest with the seed verifier.
3. Run the backend against a multi-procedure fixture and compare canonical C
   output with the historical driver.
4. Build and run the native in-process smoke, including capability rejection
   for a missing capability and invalid artifact lifecycle.

Until gate 1 passes, `@foreign` declarations in `backend_c.lain` are legacy
input and must not be copied into new compiler or provider APIs.
