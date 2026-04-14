/*
 * jsmn (minimalistic JSON parser in C)
 * Source: https://github.com/zserge/jsmn (MIT license)
 *
 * This is a single-header embed of jsmn to avoid extra dependencies on RV1106/uClibc.
 */

#pragma once

#include <stddef.h>

typedef enum {
    JSMN_UNDEFINED = 0,
    JSMN_OBJECT = 1,
    JSMN_ARRAY = 2,
    JSMN_STRING = 3,
    JSMN_PRIMITIVE = 4
} jsmntype_t;

enum jsmnerr {
    /* Not enough tokens were provided */
    JSMN_ERROR_NOMEM = -1,
    /* Invalid character inside JSON string */
    JSMN_ERROR_INVAL = -2,
    /* The string is not a full JSON packet, more bytes expected */
    JSMN_ERROR_PART = -3
};

typedef struct {
    jsmntype_t type;
    int start;
    int end;
    int size;
    int parent;
} jsmntok_t;

typedef struct {
    unsigned int pos;     /* offset in the JSON string */
    unsigned int toknext; /* next token to allocate */
    int toksuper;         /* superior token node, e.g parent object or array */
} jsmn_parser;

static inline void jsmn_init(jsmn_parser *parser) {
    parser->pos = 0;
    parser->toknext = 0;
    parser->toksuper = -1;
}

static inline jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens, size_t num_tokens) {
    if (parser->toknext >= num_tokens) return NULL;
    jsmntok_t *tok = &tokens[parser->toknext++];
    tok->start = tok->end = -1;
    tok->size = 0;
    tok->parent = -1;
    tok->type = JSMN_UNDEFINED;
    return tok;
}

static inline void jsmn_fill_token(jsmntok_t *token, jsmntype_t type, int start, int end) {
    token->type = type;
    token->start = start;
    token->end = end;
    token->size = 0;
}

static inline int jsmn_parse_primitive(jsmn_parser *parser, const char *js, size_t len,
                                      jsmntok_t *tokens, size_t num_tokens) {
    int start = (int)parser->pos;
    for (; parser->pos < len; parser->pos++) {
        switch (js[parser->pos]) {
            case '\t': case '\r': case '\n': case ' ':
            case ',': case ']': case '}':
                goto found;
        }
        if (js[parser->pos] < 32 || js[parser->pos] >= 127) {
            parser->pos = (unsigned int)start;
            return JSMN_ERROR_INVAL;
        }
    }
found:
    {
        jsmntok_t *tok = jsmn_alloc_token(parser, tokens, num_tokens);
        if (!tok) {
            parser->pos = (unsigned int)start;
            return JSMN_ERROR_NOMEM;
        }
        jsmn_fill_token(tok, JSMN_PRIMITIVE, start, (int)parser->pos);
        tok->parent = parser->toksuper;
    }
    parser->pos--; /* keep current char for outer loop */
    return 0;
}

static inline int jsmn_parse_string(jsmn_parser *parser, const char *js, size_t len,
                                   jsmntok_t *tokens, size_t num_tokens) {
    int start = (int)parser->pos;
    parser->pos++;
    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        if (c == '"') {
            jsmntok_t *tok = jsmn_alloc_token(parser, tokens, num_tokens);
            if (!tok) {
                parser->pos = (unsigned int)start;
                return JSMN_ERROR_NOMEM;
            }
            jsmn_fill_token(tok, JSMN_STRING, start + 1, (int)parser->pos);
            tok->parent = parser->toksuper;
            return 0;
        }
        if (c == '\\') {
            parser->pos++;
            if (parser->pos >= len) {
                parser->pos = (unsigned int)start;
                return JSMN_ERROR_PART;
            }
            /* skip escaped char */
        }
    }
    parser->pos = (unsigned int)start;
    return JSMN_ERROR_PART;
}

static inline int jsmn_parse(jsmn_parser *parser, const char *js, size_t len,
                             jsmntok_t *tokens, unsigned int num_tokens) {
    int r;
    int count = (int)parser->toknext;

    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        jsmntok_t *tok;

        switch (c) {
            case '{': case '[':
                count++;
                tok = jsmn_alloc_token(parser, tokens, num_tokens);
                if (!tok) return JSMN_ERROR_NOMEM;
                tok->type = (c == '{' ? JSMN_OBJECT : JSMN_ARRAY);
                tok->start = (int)parser->pos;
                tok->parent = parser->toksuper;
                parser->toksuper = (int)parser->toknext - 1;
                break;
            case '}': case ']':
                {
                    jsmntype_t type = (c == '}' ? JSMN_OBJECT : JSMN_ARRAY);
                    int i;
                    for (i = (int)parser->toknext - 1; i >= 0; i--) {
                        tok = &tokens[i];
                        if (tok->start != -1 && tok->end == -1) {
                            if (tok->type != type) return JSMN_ERROR_INVAL;
                            tok->end = (int)parser->pos + 1;
                            parser->toksuper = tok->parent;
                            break;
                        }
                    }
                    if (i == -1) return JSMN_ERROR_INVAL;
                }
                break;
            case '"':
                r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
                if (r < 0) return r;
                count++;
                break;
            case '\t': case '\r': case '\n': case ' ':
            case ':': case ',':
                break;
            default:
                r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
                if (r < 0) return r;
                count++;
                break;
        }

        if (parser->toksuper != -1 && tokens != NULL) {
            tokens[parser->toksuper].size++;
        }
    }

    for (unsigned int i = parser->toknext; i > 0; i--) {
        if (tokens[i - 1].start != -1 && tokens[i - 1].end == -1) {
            return JSMN_ERROR_PART;
        }
    }

    return count;
}
