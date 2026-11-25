; Lain 语法高亮查询。

; --- 注释与字面量 ---------------------------------------------------------
(line_comment) @comment

(string) @string
(interpolated_string) @string
(escape_sequence) @string.escape
(interpolation
  "{" @punctuation.special
  "}" @punctuation.special)

(number) @number
(boolean) @boolean

; --- 关键字 ---------------------------------------------------------------
[
  "let"
  "pub"
  "func"
  "struct"
  "enum"
  "module"
  "effect"
  "op"
  "perform"
  "handle"
  "with"
  "match"
  "use"
  "open"
  "extern"
  "comptime"
  "import"
  "for"
  "in"
  "loop"
  "as"
  "and"
  "or"
] @keyword

(if_expression
  "if" @keyword.conditional
  "else" @keyword.conditional)

(while_statement "while" @keyword.repeat)
(for_expression "for" @keyword.repeat)

(return_statement "return" @keyword.return)
(break_statement "break" @keyword)
(continue_statement "continue" @keyword)

(function_expression "func" @keyword.function)
(function_signature "func" @keyword.function)
(struct_expression "struct" @keyword)
(enum_expression "enum" @keyword)
(module_expression "module" @keyword)
(effect_expression "effect" @keyword)
(effect_operation "op" @keyword)
(sig_expression keyword: (path (identifier) @keyword))
(icall_expression "icall" @keyword)

; --- 声明名字 -------------------------------------------------------------
(let_declaration
  name: (identifier) @variable)

(let_declaration
  name: (identifier) @function
  value: (function_expression))

(let_declaration
  name: (identifier) @function
  signature: (function_signature))

(let_declaration
  name: (identifier) @type
  value: [
    (struct_expression)
    (enum_expression)
    (sig_expression)
  ])

(let_declaration
  name: (identifier) @module
  value: (module_expression))

(extern_declaration name: (identifier) @function)

; --- 参数与字段 -----------------------------------------------------------
(parameter
  name: (identifier) @variable.parameter
  type: (_) @type)

(loop_binding
  name: (identifier) @variable
  type: (_) @type)

(witness_parameters
  name: (identifier) @variable.parameter
  type: (_) @type)

(struct_field
  name: (identifier) @property
  type: (_) @type)

(field_initializer name: (identifier) @property)

(attribute
  "@" @attribute
  name: (identifier) @attribute)

; --- 类型 -----------------------------------------------------------------
(type_application name: (path (identifier) @type))
(function_type "Fn" @type.builtin)
(raw_function_type)
(effect_row (path (identifier) @type))

(function_expression return_type: (path (identifier) @type))
(function_signature return_type: (path (identifier) @type))

((identifier) @type.builtin
  (#match? @type.builtin "^(u8|u16|u32|u64|i8|i16|i32|i64|f32|f64|usize|bool|void|Type|Module|Effect|EffectRow|Never|c_int|c_uint|c_long|c_size|c_char)$"))

((identifier) @type.builtin
  (#match? @type.builtin "^(Ref|Mut|Slice|MutSlice|Array|Uninit)$"))

; --- 调用与成员 -----------------------------------------------------------
(import_expression (string) @string.special)

(call_expression
  function: (path (identifier) @function.call))

(field_expression field: (identifier) @property)

(member_splice_expression
  "." @punctuation.delimiter
  "[" @punctuation.bracket
  ":" @punctuation.delimiter
  "]" @punctuation.bracket)

; --- 模式匹配 -------------------------------------------------------------
(match_arm
  pattern: (variant_pattern
    variant: (path
      (identifier) @constructor)))

(variant_pattern variant: (path))

(wildcard_pattern) @variable.builtin

(handle_expression "with" @keyword)

; --- 中标点与运算符 -------------------------------------------------------
[
  "+"
  "-"
  "*"
  "/"
  "%"
  "=="
  "!="
  "<"
  "<="
  ">"
  ">="
  "<<"
  ">>"
  "&"
  "|"
  "^"
  "="
  "->"
  "=>"
  "!"
  "?"
] @operator

["(" ")" "[" "]" "{" "}"] @punctuation.bracket

["," ";" "." "::" ":"] @punctuation.delimiter
