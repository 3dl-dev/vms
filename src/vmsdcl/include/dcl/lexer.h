#ifndef __DCL_LEXER_H
#define __DCL_LEXER_H

#include <stdint.h>
#include <stddef.h>

/* Token types */
typedef enum {
    TOK_EOF = 0,
    TOK_WORD,           /* Command word or argument */
    TOK_STRING,         /* "quoted string" */
    TOK_NUMBER,         /* Integer literal */
    TOK_SYMBOL_REF,     /* 'symbol or &symbol */
    TOK_LEXICAL,        /* F$xxx(...) */
    TOK_QUALIFIER,      /* /QUALIFIER */
    TOK_EQUALS,         /* = */
    TOK_COLON_EQUALS,   /* := */
    TOK_COLON_COLON_EQUALS, /* :== */
    TOK_COMMA,          /* , */
    TOK_PLUS,           /* + */
    TOK_MINUS,          /* - */
    TOK_LPAREN,         /* ( */
    TOK_RPAREN,         /* ) */
    TOK_AT,             /* @ (execute procedure) */
    TOK_PIPE,           /* | (for PIPE command) */
    TOK_AMPERSAND,      /* & (symbol substitution) */
    TOK_APOSTROPHE,     /* ' (symbol substitution) */
    TOK_SEMICOLON,      /* ; (comment) */
    TOK_LABEL,          /* LABEL: at start of line */
    TOK_NEWLINE,
    TOK_CONTINUATION,   /* - at end of line */
    TOK_DOT_AND,        /* .AND. */
    TOK_DOT_OR,         /* .OR. */
    TOK_DOT_NOT,        /* .NOT. */
    TOK_EQ,             /* .EQ. */
    TOK_NE,             /* .NE. */
    TOK_LT,             /* .LT. */
    TOK_GT,             /* .GT. */
    TOK_LE,             /* .LE. */
    TOK_GE,             /* .GE. */
    TOK_EQS,            /* .EQS. */
    TOK_NES,            /* .NES. */
    TOK_LTS,            /* .LTS. */
    TOK_GTS,            /* .GTS. */
    TOK_LES,            /* .LES. */
    TOK_GES,            /* .GES. */
} dcl_token_type_t;

typedef struct {
    dcl_token_type_t type;
    char value[1024];       /* Token text */
    int  line;
    int  col;
} dcl_token_t;

typedef struct {
    const char *input;
    size_t pos;
    size_t length;
    int line;
    int col;
    int at_line_start;      /* Track if we are at beginning of line */
} dcl_lexer_t;

void dcl_lexer_init(dcl_lexer_t *lex, const char *input);
int  dcl_lexer_next(dcl_lexer_t *lex, dcl_token_t *token);
int  dcl_lexer_peek(dcl_lexer_t *lex, dcl_token_t *token);

#endif /* __DCL_LEXER_H */
