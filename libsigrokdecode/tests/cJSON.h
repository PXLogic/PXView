/*
  cJSON.h - minimal single-header JSON parser/writer for decoder_test

  Derived from cJSON by Dave Gamble (MIT License).
  Stripped down to only the features needed by the test program:
  - Parse JSON strings into a tree
  - Get object/array items by key/index
  - Type checks and value getters
  - Create objects, arrays, strings, numbers, bools
  - Add items to objects/arrays
  - Print to formatted string
  - Delete tree

  Define CJSON_IMPLEMENTATION in exactly one .c file before including
  this header to get the implementation.
*/

#ifndef CJSON_H
#define CJSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* cJSON type bits */
#define CJSON_INVALID  0
#define CJSON_FALSE    1
#define CJSON_TRUE     2
#define CJSON_NULL     4
#define CJSON_NUMBER   8
#define CJSON_STRING   16
#define CJSON_ARRAY    32
#define CJSON_OBJECT   64

#define CJSON_ISREF    256
#define CJSON_ISCONST  512

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;  /* key for object items */
} cJSON;

/* Parser */
cJSON *cJSON_Parse(const char *value);
cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length);

/* Deletion */
void cJSON_Delete(cJSON *c);

/* Getters */
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string);
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
int    cJSON_GetArraySize(const cJSON *array);

/* Type checks */
int cJSON_IsInvalid(const cJSON *item);
int cJSON_IsFalse(const cJSON *item);
int cJSON_IsTrue(const cJSON *item);
int cJSON_IsBool(const cJSON *item);
int cJSON_IsNull(const cJSON *item);
int cJSON_IsNumber(const cJSON *item);
int cJSON_IsString(const cJSON *item);
int cJSON_IsArray(const cJSON *item);
int cJSON_IsObject(const cJSON *item);

/* Value getters */
char        *cJSON_GetStringValue(const cJSON *item);
double       cJSON_GetNumberValue(const cJSON *item);
int          cJSON_GetArraySize(const cJSON *array);

/* Creators */
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_CreateArray(void);
cJSON *cJSON_CreateString(const char *string);
cJSON *cJSON_CreateNumber(double num);
cJSON *cJSON_CreateBool(int boolean);
cJSON *cJSON_CreateTrue(void);
cJSON *cJSON_CreateFalse(void);
cJSON *cJSON_CreateNull(void);
cJSON *cJSON_CreateIntArray(const int *numbers, int count);
cJSON *cJSON_CreateStringArray(const char **strings, int count);

/* Add items */
void cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
void cJSON_AddItemToArray(cJSON *array, cJSON *item);
cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *string);
cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double number);
cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, int boolean);
cJSON *cJSON_AddTrueToObject(cJSON *object, const char *name);
cJSON *cJSON_AddFalseToObject(cJSON *object, const char *name);
cJSON *cJSON_AddNullToObject(cJSON *object, const char *name);
void cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item);

/* Print */
char *cJSON_Print(const cJSON *item);
char *cJSON_PrintUnformatted(const cJSON *item);
void  cJSON_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* CJSON_H */

#ifdef CJSON_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <ctype.h>

/* ---- internal helpers ---- */

typedef struct {
    const unsigned char *content;
    size_t length;
    size_t offset;
    size_t depth;
} parse_buffer;

static int cJSON_strcasecmp(const char *s1, const char *s2)
{
    if (!s1) return (s2 ? -1 : 0);
    if (!s2) return 1;
    for (; tolower(*s1) == tolower(*s2); s1++, s2++) {
        if (*s1 == '\0') return 0;
    }
    return tolower(*(const unsigned char *)s1) - tolower(*(const unsigned char *)s2);
}

static void *cJSON_internal_malloc(size_t size) { return malloc(size); }
static void  cJSON_internal_free(void *ptr) { free(ptr); }

