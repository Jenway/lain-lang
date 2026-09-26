/* lainmeta/tree.h 的实现。 */
#include "lainmeta/tree.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  LainMetaTreeNode info;
  uint64_t *children;
  char *generated_text;
  uint64_t child_bytes;
  uint64_t text_bytes;
} TreeNode;

struct LainMetaTree {
  char *source;
  uint32_t source_length;
  uint32_t source_index;
  TreeNode *nodes;
  uint32_t count;
  uint32_t capacity;
  uint64_t root;
  LainVmQuota *quota;
  uint64_t source_bytes;
  uint64_t node_bytes;
};

static void *tree_alloc(LainVmQuota *quota, size_t bytes) {
  void *memory;
  if (lainvm_quota_charge(quota, bytes) != 0) return NULL;
  memory = malloc(bytes);
  if (!memory) (void)lainvm_quota_release(quota, bytes);
  return memory;
}

static void tree_release(LainVmQuota *quota, void *memory, uint64_t bytes) {
  if (!memory) return;
  free(memory);
  (void)lainvm_quota_release(quota, bytes);
}

static bool valid(const LainMetaTree *tree, uint64_t handle) {
  return tree && handle > 0 && handle <= tree->count;
}

static uint64_t append(LainMetaTree *tree, TreeNode *node) {
  TreeNode *grown;
  uint32_t next;
  if (tree->count == UINT32_MAX) return 0;
  if (tree->count == tree->capacity) {
    uint64_t old_bytes, new_bytes, grow;
    next = tree->capacity ? tree->capacity * 2u : 32u;
    if (next <= tree->capacity) return 0;
    old_bytes = tree->node_bytes;
    new_bytes = (uint64_t)sizeof(*grown) * next;
    grow = new_bytes - old_bytes;
    if (lainvm_quota_charge(tree->quota, grow) != 0) return 0;
    grown = (TreeNode *)realloc(tree->nodes, sizeof(*grown) * next);
    if (!grown) {
      (void)lainvm_quota_release(tree->quota, grow);
      return 0;
    }
    tree->nodes = grown;
    tree->capacity = next;
    tree->node_bytes = new_bytes;
  }
  tree->nodes[tree->count] = *node;
  return ++tree->count;
}

static unsigned char closing(unsigned char open) {
  if (open == '(') return ')';
  if (open == '[') return ']';
  if (open == '{') return '}';
  return 0;
}

static bool word(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c >= 128;
}

static uint32_t token_end(const char *text, uint32_t length, uint32_t at) {
  unsigned char c = (unsigned char)text[at];
  uint32_t next = at + 1u;
  if (word(c)) {
    while (next < length && word((unsigned char)text[next])) next++;
  } else if (c == '"') {
    while (next < length) {
      if (text[next] == '\\' && next + 1u < length) {
        next += 2u;
        continue;
      }
      if (text[next++] == '"') break;
    }
  } else if (next < length &&
             ((c == ':' && text[next] == ':') ||
              (c == '-' && text[next] == '>') ||
              (c == '.' && text[next] == '.'))) {
    next++;
  }
  return next;
}

static bool add_child(LainMetaTree *tree, uint64_t **children, uint32_t *count,
                      uint32_t *capacity, uint64_t child) {
  uint64_t *grown;
  uint32_t next;
  if (*count == *capacity) {
    next = *capacity ? *capacity * 2u : 8u;
    if (next <= *capacity) return false;
    if (lainvm_quota_charge(tree->quota,
                            (uint64_t)(next - *capacity) * sizeof(*grown)) != 0)
      return false;
    grown = (uint64_t *)realloc(*children, sizeof(*grown) * next);
    if (!grown) {
      (void)lainvm_quota_release(
          tree->quota, (uint64_t)(next - *capacity) * sizeof(*grown));
      return false;
    }
    *children = grown;
    *capacity = next;
  }
  (*children)[(*count)++] = child;
  return true;
}

