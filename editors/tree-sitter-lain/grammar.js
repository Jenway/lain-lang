module.exports = grammar({
  name: 'lain',

  extras: $ => [/\s/, $.line_comment],

  word: $ => $.identifier,

  conflicts: $ => [
    [$._statement, $._expression],
    [$._expression, $.match_arm],
    [$._expression, $.struct_literal],
    [$._expression, $.struct_literal, $.sig_expression],
  ],

  rules: {
    source_file: $ => repeat(choice($._statement, ';')),

    line_comment: $ => token(seq('//', /.*/)),

    identifier: $ => /[A-Za-z_][A-Za-z0-9_]*/,

    // 关键字在名字位置（字段名、参数名、声明名）也得能用：Lain 的 Reader 不设保留字，
    // 所以这些词在同形状的另一种解读里立得住时，必须让两种解读共用同一个词法单元。
    _name: $ => choice(
      $.identifier,
      'let', 'pub', 'func', 'struct', 'enum', 'module', 'effect', 'op',
      'perform', 'handle', 'with', 'match', 'if', 'else',
      'while', 'loop', 'for', 'in', 'return', 'break', 'continue', 'as',
      'import', 'extern', 'use', 'open', 'comptime', 'and', 'or',
      'true', 'false', 'Fn', 'icall',
    ),

    number: $ => token(choice(
      /0[xX][0-9a-fA-F_]+/,
      seq(/[0-9][0-9_]*/, '.', /[0-9][0-9_]*/, optional(seq(choice('e', 'E'), optional(choice('+', '-')), /[0-9]+/))),
      seq(/[0-9][0-9_]*/, optional(seq(choice('e', 'E'), optional(choice('+', '-')), /[0-9]+/))),
    )),

    boolean: $ => choice('true', 'false'),

    string: $ => token(seq('"', repeat(choice(/[^"\\\n]/, /\\./)), '"')),

    interpolated_string: $ => seq(
      token(seq('f', '"')),
      repeat(choice(
        $.escape_sequence,
        $.interpolation,
        /[^"\\{\n]/,
      )),
      token.immediate('"'),
    ),

    interpolation: $ => seq('{', $._expression, '}'),

    escape_sequence: $ => token.immediate(/\\./),

    // --- 语句 -----------------------------------------------------------------
    _statement: $ => choice(
      $.let_declaration,
      $.use_declaration,
      $.open_declaration,
      $.extern_declaration,
      $.attributed_statement,
      $.return_statement,
      $.break_statement,
      $.continue_statement,
      $.while_statement,
      $.block,
      $.expression_statement,
    ),

    let_declaration: $ => seq(
      optional('pub'),
      'let',
      field('name', $.identifier),
      optional(seq(':', field('type', $._type))),
      optional(seq(
        '=',
        choice(
          field('value', $._expression),
          field('signature', $.function_signature),
        ),
      )),
      ';',
    ),

    use_declaration: $ => seq('use', $._expression, ';'),

    open_declaration: $ => seq('open', $._expression, ';'),

    extern_declaration: $ => seq(
      'extern',
      field('name', $.identifier),
      $.parameters,
      optional(seq('->', field('return_type', $._type))),
      '=',
      $._expression,
      ';',
    ),

    attributed_statement: $ => seq(
      repeat1($.attribute),
      choice(
        $.let_declaration,
        $.use_declaration,
        $.extern_declaration,
      ),
    ),

    attribute: $ => seq(
      '@',
      field('name', $.identifier),
      optional($.attribute_arguments),
    ),

    attribute_arguments: $ => seq(
      '(',
      sep(',', $._expression),
      optional(','),
      ')',
    ),

    return_statement: $ => prec.right(seq('return', optional($._expression))),

    break_statement: $ => prec.right(seq('break', optional($._expression))),

    continue_statement: $ => prec.right(seq('continue', optional($._expression))),

    while_statement: $ => seq('while', field('condition', $._expression), field('body', $.block)),

    expression_statement: $ => $._expression,

    block: $ => seq('{', repeat(choice($._statement, ';')), '}'),

    // --- 表达式 ---------------------------------------------------------------
    _expression: $ => choice(
      $.path,
      $.number,
      $.string,
      $.interpolated_string,
      $.boolean,
      $.parenthesized_expression,
      $.block,
      $.unary_expression,
      $.binary_expression,
      $.assignment_expression,
      $.cast_expression,
      $.call_expression,
      $.index_expression,
      $.icall_expression,
      $.field_expression,
      $.member_splice_expression,
      $.witness_expression,
      $.array_literal,
      $.struct_literal,
      $.if_expression,
      $.match_expression,
      $.handle_expression,
      $.perform_expression,
      $.loop_expression,
      $.for_expression,
      $.comptime_expression,
      $.function_expression,
      $.struct_expression,
      $.enum_expression,
      $.sig_expression,
      $.module_expression,
      $.effect_expression,
      $.import_expression,
    ),

    parenthesized_expression: $ => seq('(', $._expression, ')'),

    unary_expression: $ => prec.left(13, seq(
      choice('-', '!', '*', '&'),
      $._expression,
    )),

    binary_expression: $ => choice(
      ...[
        [prec.left, 10, choice('*', '/', '%')],
        [prec.left, 9, choice('+', '-')],
        [prec.left, 8, choice('<<', '>>')],
        [prec.left, 7, choice('<', '<=', '>', '>=')],
        [prec.left, 6, choice('==', '!=')],
        [prec.left, 5, '&'],
        [prec.left, 5, '^'],
        [prec.left, 5, '|'],
        [prec.left, 4, 'and'],
        [prec.left, 3, 'or'],
        [prec.left, 2, '..'],
      ].map(([fn, p, op]) => fn(p, seq(
        field('left', $._expression),
        field('operator', op),
        field('right', $._expression),
      ))),
    ),

    assignment_expression: $ => prec.right(1, seq(
      field('left', $._expression),
      '=',
      field('right', $._expression),
    )),

    cast_expression: $ => prec.left(11, seq(
      field('value', $._expression),
      'as',
      field('type', $._type),
    )),

    call_expression: $ => prec.left(14, seq(
      field('function', $._expression),
      field('arguments', $.arguments),
    )),

    arguments: $ => seq('(', sep(',', $._expression), optional(','), ')'),

    index_expression: $ => prec.left(14, seq(
      field('value', $._expression),
      '[',
      field('index', $._expression),
      ']',
    )),

    icall_expression: $ => prec.left(15, seq(
      'icall',
      '[',
      field('signature', $.raw_function_type),
      ']',
    )),

    raw_function_type: $ => seq(
      '(',
      sep(',', $._type),
      optional(','),
      ')',
      '->',
      $._type,
    ),

    member_splice_expression: $ => prec.left(14, seq(
      field('value', $._expression),
      '.',
      '[',
      ':',
      field('member', $._expression),
      ']',
    )),

    field_expression: $ => prec.left(14, seq(
      field('value', $._expression),
      choice('.', '::'),
      field('field', $.identifier),
    )),

    witness_expression: $ => prec.left(14, seq(
      field('value', $._expression),
      field('witness', $.witness_arguments),
    )),

    witness_arguments: $ => seq(
      '?',
      '{',
      sep(',', seq(field('name', $.identifier), ':', $._expression)),
      optional(','),
      '}',
    ),

    array_literal: $ => seq(
      '[',
      choice(
        seq($._expression, ';', $._expression),
        seq(sep(',', $._expression), optional(',')),
      ),
      ']',
    ),

    struct_literal: $ => seq(
      field('type', choice($.path, $.field_expression)),
      '{',
      repeat(choice($.field_initializer, $._expression, ',')),
      '}',
    ),

    field_initializer: $ => seq(field('name', $._name), ':', $._expression),

    // --- 控制流 ---------------------------------------------------------------
    if_expression: $ => prec.right(seq(
      'if',
      field('condition', $._expression),
      field('consequence', $.block),
      optional(seq('else', field('alternative', choice($.block, $.if_expression)))),
    )),

    match_expression: $ => seq(
      'match',
      field('value', $._expression),
      '{',
      sep(',', $.match_arm),
      optional(','),
      '}',
    ),

    match_arm: $ => seq(
      field('pattern', choice($.wildcard_pattern, $.variant_pattern, $.literal_pattern)),
      '=>',
      field('body', choice($._expression, $.block)),
    ),

    literal_pattern: $ => choice($.number, $.string, $.boolean),

    wildcard_pattern: $ => '_',

    variant_pattern: $ => seq(
      field('variant', $.path),
      optional(seq('(', sep(',', choice($.identifier, $.wildcard_pattern)), ')')),
    ),

    handle_expression: $ => seq(
      'handle',
      field('value', $._expression),
      'with',
      '{',
      sep(',', $.match_arm),
      optional(','),
      '}',
    ),

    perform_expression: $ => prec.right(seq('perform', $._expression)),

    loop_expression: $ => seq(
      'loop',
      optional(seq(field('name', $.identifier), '(', sep(',', $.loop_binding), ')')),
      optional(seq('while', field('condition', $._expression))),
      field('body', $.block),
    ),

    loop_binding: $ => seq(
      field('name', $.identifier),
      ':',
      field('type', $._type),
      '=',
      field('value', $._expression),
    ),

    for_expression: $ => seq(
      'for',
      field('name', $.identifier),
      'in',
      field('iterable', $._expression),
      field('body', $.block),
    ),

    comptime_expression: $ => seq(
      'comptime',
      field('body', choice($.if_expression, $.for_expression)),
    ),

    import_expression: $ => prec.dynamic(1, seq(
      'import',
      '(',
      $._expression,
      ')',
    )),

    // --- 声明形式（全部也是表达式） --------------------------------------------
    function_expression: $ => prec.right(seq(
      'func',
      field('parameters', $.parameters),
      optional(field('witness', $.witness_parameters)),
      optional(field('effects', $.effect_row)),
      optional(seq('->', field('return_type', $._type))),
      field('body', $.block),
    )),

    function_signature: $ => seq(
      'func',
      field('parameters', $.parameters),
      optional(field('witness', $.witness_parameters)),
      optional(field('effects', $.effect_row)),
      optional(seq('->', field('return_type', $._type))),
    ),

    parameters: $ => seq('(', sep(',', $.parameter), optional(','), ')'),

    parameter: $ => seq(
      field('name', $._name),
      ':',
      field('type', $._type),
    ),

    witness_parameters: $ => seq(
      '?',
      '{',
      sep(',', seq(
        field('name', $.identifier),
        ':',
        field('type', $._type),
        optional(seq('=', field('value', $._expression))),
      )),
      optional(','),
      '}',
    ),

    effect_row: $ => seq('!', '{', sep(',', $._type), optional(','), '}'),

    struct_expression: $ => seq(
      'struct',
      '{',
      repeat($.struct_field),
      '}',
    ),

    struct_field: $ => seq(
      field('name', $._name),
      ':',
      field('type', $._type),
      optional(','),
    ),

    enum_expression: $ => seq(
      'enum',
      optional(seq('(', field('base', $._type), ')')),
      '{',
      repeat($.enum_variant),
      '}',
    ),

    enum_variant: $ => seq(
      field('name', $._name),
      optional(seq('(', sep(',', $._type), ')')),
      optional(seq('=', field('value', $._expression))),
      optional(','),
    ),

    sig_expression: $ => seq(
      field('keyword', $.path),
      field('body', $.sig_body),
    ),

    sig_body: $ => seq('{', $.let_declaration, repeat(choice($._statement, ';')), '}'),

    module_expression: $ => seq('module', '{', repeat(choice($._statement, ';')), '}'),

    effect_expression: $ => seq(
      'effect',
      '{',
      repeat($.effect_operation),
      '}',
    ),

    effect_operation: $ => seq(
      'op',
      field('name', $.identifier),
      $.parameters,
      '->',
      field('return_type', $._type),
      optional(';'),
    ),

    // --- 路径与类型 -----------------------------------------------------------
    path: $ => prec.left(1, seq(
      $.identifier,
      repeat(seq(choice('.', '::'), $.identifier)),
    )),

    _type: $ => choice(
      $.path,
      $.type_application,
      $.function_type,
      $.number,
    ),

    type_application: $ => prec(1, seq(
      field('name', $.path),
      '(',
      sep(',', $._type),
      optional(','),
      ')',
    )),

    function_type: $ => seq(
      'Fn',
      '(',
      '(',
      sep(',', $._type),
      optional(','),
      ')',
      '->',
      $._type,
      optional($.effect_row),
      ')',
    ),
  },
});

function sep(separator, rule) {
  return optional(seq(rule, repeat(seq(separator, rule))));
}