static cJSON *cJSON_New_Item(void)
{
    cJSON *node = (cJSON *)cJSON_internal_malloc(sizeof(cJSON));
    if (node) {
        memset(node, 0, sizeof(cJSON));
    }
    return node;
}

static void cJSON_Delete_local(cJSON *c)
{
    cJSON *next = NULL;
    while (c) {
        next = c->next;
        if (!(c->type & CJSON_ISREF) && c->child) {
            cJSON_Delete_local(c->child);
        }
        if (!(c->type & CJSON_ISREF) && c->valuestring) {
            cJSON_internal_free(c->valuestring);
        }
        if (c->string) {
            cJSON_internal_free(c->string);
        }
        cJSON_internal_free(c);
        c = next;
    }
}

void cJSON_Delete(cJSON *c) { cJSON_Delete_local(c); }

/* ---- parser ---- */

static parse_buffer *buffer_at_offset(parse_buffer *buf)
{
    return buf;
}

static int can_access_at_index(parse_buffer *buf, size_t index)
{
    return (buf && (index < buf->length));
}

static int can_read(parse_buffer *buf, size_t num)
{
    return (buf && (buf->offset + num <= buf->length));
}

static unsigned char buffer_at(parse_buffer *buf, size_t index)
{
    return buf->content[index];
}

static int buffer_skip_whitespace(parse_buffer *buf)
{
    while (can_access_at_index(buf, buf->offset)) {
        if (buf->content[buf->offset] == ' '  ||
            buf->content[buf->offset] == '\t' ||
            buf->content[buf->offset] == '\r' ||
            buf->content[buf->offset] == '\n') {
            buf->offset++;
        } else {
            return 1;
        }
    }
    return 0;
}

static int parse_string(cJSON *item, parse_buffer *buf)
{
    const unsigned char *input_pointer = buf->content + buf->offset;
    size_t input_len = 0;
    size_t output_len = 0;

    if (buffer_at(buf, buf->offset) != '\"') return 0;
    buf->offset++;

    /* calculate output length */
    const unsigned char *p = input_pointer + 1;
    while (*p != '\"') {
        if (*p == '\0') return 0;
        if (*p == '\\') {
            p++;
            if (*p == '\0') return 0;
        }
        output_len++;
        input_len++;
        p++;
    }
    input_len++; /* for the closing quote */

    /* allocate output */
    char *output = (char *)cJSON_internal_malloc(output_len + 1);
    if (!output) return 0;

    /* copy with unescaping */
    p = input_pointer + 1;
    size_t i = 0;
    while (*p != '\"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '\"': output[i++] = '\"'; break;
                case '\\': output[i++] = '\\'; break;
                case '/':  output[i++] = '/';  break;
                case 'b':  output[i++] = '\b'; break;
                case 'f':  output[i++] = '\f'; break;
                case 'n':  output[i++] = '\n'; break;
                case 'r':  output[i++] = '\r'; break;
                case 't':  output[i++] = '\t'; break;
                case 'u':
                    /* simplified: just skip 4 hex digits, output '?' */
                    output[i++] = '?';
                    p += 4;
                    break;
                default:   output[i++] = *p;    break;
            }
        } else {
            output[i++] = *p;
        }
        p++;
    }
    output[i] = '\0';

    item->type = CJSON_STRING;
    item->valuestring = output;
    buf->offset += input_len; /* skip past closing quote */
    return 1;
}