static uint64_t parse_group(LainMetaTree *tree, uint32_t *at,
                            unsigned char open, uint32_t depth,
                            uint32_t *error_offset) {
  uint64_t *children = NULL, handle = 0, child;
  uint32_t count = 0, capacity = 0, start = *at;
  bool matched = false;
  TreeNode node;
  if (depth > 256) goto error;
  if (open) ++*at;
  while (*at < tree->source_length) {
    unsigned char c = (unsigned char)tree->source[*at];
    uint32_t end;
    if (c <= 32) {
      ++*at;
      continue;
    }
    if (c == '/' && *at + 1u < tree->source_length &&
        tree->source[*at + 1u] == '/') {
      *at += 2u;
      while (*at < tree->source_length && tree->source[*at] != '\n') ++*at;
      continue;
    }
    if (c == '/' && *at + 1u < tree->source_length &&
        tree->source[*at + 1u] == '*') {
      uint32_t comment_start = *at;
      *at += 2u;
      while (*at + 1u < tree->source_length &&
             !(tree->source[*at] == '*' && tree->source[*at + 1u] == '/'))
        ++*at;
      if (*at + 1u >= tree->source_length) {
        *at = comment_start;
        goto error;
      }
      *at += 2u;
      continue;
    }
    if (c == ')' || c == ']' || c == '}') {
      if (!open || c != closing(open)) goto error;
      ++*at;
      matched = true;
      break;
    }
    if (closing(c)) {
      child = parse_group(tree, at, c, depth + 1u, error_offset);
      if (!child) goto cleanup;
    } else {
      memset(&node, 0, sizeof(node));
      end = token_end(tree->source, tree->source_length, *at);
      node.info.kind = LAINMETA_TREE_TOKEN;
      node.info.start = *at;
      node.info.length = end - *at;
      node.info.source_index = tree->source_index;
      child = append(tree, &node);
      if (!child) goto cleanup;
      *at = end;
    }
    if (!add_child(tree, &children, &count, &capacity, child)) goto cleanup;
  }
  if (open && !matched) goto error;
  memset(&node, 0, sizeof(node));
  node.info.kind = LAINMETA_TREE_GROUP;
  node.info.start = start;
  node.info.length = open ? *at - start : tree->source_length;
  node.info.source_index = tree->source_index;
  node.info.delimiter = open;
  node.info.child_count = count;
  node.children = children;
  node.child_bytes = (uint64_t)capacity * sizeof(*children);
  handle = append(tree, &node);
  if (handle) return handle;
cleanup:
  tree_release(tree->quota, children,
               (uint64_t)capacity * sizeof(*children));
  return 0;
error:
  if (error_offset) *error_offset = *at;
  goto cleanup;
}

LainMetaTree *lainmeta_tree_parse(uint32_t source_index, const char *text,
                                  uint32_t length, uint32_t *error_offset) {
  return lainmeta_tree_parse_with_quota(source_index, text, length,
                                        error_offset, NULL);
}

LainMetaTree *lainmeta_tree_parse_with_quota(uint32_t source_index,
                                             const char *text, uint32_t length,
                                             uint32_t *error_offset,
                                             LainVmQuota *quota) {
  LainMetaTree *tree;
  uint32_t at = 0;
  if (!text || length == UINT32_MAX) return NULL;
  tree = (LainMetaTree *)tree_alloc(quota, sizeof(*tree));
  if (!tree) return NULL;
  memset(tree, 0, sizeof(*tree));
  tree->quota = quota;
  tree->source_bytes = (uint64_t)length + 1u;
  tree->source = (char *)tree_alloc(quota, (size_t)tree->source_bytes);
  if (!tree->source) {
    tree_release(quota, tree, sizeof(*tree));
    return NULL;
  }
  memcpy(tree->source, text, length);
  tree->source[length] = '\0';
  tree->source_length = length;
  tree->source_index = source_index;
  tree->root = parse_group(tree, &at, 0, 0, error_offset);
  if (!tree->root) {
    lainmeta_tree_free(tree);
    return NULL;
  }
  return tree;
}

void lainmeta_tree_free(LainMetaTree *tree) {
  uint32_t i;
  if (!tree) return;
  for (i = 0; i < tree->count; i++) {
    tree_release(tree->quota, tree->nodes[i].children,
                 tree->nodes[i].child_bytes);
    tree_release(tree->quota, tree->nodes[i].generated_text,
                 tree->nodes[i].text_bytes);
  }
  tree_release(tree->quota, tree->nodes, tree->node_bytes);
  tree_release(tree->quota, tree->source, tree->source_bytes);
  tree_release(tree->quota, tree, sizeof(*tree));
}

uint64_t lainmeta_tree_root(const LainMetaTree *tree) {
  return tree ? tree->root : 0;
}

