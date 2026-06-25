# Lain

Lain is a native language experiment.

## Description

Lain consists of three layers：

1. `Lain IR`
    - `Lain IR` is a structred IR, similiar to C, but a bit lower.
2. `compile time evaluation`
    - `Lain IR` statements can be run in compile time.
3. `meta functions`
    - Scheme functions can be injected via hooks during compile time to manipulate AST

Lain trying to make the compiler as small as possible, and implement most highlevel feature during meta library.

## Lain IR

```Lain-IR
#proc add(#bits<32>, #bits<32>) -> #bits<32> {
  #let %ret = #integer.signed.add(#arg(0), #arg(1))
  #return %ret
}

#proc main() -> #bits<32> {
  #let %agg = #alloca(8)
  #store 10, #lea(base=%agg, offset=0)
  #store 20, #lea(base=%agg, offset=4)
  #let %a = #field[0](%agg)
  #let %b = #field[4](%agg)
  #let %sum = #call(add,%a, %b)
  #return %sum
}
```

## Build and Run

TBD