static int parse_number(cJSON *item, parse_buffer *buf)
{
    double number = 0;
    unsigned char *start = (unsigned char *)(buf->content + buf->offset);
    unsigned char *end = start;
    int sign = 1;

    if (*end == '-') { sign = -1; end++; }
    if (*end == '0') { end++; }
    else if (*end >= '1' && *end <= '9') {
        number = (*end - '0'); end++;
        while (*end >= '0' && *end <= '9') {
            number = number * 10 + (*end - '0'); end++;
        }
    } else {
        return 0;
    }

    number *= sign;

    if (*end == '.') {
        double fraction = 0;
        double divisor = 10;
        end++;
        while (*end >= '0' && *end <= '9') {
            fraction += (*end - '0') / divisor;
            divisor *= 10;
            end++;
        }
        number += fraction * sign;
    }

    if (*end == 'e' || *end == 'E') {
        int exp_sign = 1;
        int exponent = 0;
        end++;
        if (*end == '+') { end++; }
        else if (*end == '-') { exp_sign = -1; end++; }
        while (*end >= '0' && *end <= '9') {
            exponent = exponent * 10 + (*end - '0'); end++;
        }
        if (exp_sign == 1) {
            while (exponent > 0) { number *= 10; exponent--; }
        } else {
            while (exponent > 0) { number /= 10; exponent--; }
        }
    }

    item->valuedouble = number;
    if (number >= INT_MIN && number <= INT_MAX) {
        item->valueint = (int)number;
    } else {
        item->valueint = 0;
    }
    item->type = CJSON_NUMBER;
    buf->offset += (size_t)(end - start);
    return 1;
}

/* forward declaration */
static int parse_value(cJSON *item, parse_buffer *buf);

static int parse_array(cJSON *item, parse_buffer *buf)
{
    cJSON *head = NULL;
    cJSON *current = NULL;

    if (buffer_at(buf, buf->offset) != '[') return 0;
    buf->offset++;

    buffer_skip_whitespace(buf);
    if (can_access_at_index(buf, buf->offset) && buffer_at(buf, buf->offset) == ']') {
        buf->offset++;
        item->type = CJSON_ARRAY;
        return 1;
    }

    buf->depth++;
    if (buf->depth > 1000) return 0;

    do {
        buffer_skip_whitespace(buf);
        cJSON *new_item = cJSON_New_Item();
        if (!new_item) return 0;

        if (!head) {
            head = new_item;
        } else {
            current->next = new_item;
            new_item->prev = current;
        }
        current = new_item;

        if (!parse_value(current, buf)) {
            cJSON_Delete_local(head);
            return 0;
        }

        buffer_skip_whitespace(buf);
    } while (can_access_at_index(buf, buf->offset) && buffer_at(buf, buf->offset) == ',' && buf->offset++);

    buf->depth--;

    if (!can_access_at_index(buf, buf->offset) || buffer_at(buf, buf->offset) != ']') {
        cJSON_Delete_local(head);
        return 0;
    }
    buf->offset++;

    item->type = CJSON_ARRAY;
    item->child = head;
    return 1;
}

static int parse_object(cJSON *item, parse_buffer *buf)
{
    cJSON *head = NULL;
    cJSON *current = NULL;

    if (buffer_at(buf, buf->offset) != '{') return 0;
    buf->offset++;

    buffer_skip_whitespace(buf);
    if (can_access_at_index(buf, buf->offset) && buffer_at(buf, buf->offset) == '}') {
        buf->offset++;
        item->type = CJSON_OBJECT;
        return 1;
    }

    buf->depth++;
    if (buf->depth > 1000) return 0;

    do {
        buffer_skip_whitespace(buf);
        cJSON *new_item = cJSON_New_Item();
        if (!new_item) return 0;

        /* parse key */
        if (!parse_string(new_item, buf)) {
            cJSON_Delete_local(new_item);
            cJSON_Delete_local(head);
            return 0;
        }
        new_item->string = new_item->valuestring;
        new_item->valuestring = NULL;

        buffer_skip_whitespace(buf);
        if (!can_access_at_index(buf, buf->offset) || buffer_at(buf, buf->offset) != ':') {
            cJSON_Delete_local(new_item);
            cJSON_Delete_local(head);
            return 0;
        }
        buf->offset++;

        buffer_skip_whitespace(buf);
        if (!parse_value(new_item, buf)) {
            cJSON_Delete_local(new_item);
            cJSON_Delete_local(head);
            return 0;
        }

        if (!head) {
            head = new_item;
        } else {
            current->next = new_item;
            new_item->prev = current;
        }
        current = new_item;

        buffer_skip_whitespace(buf);
    } while (can_access_at_index(buf, buf->offset) && buffer_at(buf, buf->offset) == ',' && buf->offset++);

    buf->depth--;

    if (!can_access_at_index(buf, buf->offset) || buffer_at(buf, buf->offset) != '}') {
        cJSON_Delete_local(head);
        return 0;
    }
    buf->offset++;

    item->type = CJSON_OBJECT;
    item->child = head;
    return 1;
}

