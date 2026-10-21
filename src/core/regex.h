#ifndef ODIN_REGEX_H
#define ODIN_REGEX_H

#include <stdbool.h>

// tiny regex matcher for the username rules in site lists (python syntax subset):
// ^ $ . [a-z] [^...] \d \w \s \uXXXX, * + ? {m} {m,} {m,n}, (...) (?:...) |, (?=...) (?!...)
// it searches like python's re.search. bad or unsupported patterns (lookbehind etc) set
// *unsupported and return true, so a rule we can't read never blocks a username
bool rx_search(const char *pattern, const char *text, bool *unsupported);

// like rx_search but the pattern has to match the whole string (used by the phone validator
// where libphonenumber's patterns describe the entire national number)
bool rx_full(const char *pattern, const char *text, bool *unsupported);

#endif
