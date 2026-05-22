use crate::diag::{Diagnostic, Label};
use crate::source::{FileId, SourceFile, Span};
use crate::symbol::{Interner, Symbol};

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum TokenKind {
    Fn,
    Struct,
    Enum,
    Effect,
    Handle,
    With,
    Let,
    Mut,
    Const,
    Repr,
    CAbi,
    Return,
    If,
    Else,
    Match,
    While,
    Loop,
    Break,
    Continue,
    Unsafe,
    Null,
    Import,
    Extern,
    True,
    False,
    Ident(Symbol),
    Int(u64),
    Float(f64),
    String(Symbol),
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    At,
    Hash,
    Comma,
    Colon,
    Semi,
    Dot,
    Plus,
    Minus,
    Star,
    Slash,
    Amp,
    Bang,
    Question,
    Assign,
    EqEq,
    FatArrow,
    NotEq,
    Less,
    LessEq,
    Greater,
    GreaterEq,
    Arrow,
    DoubleColon,
    Eof,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Token {
    pub kind: TokenKind,
    pub span: Span,
}

#[derive(Debug)]
pub struct Lexer<'a> {
    interner: &'a mut Interner,
}

impl<'a> Lexer<'a> {
    pub fn new(interner: &'a mut Interner) -> Self {
        Self { interner }
    }

    pub fn lex_file(&mut self, file: &SourceFile) -> Result<Vec<Token>, Vec<Diagnostic>> {
        let mut cursor = Cursor::new(file.id(), file.text());
        let mut tokens = Vec::new();
        let mut diagnostics = Vec::new();
        let mut next_ident_is_attribute_name = false;

        while let Some(byte) = cursor.peek() {
            if is_whitespace(byte) {
                cursor.bump();
                continue;
            }

            if byte == b'/' && cursor.peek_next() == Some(b'/') {
                cursor.bump();
                cursor.bump();
                while let Some(next) = cursor.peek() {
                    cursor.bump();
                    if next == b'\n' {
                        break;
                    }
                }
                continue;
            }

            let start = cursor.offset;
            let token = match byte {
                b'a'..=b'z' | b'A'..=b'Z' | b'_' => {
                    self.lex_ident_or_keyword(&mut cursor, next_ident_is_attribute_name)
                }
                b'"' => match self.lex_string(&mut cursor) {
                    Ok(token) => token,
                    Err(diagnostic) => {
                        diagnostics.push(diagnostic);
                        next_ident_is_attribute_name = false;
                        continue;
                    }
                },
                b'0'..=b'9' => match self.lex_number(&mut cursor) {
                    Ok(token) => token,
                    Err(diagnostic) => {
                        diagnostics.push(diagnostic);
                        next_ident_is_attribute_name = false;
                        continue;
                    }
                },
                b'(' => single(&mut cursor, TokenKind::LParen),
                b')' => single(&mut cursor, TokenKind::RParen),
                b'{' => single(&mut cursor, TokenKind::LBrace),
                b'}' => single(&mut cursor, TokenKind::RBrace),
                b'[' => single(&mut cursor, TokenKind::LBracket),
                b']' => single(&mut cursor, TokenKind::RBracket),
                b'@' => single(&mut cursor, TokenKind::At),
                b'#' => single(&mut cursor, TokenKind::Hash),
                b',' => single(&mut cursor, TokenKind::Comma),
                b';' => single(&mut cursor, TokenKind::Semi),
                b'.' => single(&mut cursor, TokenKind::Dot),
                b'+' => single(&mut cursor, TokenKind::Plus),
                b'*' => single(&mut cursor, TokenKind::Star),
                b'/' => single(&mut cursor, TokenKind::Slash),
                b'&' => single(&mut cursor, TokenKind::Amp),
                b'?' => single(&mut cursor, TokenKind::Question),
                b':' => {
                    cursor.bump();
                    if cursor.peek() == Some(b':') {
                        cursor.bump();
                        token_with_span(file.id(), start, cursor.offset, TokenKind::DoubleColon)
                    } else {
                        token_with_span(file.id(), start, cursor.offset, TokenKind::Colon)
                    }
                }
                b'-' => {
                    cursor.bump();
                    if cursor.peek() == Some(b'>') {
                        cursor.bump();
                        token_with_span(file.id(), start, cursor.offset, TokenKind::Arrow)
                    } else {
                        token_with_span(file.id(), start, cursor.offset, TokenKind::Minus)
                    }
                }
                b'=' => {
                    cursor.bump();
                    if cursor.peek() == Some(b'=') {
                        cursor.bump();
                        token_with_span(file.id(), start, cursor.offset, TokenKind::EqEq)
                    } else if cursor.peek() == Some(b'>') {
                        cursor.bump();
                        token_with_span(file.id(), start, cursor.offset, TokenKind::FatArrow)
                    } else {
                        token_with_span(file.id(), start, cursor.offset, TokenKind::Assign)
                    }
                }
                b'!' => {
                    cursor.bump();
                    if cursor.peek() == Some(b'=') {
                        cursor.bump();
                        token_with_span(file.id(), start, cursor.offset, TokenKind::NotEq)
                    } else {
                        token_with_span(file.id(), start, cursor.offset, TokenKind::Bang)
                    }
                }
                b'<' => {
                    cursor.bump();
                    if cursor.peek() == Some(b'=') {
                        cursor.bump();
                        token_with_span(file.id(), start, cursor.offset, TokenKind::LessEq)
                    } else {
                        token_with_span(file.id(), start, cursor.offset, TokenKind::Less)
                    }
                }
                b'>' => {
                    cursor.bump();
                    if cursor.peek() == Some(b'=') {
                        cursor.bump();
                        token_with_span(file.id(), start, cursor.offset, TokenKind::GreaterEq)
                    } else {
                        token_with_span(file.id(), start, cursor.offset, TokenKind::Greater)
                    }
                }
                _ => {
                    cursor.bump();
                    diagnostics.push(Diagnostic::error("unexpected character").with_label(
                        Label::primary(
                            Span::new(file.id(), start, cursor.offset),
                            "this character is not part of the MVP lexer",
                        ),
                    ));
                    next_ident_is_attribute_name = false;
                    continue;
                }
            };

            next_ident_is_attribute_name = matches!(token.kind, TokenKind::At);
            tokens.push(token);
        }

        tokens.push(token_with_span(
            file.id(),
            cursor.offset,
            cursor.offset,
            TokenKind::Eof,
        ));

        if diagnostics.is_empty() {
            Ok(tokens)
        } else {
            Err(diagnostics)
        }
    }