static int parse_value(cJSON *item, parse_buffer *buf)
{
    buffer_skip_whitespace(buf);
    if (!can_access_at_index(buf, buf->offset)) return 0;

    switch (buffer_at(buf, buf->offset)) {
        case 'n':
            if (can_read(buf, 4) && memcmp(buf->content + buf->offset, "null", 4) == 0) {
                item->type = CJSON_NULL;
                buf->offset += 4;
                return 1;
            }
            return 0;
        case 't':
            if (can_read(buf, 4) && memcmp(buf->content + buf->offset, "true", 4) == 0) {
                item->type = CJSON_TRUE;
                item->valueint = 1;
                buf->offset += 4;
                return 1;
            }
            return 0;
        case 'f':
            if (can_read(buf, 5) && memcmp(buf->content + buf->offset, "false", 5) == 0) {
                item->type = CJSON_FALSE;
                item->valueint = 0;
                buf->offset += 5;
                return 1;
            }
            return 0;
        case '\"':
            return parse_string(item, buf);
        case '[':
            return parse_array(item, buf);
        case '{':
            return parse_object(item, buf);
        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return parse_number(item, buf);
        default:
            return 0;
    }
}

cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    if (!value) return NULL;
    parse_buffer buf;
    buf.content = (const unsigned char *)value;
    buf.length = buffer_length;
    buf.offset = 0;
    buf.depth = 0;

    cJSON *item = cJSON_New_Item();
    if (!item) return NULL;

    if (!parse_value(item, &buf)) {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

cJSON *cJSON_Parse(const char *value)
{
    if (!value) return NULL;
    return cJSON_ParseWithLength(value, strlen(value));
}

/* ---- getters ---- */

cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItemCaseSensitive(object, string);
}

cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string)
{
    cJSON *child = NULL;
    if (!object || !string) return NULL;
    child = object->child;
    while (child && cJSON_strcasecmp(child->string, string)) {
        child = child->next;
    }
    return child;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index)
{
    cJSON *child = NULL;
    if (!array) return NULL;
    child = array->child;
    while (child && index > 0) {
        child = child->next;
        index--;
    }
    return child;
}

int cJSON_GetArraySize(const cJSON *array)
{
    int size = 0;
    cJSON *child = NULL;
    if (!array) return 0;
    child = array->child;
    while (child) {
        size++;
        child = child->next;
    }
    return size;
}

/* ---- type checks ---- */
int cJSON_IsInvalid(const cJSON *item) { return (item && (item->type & 0xFF) == CJSON_INVALID); }
int cJSON_IsFalse(const cJSON *item)   { return (item && (item->type & 0xFF) == CJSON_FALSE); }
int cJSON_IsTrue(const cJSON *item)    { return (item && (item->type & 0xFF) == CJSON_TRUE); }
int cJSON_IsBool(const cJSON *item)    { return (item && ((item->type & 0xFF) == CJSON_TRUE || (item->type & 0xFF) == CJSON_FALSE)); }
int cJSON_IsNull(const cJSON *item)    { return (item && (item->type & 0xFF) == CJSON_NULL); }
int cJSON_IsNumber(const cJSON *item)  { return (item && (item->type & 0xFF) == CJSON_NUMBER); }
int cJSON_IsString(const cJSON *item)  { return (item && (item->type & 0xFF) == CJSON_STRING); }
int cJSON_IsArray(const cJSON *item)   { return (item && (item->type & 0xFF) == CJSON_ARRAY); }
int cJSON_IsObject(const cJSON *item)  { return (item && (item->type & 0xFF) == CJSON_OBJECT); }

