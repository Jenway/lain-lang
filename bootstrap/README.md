# Bootstrap boundary

`main` contains the Lain-owned compiler.  The Scheme frontend is maintained on
the `bootstrap/stage0` branch and is checked out as a sibling worktree:

```text
D:\codeFiles\lain-lang       main
D:\codeFiles\lain-bootstrap  bootstrap/stage0
```

Create or refresh the local bootstrap tools with:

```text
git worktree add ..\lain-bootstrap bootstrap/stage0
Set-Location ..\lain-bootstrap
zig build
zig build --prefix ..\lain-lang\.bootstrap
```

On Windows the resulting `.bootstrap/bin` contains `lainc.exe`, `l1i.exe`,
`l1check.exe`, and the stage-2 compiler seed.  `.bootstrap` is local and
ignored by Git.

The main runtime locates Scheme stage-0 sources through
`LAIN_BOOTSTRAP_ROOT`, or through the default sibling path
`../lain-bootstrap`.  This path is used only by explicit bootstrap and legacy
compatibility tests.  Normal `--emit-l1` and `--emit-workspace-l1` compilation
use the self-hosted artifact.

Release boundaries are immutable tags:

```text
v0.1.0-alpha.1
bootstrap-v0.1.0-alpha.1
```