    fn lex_ident_or_keyword(&mut self, cursor: &mut Cursor<'_>, force_ident: bool) -> Token {
        let start = cursor.offset;
        cursor.bump();
        while let Some(byte) = cursor.peek() {
            if is_ident_continue(byte) {
                cursor.bump();
            } else {
                break;
            }
        }

        let text = &cursor.text[start as usize..cursor.offset as usize];
        if force_ident {
            return token_with_span(
                cursor.file_id,
                start,
                cursor.offset,
                TokenKind::Ident(self.interner.intern(text)),
            );
        }

        let kind = match text {
            "fn" => TokenKind::Fn,
            "struct" => TokenKind::Struct,
            "enum" => TokenKind::Enum,
            "effect" => TokenKind::Effect,
            "handle" => TokenKind::Handle,
            "with" => TokenKind::With,
            "let" => TokenKind::Let,
            "mut" => TokenKind::Mut,
            "const" => TokenKind::Const,
            "repr" => TokenKind::Repr,
            "C" => TokenKind::CAbi,
            "return" => TokenKind::Return,
            "if" => TokenKind::If,
            "else" => TokenKind::Else,
            "match" => TokenKind::Match,
            "while" => TokenKind::While,
            "loop" => TokenKind::Loop,
            "break" => TokenKind::Break,
            "continue" => TokenKind::Continue,
            "unsafe" => TokenKind::Unsafe,
            "null" => TokenKind::Null,
            "import" => TokenKind::Import,
            "extern" => TokenKind::Extern,
            "true" => TokenKind::True,
            "false" => TokenKind::False,
            _ => TokenKind::Ident(self.interner.intern(text)),
        };

        token_with_span(cursor.file_id, start, cursor.offset, kind)
    }