/* ---- value getters ---- */
char *cJSON_GetStringValue(const cJSON *item)
{
    if (!cJSON_IsString(item)) return NULL;
    return item->valuestring;
}

double cJSON_GetNumberValue(const cJSON *item)
{
    if (!cJSON_IsNumber(item)) return 0;
    return item->valuedouble;
}

/* ---- creators ---- */

cJSON *cJSON_CreateObject(void) { cJSON *item = cJSON_New_Item(); if (item) item->type = CJSON_OBJECT; return item; }
cJSON *cJSON_CreateArray(void)  { cJSON *item = cJSON_New_Item(); if (item) item->type = CJSON_ARRAY;  return item; }

cJSON *cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item();
    if (item) {
        item->type = CJSON_STRING;
        item->valuestring = (char *)cJSON_internal_malloc(strlen(string) + 1);
        if (item->valuestring) {
            strcpy(item->valuestring, string);
        }
    }
    return item;
}

cJSON *cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item();
    if (item) {
        item->type = CJSON_NUMBER;
        item->valuedouble = num;
        if (num >= INT_MIN && num <= INT_MAX) {
            item->valueint = (int)num;
        } else {
            item->valueint = 0;
        }
    }
    return item;
}

cJSON *cJSON_CreateBool(int boolean) { cJSON *item = cJSON_New_Item(); if (item) { item->type = boolean ? CJSON_TRUE : CJSON_FALSE; item->valueint = boolean ? 1 : 0; } return item; }
cJSON *cJSON_CreateTrue(void)  { return cJSON_CreateBool(1); }
cJSON *cJSON_CreateFalse(void) { return cJSON_CreateBool(0); }
cJSON *cJSON_CreateNull(void)  { cJSON *item = cJSON_New_Item(); if (item) item->type = CJSON_NULL; return item; }

cJSON *cJSON_CreateIntArray(const int *numbers, int count)
{
    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(array, cJSON_CreateNumber(numbers[i]));
    }
    return array;
}

cJSON *cJSON_CreateStringArray(const char **strings, int count)
{
    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(array, cJSON_CreateString(strings[i]));
    }
    return array;
}

/* ---- add items ---- */

static void suffix_object(cJSON *prev, cJSON *item)
{
    prev->next = item;
    item->prev = prev;
}

void cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    if (!array || !item) return;
    if (!array->child) {
        array->child = item;
    } else {
        cJSON *child = array->child;
        while (child->next) child = child->next;
        suffix_object(child, item);
    }
}

void cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    if (!object || !item) return;
    if (item->string) cJSON_internal_free(item->string);
    item->string = (char *)cJSON_internal_malloc(strlen(string) + 1);
    if (item->string) strcpy(item->string, string);
    cJSON_AddItemToArray(object, item);
}

void cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item)
{
    cJSON_AddItemToObject(object, string, item);
}

cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *string) { cJSON *s = cJSON_CreateString(string); cJSON_AddItemToObject(object, name, s); return s; }
cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double number)      { cJSON *n = cJSON_CreateNumber(number); cJSON_AddItemToObject(object, name, n); return n; }
cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, int boolean)          { cJSON *b = cJSON_CreateBool(boolean); cJSON_AddItemToObject(object, name, b); return b; }
cJSON *cJSON_AddTrueToObject(cJSON *object, const char *name)                       { cJSON *t = cJSON_CreateTrue(); cJSON_AddItemToObject(object, name, t); return t; }
cJSON *cJSON_AddFalseToObject(cJSON *object, const char *name)                      { cJSON *f = cJSON_CreateFalse(); cJSON_AddItemToObject(object, name, f); return f; }
cJSON *cJSON_AddNullToObject(cJSON *object, const char *name)                       { cJSON *n = cJSON_CreateNull(); cJSON_AddItemToObject(object, name, n); return n; }

