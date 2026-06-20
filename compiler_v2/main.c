#include "l1ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* parser */
L1Module *parse_module(const char *src);

/* emitter */
void emit_module(FILE *out, L1Module *m);

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: l1c <input.l1> [options]\n");
        fprintf(stderr, "  compiles L1 IR to C on stdout\n");
        return 1;
    }

    const char *path = argv[1];
    char *src = read_file(path);
    if (!src) return 1;

    L1Module *m = parse_module(src);
    free(src);

    emit_module(stdout, m);
    module_free(m);
    return 0;
}