    fn lex_number(&mut self, cursor: &mut Cursor<'_>) -> Result<Token, Diagnostic> {
        let start = cursor.offset;
        cursor.bump();
        while let Some(byte) = cursor.peek() {
            if byte.is_ascii_digit() {
                cursor.bump();
            } else {
                break;
            }
        }

        if cursor.peek() == Some(b'.')
            && cursor.peek_next().is_some_and(|byte| byte.is_ascii_digit())
        {
            cursor.bump();
            while let Some(byte) = cursor.peek() {
                if byte.is_ascii_digit() {
                    cursor.bump();
                } else {
                    break;
                }
            }

            let text = &cursor.text[start as usize..cursor.offset as usize];
            let span = Span::new(cursor.file_id, start, cursor.offset);
            let value = text.parse::<f64>().map_err(|_| {
                Diagnostic::error("float literal is invalid")
                    .with_label(Label::primary(span, "this literal is not a valid f64"))
            })?;
            return Ok(token_with_span(
                cursor.file_id,
                start,
                cursor.offset,
                TokenKind::Float(value),
            ));
        }

        let text = &cursor.text[start as usize..cursor.offset as usize];
        let span = Span::new(cursor.file_id, start, cursor.offset);
        let value = text.parse::<u64>().map_err(|_| {
            Diagnostic::error("integer literal is too large")
                .with_label(Label::primary(span, "this literal does not fit in u64"))
        })?;
        Ok(token_with_span(
            cursor.file_id,
            start,
            cursor.offset,
            TokenKind::Int(value),
        ))
    }

    fn lex_string(&mut self, cursor: &mut Cursor<'_>) -> Result<Token, Diagnostic> {
        let start = cursor.offset;
        cursor.bump();
        let mut value = String::new();

        while let Some(byte) = cursor.peek() {
            match byte {
                b'"' => {
                    cursor.bump();
                    let symbol = self.interner.intern(&value);
                    return Ok(token_with_span(
                        cursor.file_id,
                        start,
                        cursor.offset,
                        TokenKind::String(symbol),
                    ));
                }
                b'\\' => {
                    cursor.bump();
                    let Some(escaped) = cursor.bump() else {
                        break;
                    };
                    match escaped {
                        b'n' => value.push('\n'),
                        b'r' => value.push('\r'),
                        b't' => value.push('\t'),
                        b'"' => value.push('"'),
                        b'\\' => value.push('\\'),
                        _ => {
                            return Err(Diagnostic::error("unsupported string escape").with_label(
                                Label::primary(
                                    Span::new(cursor.file_id, cursor.offset - 1, cursor.offset),
                                    "this escape sequence is not supported",
                                ),
                            ));
                        }
                    }
                }
                b'\n' | b'\r' => break,
                _ => {
                    value.push(byte as char);
                    cursor.bump();
                }
            }
        }

        Err(
            Diagnostic::error("unterminated string literal").with_label(Label::primary(
                Span::new(cursor.file_id, start, cursor.offset),
                "this string literal is missing a closing quote",
            )),
        )
    }
}

#[derive(Debug)]
struct Cursor<'a> {
    file_id: FileId,
    text: &'a str,
    offset: u32,
}

impl<'a> Cursor<'a> {
    fn new(file_id: FileId, text: &'a str) -> Self {
        Self {
            file_id,
            text,
            offset: 0,
        }
    }

    fn peek(&self) -> Option<u8> {
        self.text.as_bytes().get(self.offset as usize).copied()
    }

    fn peek_next(&self) -> Option<u8> {
        self.text.as_bytes().get(self.offset as usize + 1).copied()
    }

    fn bump(&mut self) -> Option<u8> {
        let byte = self.peek()?;
        self.offset += 1;
        Some(byte)
    }
}

fn single(cursor: &mut Cursor<'_>, kind: TokenKind) -> Token {
    let start = cursor.offset;
    cursor.bump();
    token_with_span(cursor.file_id, start, cursor.offset, kind)
}