bool lainmeta_tree_node(const LainMetaTree *tree, uint64_t handle,
                        LainMetaTreeNode *out) {
  if (!valid(tree, handle) || !out) return false;
  *out = tree->nodes[handle - 1u].info;
  return true;
}

uint64_t lainmeta_tree_child(const LainMetaTree *tree, uint64_t parent,
                             uint32_t index) {
  const TreeNode *node;
  if (!valid(tree, parent)) return 0;
  node = &tree->nodes[parent - 1u];
  return index < node->info.child_count ? node->children[index] : 0;
}

const char *lainmeta_tree_text(const LainMetaTree *tree, uint64_t handle,
                               uint32_t *length_out) {
  const TreeNode *node;
  if (length_out) *length_out = 0;
  if (!valid(tree, handle)) return NULL;
  node = &tree->nodes[handle - 1u];
  if (node->info.kind != LAINMETA_TREE_TOKEN) return NULL;
  if (length_out) *length_out = node->info.length;
  return node->generated_text ? node->generated_text
                              : tree->source + node->info.start;
}

uint64_t lainmeta_tree_make_token(LainMetaTree *tree, const char *text,
                                  uint32_t length, uint64_t origin) {
  TreeNode node;
  uint64_t handle;
  if (!tree || !text || length == UINT32_MAX ||
      (origin && !valid(tree, origin))) return 0;
  memset(&node, 0, sizeof(node));
  node.text_bytes = (uint64_t)length + 1u;
  node.generated_text = (char *)tree_alloc(tree->quota,
                                          (size_t)node.text_bytes);
  if (!node.generated_text) return 0;
  memcpy(node.generated_text, text, length);
  node.generated_text[length] = '\0';
  node.info.kind = LAINMETA_TREE_TOKEN;
  node.info.source_index = origin ? tree->nodes[origin - 1u].info.source_index
                                  : UINT32_MAX;
  node.info.origin = origin;
  node.info.length = length;
  handle = append(tree, &node);
  if (!handle) tree_release(tree->quota, node.generated_text,
                            node.text_bytes);
  return handle;
}

uint64_t lainmeta_tree_make_group(LainMetaTree *tree, unsigned char delimiter,
                                  const uint64_t *children, uint32_t count,
                                  uint64_t origin) {
  TreeNode node;
  uint32_t i;
  uint64_t handle;
  if (!tree || (delimiter && !closing(delimiter)) ||
      (origin && !valid(tree, origin)) || (count && !children)) return 0;
  for (i = 0; i < count; i++) if (!valid(tree, children[i])) return 0;
  memset(&node, 0, sizeof(node));
  if (count) {
    node.child_bytes = (uint64_t)sizeof(*children) * count;
    node.children = (uint64_t *)tree_alloc(tree->quota,
                                          (size_t)node.child_bytes);
    if (!node.children) return 0;
    memcpy(node.children, children, sizeof(*children) * count);
  }
  node.info.kind = LAINMETA_TREE_GROUP;
  node.info.delimiter = delimiter;
  node.info.child_count = count;
  node.info.source_index = origin ? tree->nodes[origin - 1u].info.source_index
                                  : UINT32_MAX;
  node.info.origin = origin;
  handle = append(tree, &node);
  if (!handle) tree_release(tree->quota, node.children, node.child_bytes);
  return handle;
}

uint64_t lainmeta_tree_replace_child(LainMetaTree *tree, uint64_t group,
                                     uint32_t index, uint64_t replacement) {
  const TreeNode *old;
  uint64_t *children, handle;
  uint32_t count;
  unsigned char delimiter;
  if (!valid(tree, group) || !valid(tree, replacement)) return 0;
  old = &tree->nodes[group - 1u];
  if (old->info.kind != LAINMETA_TREE_GROUP || index >= old->info.child_count)
    return 0;
  count = old->info.child_count;
  delimiter = old->info.delimiter;
  children = (uint64_t *)tree_alloc(tree->quota,
                                    (size_t)sizeof(*children) * count);
  if (!children) return 0;
  memcpy(children, old->children, sizeof(*children) * count);
  children[index] = replacement;
  handle = lainmeta_tree_make_group(tree, delimiter, children, count, group);
  tree_release(tree->quota, children, (uint64_t)sizeof(*children) * count);
  return handle;
}
