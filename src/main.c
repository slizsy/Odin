#include <stdio.h>
#include <string.h>

#include "core/platform.h"
#include "modules/meta/meta_cmd.h"
#include "modules/shell/shell.h"
#include "modules/upgrade/upgrade_cmd.h"
#include "modules/usage.h"
#include "modules/user/user_cmd.h"
#include "ui/banner.h"
#include "ui/color.h"

int main(int argc, char **argv) {
    platform_init();
    color_init();
    // just `odin`: interactive mode
    if (argc < 2) return run_shell();
    if (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        banner_print();
        fputs(usage, stdout);
        return 0;
    }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "meta") || !strcmp(cmd, "m")) return run_meta(argc - 2, argv + 2);
    if (!strcmp(cmd, "user") || !strcmp(cmd, "u")) return run_user(argc - 2, argv + 2);
    if (!strcmp(cmd, "upgrade") || !strcmp(cmd, "update")) return run_upgrade(argc - 2, argv + 2);
    if (!strcmp(cmd, "version") || !strcmp(cmd, "-v") || !strcmp(cmd, "--version")) {
        puts("odin " ODIN_VERSION);
        return 0;
    }
    const char *soon[] = {"email", "domain", "phone", "ip", NULL};
    for (int i = 0; soon[i]; i++) {
        if (!strcmp(cmd, soon[i])) {
            fprintf(stderr, "odin: the %s module isn't built yet\n", cmd);
            return 1;
        }
    }
    fprintf(stderr, "odin: no idea what \"%s\" is\n\n%s", cmd, usage);
    return 2;
}