fn token_with_span(file_id: FileId, start: u32, end: u32, kind: TokenKind) -> Token {
    Token {
        kind,
        span: Span::new(file_id, start, end),
    }
}

fn is_whitespace(byte: u8) -> bool {
    matches!(byte, b' ' | b'\t' | b'\r' | b'\n')
}

fn is_ident_continue(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'_'
}

#[cfg(test)]
mod tests {
    use super::{Lexer, TokenKind};
    use crate::source::SourceMap;
    use crate::symbol::Interner;
    use std::path::PathBuf;

    #[test]
    fn lexes_minimal_function() {
        let mut sources = SourceMap::new();
        let file_id = sources.add_file(
            PathBuf::from("test.lain"),
            "fn main() -> i32 { 0 }\n".to_owned(),
        );
        let file = sources.file(file_id);
        let mut interner = Interner::new();
        let mut lexer = Lexer::new(&mut interner);

        let tokens = lexer.lex_file(file).unwrap();

        assert_eq!(tokens[0].kind, TokenKind::Fn);
        match tokens[1].kind {
            TokenKind::Ident(symbol) => assert_eq!(interner.resolve(symbol), "main"),
            other => panic!("expected identifier, got {other:?}"),
        }
        assert_eq!(tokens[2].kind, TokenKind::LParen);
        assert_eq!(tokens[3].kind, TokenKind::RParen);
        assert_eq!(tokens[4].kind, TokenKind::Arrow);
        match tokens[5].kind {
            TokenKind::Ident(symbol) => assert_eq!(interner.resolve(symbol), "i32"),
            other => panic!("expected identifier, got {other:?}"),
        }
        assert_eq!(tokens[6].kind, TokenKind::LBrace);
        assert_eq!(tokens[7].kind, TokenKind::Int(0));
        assert_eq!(tokens[8].kind, TokenKind::RBrace);
        assert_eq!(tokens[9].kind, TokenKind::Eof);
    }

    #[test]
    fn lexes_keywords_and_punctuation() {
        let mut sources = SourceMap::new();
        let file_id = sources.add_file(
            PathBuf::from("test.lain"),
            "import io\nlet mut x = true != false;\n".to_owned(),
        );
        let file = sources.file(file_id);
        let mut interner = Interner::new();
        let mut lexer = Lexer::new(&mut interner);

        let tokens = lexer.lex_file(file).unwrap();

        assert_eq!(tokens[0].kind, TokenKind::Import);
        assert!(matches!(tokens[1].kind, TokenKind::Ident(_)));
        assert_eq!(tokens[2].kind, TokenKind::Let);
        assert_eq!(tokens[3].kind, TokenKind::Mut);
        assert!(matches!(tokens[4].kind, TokenKind::Ident(_)));
        assert_eq!(tokens[5].kind, TokenKind::Assign);
        assert_eq!(tokens[6].kind, TokenKind::True);
        assert_eq!(tokens[7].kind, TokenKind::NotEq);
        assert_eq!(tokens[8].kind, TokenKind::False);
        assert_eq!(tokens[9].kind, TokenKind::Semi);
    }

    #[test]
    fn lexes_item_attribute_marker() {
        let mut sources = SourceMap::new();
        let file_id = sources.add_file(
            PathBuf::from("test.lain"),
            "@derive(Clone) struct Point {}\n".to_owned(),
        );
        let file = sources.file(file_id);
        let mut interner = Interner::new();
        let mut lexer = Lexer::new(&mut interner);

        let tokens = lexer.lex_file(file).unwrap();

        assert_eq!(tokens[0].kind, TokenKind::At);
        assert!(matches!(tokens[1].kind, TokenKind::Ident(_)));
        assert_eq!(tokens[2].kind, TokenKind::LParen);
    }