/* ---- printer ---- */

typedef struct {
    char *buffer;
    size_t length;
    size_t offset;
} printbuffer;

static int ensure(printbuffer *p, size_t needed)
{
    if (!p) return 0;
    if (p->offset + needed > p->length) {
        size_t newsize = p->length * 2;
        if (newsize < p->offset + needed) newsize = p->offset + needed + 256;
        char *newbuf = (char *)cJSON_internal_malloc(newsize);
        if (!newbuf) return 0;
        memcpy(newbuf, p->buffer, p->offset);
        cJSON_internal_free(p->buffer);
        p->buffer = newbuf;
        p->length = newsize;
    }
    return 1;
}

static int print_string_ptr(const char *str, printbuffer *p)
{
    const char *ptr;
    size_t len = 0;

    if (!str) {
        if (ensure(p, 4)) { memcpy(p->buffer + p->offset, "\"\"", 3); p->offset += 2; }
        return 1;
    }

    /* calculate escaped length */
    for (ptr = str; *ptr; ptr++) {
        switch (*ptr) {
            case '\"': case '\\': case '\b': case '\f': case '\n': case '\r': case '\t':
                len += 2; break;
            default:
                if ((unsigned char)*ptr < 0x20) len += 6; /* \uXXXX */
                else len++;
                break;
        }
    }

    if (!ensure(p, len + 3)) return 0;
    p->buffer[p->offset++] = '\"';
    for (ptr = str; *ptr; ptr++) {
        switch (*ptr) {
            case '\"': p->buffer[p->offset++] = '\\'; p->buffer[p->offset++] = '\"'; break;
            case '\\': p->buffer[p->offset++] = '\\'; p->buffer[p->offset++] = '\\'; break;
            case '\b': p->buffer[p->offset++] = '\\'; p->buffer[p->offset++] = 'b';  break;
            case '\f': p->buffer[p->offset++] = '\\'; p->buffer[p->offset++] = 'f';  break;
            case '\n': p->buffer[p->offset++] = '\\'; p->buffer[p->offset++] = 'n';  break;
            case '\r': p->buffer[p->offset++] = '\\'; p->buffer[p->offset++] = 'r';  break;
            case '\t': p->buffer[p->offset++] = '\\'; p->buffer[p->offset++] = 't';  break;
            default:
                if ((unsigned char)*ptr < 0x20) {
                    p->offset += sprintf(p->buffer + p->offset, "\\u%04x", (unsigned char)*ptr);
                } else {
                    p->buffer[p->offset++] = *ptr;
                }
                break;
        }
    }
    p->buffer[p->offset++] = '\"';
    return 1;
}

static int print_number(const cJSON *item, printbuffer *p)
{
    double d = item->valuedouble;
    int intitem = item->valueint;

    if (d == 0) {
        if (ensure(p, 4)) { p->offset += sprintf(p->buffer + p->offset, "0"); }
    } else if ((double)intitem == d && intitem >= INT_MIN && intitem <= INT_MAX && fabs(d) < 1e15) {
        if (ensure(p, 32)) {
            p->offset += sprintf(p->buffer + p->offset, "%d", intitem);
        }
    } else {
        if (ensure(p, 64)) {
            if (fabs(d) < 1e-6 || fabs(d) > 1e15) {
                p->offset += sprintf(p->buffer + p->offset, "%1.17g", d);
            } else {
                p->offset += sprintf(p->buffer + p->offset, "%1.17g", d);
            }
        }
    }
    return 1;
}

/* forward declaration */
static int print_value(const cJSON *item, int depth, int format, printbuffer *p);

