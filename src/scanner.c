#include "tree_sitter/parser.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

enum TokenType {
    PAIRED_COMMENT_CONTENT,
    VERBATIM_CONTENT,
    VERBATIM_LABEL
};

#define VERBATIM_LABEL_MAX 255

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(VERBATIM_LABEL_MAX + 1 <= TREE_SITTER_SERIALIZATION_BUFFER_SIZE,
               "verbatim label must fit in the serialization buffer");
#endif

typedef struct {
    uint8_t label_len;
    char label[VERBATIM_LABEL_MAX];
} Scanner;

static void advance(TSLexer *lexer) {
    lexer->advance(lexer, false);
}

static void skip(TSLexer *lexer) {
    lexer->advance(lexer, true);
}

static bool is_word_char(int32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
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

// Django closes a verbatim block only when the endverbatim label is an exact
// match for the opening label (both empty counts as a match).
static bool scan_matching_end_label(Scanner *scanner, TSLexer *lexer) {
    if (is_word_char(lexer->lookahead)) return false;

    while (iswspace(lexer->lookahead)) advance(lexer);

    for (uint8_t i = 0; i < scanner->label_len; i++) {
        if (lexer->lookahead != scanner->label[i]) return false;
        advance(lexer);
    }
    if (is_word_char(lexer->lookahead)) return false;

    while (iswspace(lexer->lookahead)) advance(lexer);

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

    // VERBATIM_LABEL and VERBATIM_CONTENT are never valid in the same parse
    // state; both being valid means error recovery, where capturing a label
    // would poison scanner state.
    if (valid_symbols[VERBATIM_LABEL] && !valid_symbols[VERBATIM_CONTENT]) {
        while (iswspace(lexer->lookahead)) skip(lexer);

        if (is_word_char(lexer->lookahead)) {
            uint8_t len = 0;
            while (is_word_char(lexer->lookahead)) {
                if (len >= VERBATIM_LABEL_MAX) return false;
                scanner->label[len++] = (char)lexer->lookahead;
                advance(lexer);
            }
            scanner->label_len = len;
            lexer->mark_end(lexer);
            lexer->result_symbol = VERBATIM_LABEL;
            return true;
        }
        return false;
    }

    if (valid_symbols[VERBATIM_CONTENT]) {
        while (lexer->lookahead != 0) {
            lexer->mark_end(lexer);

            if (lexer->lookahead == '{') {
                advance(lexer);

                if (lexer->lookahead == '%') {
                    advance(lexer);

                    while (iswspace(lexer->lookahead)) advance(lexer);

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
    buffer[0] = (char)scanner->label_len;
    memcpy(buffer + 1, scanner->label, scanner->label_len);
    return scanner->label_len + 1;
}

void tree_sitter_htmldjango_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    scanner->label_len = 0;
    if (length > 0) {
        scanner->label_len = (uint8_t)buffer[0];
        memcpy(scanner->label, buffer + 1, scanner->label_len);
    }
}
