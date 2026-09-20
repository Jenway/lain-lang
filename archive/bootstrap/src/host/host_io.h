#ifndef LAINIR_HOST_IO_H
#define LAINIR_HOST_IO_H

#include "lainir/emit.h"

#include <stdio.h>

unsigned char *lainir_host_read_file(
    const char *path,
    size_t *length_out);

LainirWriter lainir_host_file_writer(FILE *file);

#endif /* LAINIR_HOST_IO_H */
