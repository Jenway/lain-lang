#ifndef LAINIR_BUILDER_H
#define LAINIR_BUILDER_H

/* The owner of an Artifact.
 *
 * Everything a physical Artifact owns — subroutines, blocks, instructions,
 * expressions, types, names and `#data` bytes — is allocated through one
 * builder and released with it.  Nothing here is global: two builders can
 * build two artifacts in the same process, and neither can observe the other.
 *
 * Allocation is injected rather than assumed.  A host that wants an artifact
 * inside memory it already manages (a VSpace region, a bump arena, an
 * interaction with a bootstrap page allocator) passes its own allocator;
 * callers that pass NULL get the process heap.  See seed/REFACTOR_PLAN.md for
 * why the Artifact is owned here and not by LAINVM: the VM borrows a verified
 * Artifact read-only, and a tool such as lainir-print must be able to read one
 * with no VM present at all. */

#include "lainir/core.h"

#include <stddef.h>

typedef struct {
  /* Must return zeroed memory of at least `size` bytes, or NULL. */
  void *(*allocate)(void *context, size_t size);
  void (*release)(void *context, void *pointer);
  void *context;
} LainirAllocator;

typedef struct L1Builder L1Builder;

/* `allocator` NULL selects the process heap.  Returns NULL when the allocator
 * is unusable or out of memory. */
L1Builder *lainir_builder_new(const LainirAllocator *allocator);

/* Release every allocation recorded against the builder, including the
 * builder itself.  Every pointer it handed out, and every node reachable from
 * the root, becomes invalid. */
void lainir_builder_free(L1Builder *builder);

void *lainir_builder_allocate(L1Builder *builder, size_t size);
char *lainir_builder_copy_string(L1Builder *builder, const char *text);

/* Grow an array the builder owns.  The previous block stays on the builder's
 * release list, so the caller must not free `pointer` afterwards; the returned
 * block is zeroed and holds the first `old_size` bytes of the old one. */
void *lainir_builder_grow(L1Builder *builder, void *pointer,
                          size_t old_size, size_t new_size);

/* The subroutine list this builder owns, in module order (`->next` links
 * them).  NULL until the artifact has been parsed. */
L1Subroutine *lainir_builder_root(const L1Builder *builder);
void lainir_builder_set_root(L1Builder *builder, L1Subroutine *root);

L1Type *lainir_new_type(L1Builder *builder, L1TypeKind kind, uint32_t width);
L1Expr *lainir_new_expr(L1Builder *builder, L1ExprKind kind);
L1Instruction *lainir_new_instruction(L1Builder *builder, L1InstKind kind);
L1Block *lainir_new_block(L1Builder *builder);
L1Subroutine *lainir_new_subroutine(L1Builder *builder, const char *name);

#endif /* LAINIR_BUILDER_H */