    #[test]
    fn lexes_keyword_after_at_as_attribute_name() {
        let mut sources = SourceMap::new();
        let file_id = sources.add_file(
            PathBuf::from("test.lain"),
            "@effect(Net) struct NetOps {}\neffect Raw;\n".to_owned(),
        );
        let file = sources.file(file_id);
        let mut interner = Interner::new();
        let mut lexer = Lexer::new(&mut interner);

        let tokens = lexer.lex_file(file).unwrap();

        match tokens[1].kind {
            TokenKind::Ident(symbol) => assert_eq!(interner.resolve(symbol), "effect"),
            other => panic!("expected attribute identifier, got {other:?}"),
        }
        assert!(tokens.iter().any(|token| token.kind == TokenKind::Effect));
    }

    #[test]
    fn lexes_question_mark_operator() {
        let mut sources = SourceMap::new();
        let file_id = sources.add_file(PathBuf::from("test.lain"), "value?\n".to_owned());
        let file = sources.file(file_id);
        let mut interner = Interner::new();
        let mut lexer = Lexer::new(&mut interner);

        let tokens = lexer.lex_file(file).unwrap();

        assert!(matches!(tokens[0].kind, TokenKind::Ident(_)));
        assert_eq!(tokens[1].kind, TokenKind::Question);
        assert_eq!(tokens[2].kind, TokenKind::Eof);
    }

    #[test]
    fn lexes_match_keyword_and_fat_arrow() {
        let mut sources = SourceMap::new();
        let file_id = sources.add_file(
            PathBuf::from("test.lain"),
            "match status { Status::Ok => 1 }\n".to_owned(),
        );
        let file = sources.file(file_id);
        let mut interner = Interner::new();
        let mut lexer = Lexer::new(&mut interner);

        let tokens = lexer.lex_file(file).unwrap();

        assert_eq!(tokens[0].kind, TokenKind::Match);
        assert!(
            tokens
                .iter()
                .any(|token| token.kind == TokenKind::DoubleColon)
        );
        assert!(tokens.iter().any(|token| token.kind == TokenKind::FatArrow));
    }

    #[test]
    fn skips_line_comments() {
        let mut sources = SourceMap::new();
        let file_id = sources.add_file(
            PathBuf::from("test.lain"),
            "// comment\nfn main() { 0 }\n".to_owned(),
        );
        let file = sources.file(file_id);
        let mut interner = Interner::new();
        let mut lexer = Lexer::new(&mut interner);

        let tokens = lexer.lex_file(file).unwrap();

        assert_eq!(tokens[0].kind, TokenKind::Fn);
    }

    #[test]
    fn reports_unexpected_characters() {
        let mut sources = SourceMap::new();
        let file_id = sources.add_file(PathBuf::from("test.lain"), "$\n".to_owned());
        let file = sources.file(file_id);
        let mut interner = Interner::new();
        let mut lexer = Lexer::new(&mut interner);

        let diagnostics = lexer.lex_file(file).unwrap_err();

        assert_eq!(diagnostics.len(), 1);
        assert_eq!(diagnostics[0].message, "unexpected character");
    }

    #[test]
    fn reports_integer_literals_that_do_not_fit_u64() {
        let mut sources = SourceMap::new();
        let file_id = sources.add_file(
            PathBuf::from("test.lain"),
            "999999999999999999999999999999999999999999\n".to_owned(),
        );
        let file = sources.file(file_id);
        let mut interner = Interner::new();
        let mut lexer = Lexer::new(&mut interner);

        let diagnostics = lexer.lex_file(file).unwrap_err();

        assert_eq!(diagnostics.len(), 1);
        assert_eq!(diagnostics[0].message, "integer literal is too large");
    }

    #[test]
    fn lexes_string_literals_with_simple_escapes() {
        let mut sources = SourceMap::new();
        let file_id = sources.add_file(PathBuf::from("test.lain"), "\"hello\\n\"".to_owned());
        let file = sources.file(file_id);
        let mut interner = Interner::new();
        let mut lexer = Lexer::new(&mut interner);

        let tokens = lexer.lex_file(file).unwrap();

        match tokens[0].kind {
            TokenKind::String(symbol) => assert_eq!(interner.resolve(symbol), "hello\n"),
            other => panic!("expected string literal, got {other:?}"),
        }
    }
}
