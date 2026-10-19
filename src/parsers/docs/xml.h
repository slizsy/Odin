#ifndef ODIN_XML_H
#define ODIN_XML_H

#include <stddef.h>

// called for every element at the wanted depth that has text and no child elements.
// name is the local name (namespace prefix stripped)
typedef void (*xml_leaf_fn)(void *ctx, const char *name, const char *text);

void xml_leaves(const char *s, size_t n, int depth, xml_leaf_fn fn, void *ctx);

// value of attr `name` inside a tag's text, unescaped. NULL if it's not there
char *xml_attr(const char *tag, size_t n, const char *name);

// end of a tag that starts at s (the '<'), skipping '>' inside quotes. NULL if unterminated
const char *xml_tag_end(const char *s, const char *end);

#endif
