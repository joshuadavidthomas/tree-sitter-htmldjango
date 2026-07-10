#include "tree_sitter/parser.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

enum TokenType {
    PAIRED_COMMENT_CONTENT,
    VERBATIM_CONTENT,
    VERBATIM_LABEL
};

// Django compares the full stripped tag content ("endverbatim" + label,
// internal whitespace preserved) byte-for-byte against the string stored
// when the block opened. See Lexer.create_token in django/template/base.py.
// The label is stored as code points so comparison is exact for non-ASCII.
#define LABEL_CAPACITY (TREE_SITTER_SERIALIZATION_BUFFER_SIZE / sizeof(uint32_t))

typedef struct {
    uint32_t label_len;
    uint32_t label[LABEL_CAPACITY];
} Scanner;

static void advance(TSLexer *lexer) {
    lexer->advance(lexer, false);
}

static void skip(TSLexer *lexer) {
    lexer->advance(lexer, true);
}

// Django's tag regex ({%.*?%}) cannot match across a newline, so a newline
// terminates any candidate tag. '.' matches every other whitespace character.
static bool is_inline_space(int32_t c) {
    return c != '\n' && iswspace(c);
}

static bool scan_str(TSLexer *lexer, const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        if (lexer->lookahead == str[i]) {
            advance(lexer);
        } else {
            return false;
        }
    }
    return true;
}

// Captures everything between "verbatim" and "%}" with trailing whitespace
// stripped, mirroring Django's token_string[2:-2].strip(). The token spans
// only the stripped label text; surrounding whitespace stays out of the node.
static bool scan_verbatim_label(Scanner *scanner, TSLexer *lexer) {
    // Django only activates verbatim mode when the tag name is followed by
    // a literal space: block_content[:9] in ('verbatim', 'verbatim ').
    if (lexer->lookahead != ' ') return false;

    uint32_t len = 0;
    uint32_t stripped_len = 0;

    while (is_inline_space(lexer->lookahead)) {
        if (len >= LABEL_CAPACITY) return false;
        scanner->label[len++] = (uint32_t)lexer->lookahead;
        skip(lexer);
    }

    while (lexer->lookahead != 0 && lexer->lookahead != '\n') {
        if (lexer->lookahead == '%') {
            advance(lexer);
            if (lexer->lookahead == '}') {
                if (stripped_len == 0) return false;
                scanner->label_len = stripped_len;
                lexer->result_symbol = VERBATIM_LABEL;
                return true;
            }
            if (len >= LABEL_CAPACITY) return false;
            scanner->label[len++] = '%';
            stripped_len = len;
            lexer->mark_end(lexer);
            continue;
        }

        if (len >= LABEL_CAPACITY) return false;
        scanner->label[len++] = (uint32_t)lexer->lookahead;
        bool space = is_inline_space(lexer->lookahead);
        advance(lexer);
        if (!space) {
            stripped_len = len;
            lexer->mark_end(lexer);
        }
    }

    return false;
}

static bool scan_matching_end_label(Scanner *scanner, TSLexer *lexer) {
    for (uint32_t i = 0; i < scanner->label_len; i++) {
        if (lexer->lookahead != (int32_t)scanner->label[i]) return false;
        advance(lexer);
    }

    while (is_inline_space(lexer->lookahead)) advance(lexer);

    if (lexer->lookahead != '%') return false;
    advance(lexer);
    return lexer->lookahead == '}';
}

bool tree_sitter_htmldjango_external_scanner_scan(
    void *payload,
    TSLexer *lexer,
    const bool *valid_symbols
) {
    Scanner *scanner = (Scanner *)payload;

    // VERBATIM_LABEL and VERBATIM_CONTENT are never valid in the same parse
    // state; both being valid means error recovery, where scanning could
    // mutate label state or swallow arbitrary source.
    if (valid_symbols[VERBATIM_LABEL] && valid_symbols[VERBATIM_CONTENT]) {
        return false;
    }

    if (valid_symbols[PAIRED_COMMENT_CONTENT]) {
        int depth = 0;
        while (lexer->lookahead != 0) {
            lexer->mark_end(lexer);

            if (lexer->lookahead == '{') {
                advance(lexer);

                if (lexer->lookahead == '%') {
                    advance(lexer);

                    while (iswspace(lexer->lookahead)) advance(lexer);

                    if (scan_str(lexer, "comment")) {
                        depth++;
                        continue;
                    }

                    if (scan_str(lexer, "endcomment")) {
                        if (depth == 0) {
                            lexer->result_symbol = PAIRED_COMMENT_CONTENT;
                            return true;
                        }
                        depth--;
                    }
                }
            }

            advance(lexer);
        }
    }

    if (valid_symbols[VERBATIM_LABEL]) {
        return scan_verbatim_label(scanner, lexer);
    }

    if (valid_symbols[VERBATIM_CONTENT]) {
        while (lexer->lookahead != 0) {
            lexer->mark_end(lexer);

            if (lexer->lookahead == '{') {
                advance(lexer);

                if (lexer->lookahead == '%') {
                    advance(lexer);

                    while (is_inline_space(lexer->lookahead)) advance(lexer);

                    if (scan_str(lexer, "endverbatim") &&
                        scan_matching_end_label(scanner, lexer)) {
                        scanner->label_len = 0;
                        lexer->result_symbol = VERBATIM_CONTENT;
                        return true;
                    }
                }
            }

            advance(lexer);
        }

        // Django stays in verbatim mode through EOF; the interior remains
        // raw text even when the block is never closed.
        lexer->mark_end(lexer);
        lexer->result_symbol = VERBATIM_CONTENT;
        return true;
    }

    return false;
}

void *tree_sitter_htmldjango_external_scanner_create() {
    return calloc(1, sizeof(Scanner));
}

void tree_sitter_htmldjango_external_scanner_destroy(void *payload) {
    free(payload);
}

unsigned tree_sitter_htmldjango_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    unsigned size = scanner->label_len * sizeof(uint32_t);
    memcpy(buffer, scanner->label, size);
    return size;
}

void tree_sitter_htmldjango_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    scanner->label_len = length / sizeof(uint32_t);
    memcpy(scanner->label, buffer, length);
}
