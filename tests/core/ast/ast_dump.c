#include "lainast/lain_ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void indent(int n) {
  for (int i = 0; i < n; i++) putchar(' ');
}

static const char *node_text(const AstArena *arena, AstNodeId id) {
  const AstNode *node = ast_get(arena, id);
  return node && node->text ? node->text : "";
}

static const char *group_name(const AstArena *arena, const AstNode *node) {
  const char *op = node_text(arena, node->op);
  if (strcmp(op, "(") == 0) return "paren";
  if (strcmp(op, "{") == 0) return "brace";
  if (strcmp(op, "[") == 0) return "bracket";
  return "root";
}

static void dump_node(const AstArena *arena, AstNodeId id, int depth);

static void print_escaped(const char *text) {
  putchar('"');
  for (const char *p = text; p && *p; p++) {
    if (*p == '"' || *p == '\\') {
      putchar('\\');
      putchar(*p);
    } else if (*p == '\n') {
      printf("\\n");
    } else if (*p == '\r') {
      printf("\\r");
    } else if (*p == '\t') {
      printf("\\t");
    } else {
      putchar(*p);
    }
  }
  putchar('"');
}

static void dump_children(const AstArena *arena, AstNodeId first, int depth) {
  AstNodeId curr = first;
  while (curr != AST_NULL) {
    dump_node(arena, curr, depth);
    curr = ast_get(arena, curr)->next;
  }
}

static void dump_node(const AstArena *arena, AstNodeId id, int depth) {
  const AstNode *node = ast_get(arena, id);
  if (!node) return;

  switch (node->kind) {
  case AST_ATOM:
    indent(depth);
    printf("(atom ");
    print_escaped(node->text ? node->text : "");
    printf(")\n");
    break;
  case AST_GROUP:
    indent(depth);
    printf("(group %s\n", group_name(arena, node));
    dump_children(arena, node->left, depth + 2);
    indent(depth);
    printf(")\n");
    break;
  case AST_PREFIX:
    indent(depth);
    printf("(prefix ");
    print_escaped(node_text(arena, node->op));
    printf("\n");
    dump_node(arena, node->left, depth + 2);
    indent(depth);
    printf(")\n");
    break;
  case AST_POSTFIX:
    indent(depth);
    printf("(postfix ");
    print_escaped(node_text(arena, node->op));
    printf("\n");
    dump_node(arena, node->left, depth + 2);
    if (node->right != AST_NULL) dump_node(arena, node->right, depth + 2);
    indent(depth);
    printf(")\n");
    break;
  case AST_INFIX: {
    const char *op = node_text(arena, node->op);
    indent(depth);
    if (strcmp(op, " ") == 0)
      printf("(juxt\n");
    else {
      printf("(infix ");
      print_escaped(op);
      printf("\n");
    }
    dump_node(arena, node->left, depth + 2);
    dump_node(arena, node->right, depth + 2);
    indent(depth);
    printf(")\n");
    break;
  }
  }
}

static char *read_file(const char *path, long *len_out) {
  FILE *file = fopen(path, "rb");
  char *buffer;
  long len;
  if (!file) return NULL;
  fseek(file, 0, SEEK_END);
  len = ftell(file);
  fseek(file, 0, SEEK_SET);
  buffer = (char *)malloc((size_t)len + 1);
  if (!buffer) {
    fclose(file);
    return NULL;
  }
  if (fread(buffer, 1, (size_t)len, file) != (size_t)len) {
    free(buffer);
    fclose(file);
    return NULL;
  }
  fclose(file);
  buffer[len] = '\0';
  *len_out = len;
  return buffer;
}

int main(int argc, char **argv) {
  AstArena arena;
  AstNodeId root;
  long len = 0;
  char *source;

  if (argc != 2) {
    fprintf(stderr, "usage: ast_dump <input.lain>\n");
    return 2;
  }

  source = read_file(argv[1], &len);
  if (!source) {
    fprintf(stderr, "failed to read %s\n", argv[1]);
    return 2;
  }

  ast_arena_init(&arena);
  root = ast_parse(&arena, source, (uint32_t)len);
  dump_node(&arena, root, 0);
  ast_arena_destroy(&arena);
  free(source);
  return 0;
}
