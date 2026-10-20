#ifndef ODIN_EMAIL_H
#define ODIN_EMAIL_H

#include <stddef.h>

// bundled list of throwaway/temporary email domains (generated from disposable-email-domains)
extern const char *const disposable_domains[];
extern const size_t disposable_domains_n;

int run_email(int argc, char **argv);

#endif
