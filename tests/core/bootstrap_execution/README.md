# Bootstrap execution closure

This suite compiles a minimal Lain-written constant lowering component to
LAIN-IR, then executes that component with the repository's LAIN-IR interpreter.
The input `value=42` must produce `42`.

This is the first execution closure for a Lain-written compiler component. It
does not yet dynamically construct a second LAIN-IR module: the source-level
Lain API currently models IR as values and does not expose the host IR-builder
capability to code running inside the interpreter.