static int print_array(const cJSON *item, int depth, int format, printbuffer *p)
{
    if (!ensure(p, 3)) return 0;
    p->buffer[p->offset++] = '[';

    cJSON *child = item->child;
    while (child) {
        if (format) {
            if (!ensure(p, depth + 4)) return 0;
            p->buffer[p->offset++] = '\n';
            memset(p->buffer + p->offset, '\t', depth + 1);
            p->offset += depth + 1;
        }
        if (!print_value(child, depth + 1, format, p)) return 0;
        child = child->next;
        if (child) {
            if (!ensure(p, 2)) return 0;
            p->buffer[p->offset++] = ',';
            if (format) p->buffer[p->offset++] = ' ';
        }
    }

    if (format) {
        if (!ensure(p, depth + 3)) return 0;
        p->buffer[p->offset++] = '\n';
        memset(p->buffer + p->offset, '\t', depth);
        p->offset += depth;
    }
    p->buffer[p->offset++] = ']';
    return 1;
}

static int print_object(const cJSON *item, int depth, int format, printbuffer *p)
{
    if (!ensure(p, 3)) return 0;
    p->buffer[p->offset++] = '{';

    cJSON *child = item->child;
    while (child) {
        if (format) {
            if (!ensure(p, depth + 4)) return 0;
            p->buffer[p->offset++] = '\n';
            memset(p->buffer + p->offset, '\t', depth + 1);
            p->offset += depth + 1;
        }
        if (!print_string_ptr(child->string, p)) return 0;
        if (!ensure(p, 3)) return 0;
        p->buffer[p->offset++] = ':';
        if (format) p->buffer[p->offset++] = ' ';
        if (!print_value(child, depth + 1, format, p)) return 0;
        child = child->next;
        if (child) {
            if (!ensure(p, 2)) return 0;
            p->buffer[p->offset++] = ',';
        }
    }

    if (format) {
        if (!ensure(p, depth + 3)) return 0;
        p->buffer[p->offset++] = '\n';
        memset(p->buffer + p->offset, '\t', depth);
        p->offset += depth;
    }
    p->buffer[p->offset++] = '}';
    return 1;
}

static int print_value(const cJSON *item, int depth, int format, printbuffer *p)
{
    if (!item) return 0;
    switch (item->type & 0xFF) {
        case CJSON_NULL:   if (ensure(p, 5))  { memcpy(p->buffer + p->offset, "null", 4); p->offset += 4; } return 1;
        case CJSON_FALSE:  if (ensure(p, 6))  { memcpy(p->buffer + p->offset, "false", 5); p->offset += 5; } return 1;
        case CJSON_TRUE:   if (ensure(p, 5))  { memcpy(p->buffer + p->offset, "true", 4); p->offset += 4; } return 1;
        case CJSON_NUMBER: return print_number(item, p);
        case CJSON_STRING: return print_string_ptr(item->valuestring, p);
        case CJSON_ARRAY:  return print_array(item, depth, format, p);
        case CJSON_OBJECT: return print_object(item, depth, format, p);
        default: return 0;
    }
}

char *cJSON_Print(const cJSON *item)
{
    printbuffer p;
    p.length = 4096;
    p.offset = 0;
    p.buffer = (char *)cJSON_internal_malloc(p.length);
    if (!p.buffer) return NULL;

    if (!print_value(item, 0, 1, &p)) {
        cJSON_internal_free(p.buffer);
        return NULL;
    }
    if (!ensure(&p, 2)) {
        cJSON_internal_free(p.buffer);
        return NULL;
    }
    p.buffer[p.offset] = '\0';
    return p.buffer;
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    printbuffer p;
    p.length = 4096;
    p.offset = 0;
    p.buffer = (char *)cJSON_internal_malloc(p.length);
    if (!p.buffer) return NULL;

    if (!print_value(item, 0, 0, &p)) {
        cJSON_internal_free(p.buffer);
        return NULL;
    }
    if (!ensure(&p, 2)) {
        cJSON_internal_free(p.buffer);
        return NULL;
    }
    p.buffer[p.offset] = '\0';
    return p.buffer;
}

void cJSON_free(void *ptr)
{
    cJSON_internal_free(ptr);
}

#endif /* CJSON_IMPLEMENTATION */
