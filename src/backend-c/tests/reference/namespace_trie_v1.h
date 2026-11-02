/* Frozen namespace index from match.c at d4dd0f05a968f8e9c9e7186be083d56a26648521.
 * Production blob: 9437f3e4a34c2647db904b74b2bb1c1b9d185886.
 * Test oracle only; keep its semantics independent of the compact index.
 */
/* reversed-label trie for namespace rules:
 * "example.com" is stored as com -> example; a lookup walks the domain's
 * labels right-to-left and matches when it passes through a terminal node
 * at a label boundary. Also covers the exact-match and leading-dot cases
 * of namespace_match. */
typedef struct trie_node {
    char *label;
    struct trie_node **children;
    size_t n_children, cap_children;
    mt_lookup_t child_index;
    bool terminal;
} trie_node_t;

typedef struct ns_trie {
    trie_node_t root;
    bool has_empty_rule; /* namespace rule "" (matches domains ending in '.')*/
} ns_trie_t;

/* Most namespace nodes have only one or two children. Keep those inline in
 * the small vector; allocate a hash table only for a branching node. This
 * bounds lookup work without spending a kilobyte on every DNS label. */
static const trie_node_t *trie_find(const trie_node_t *node, const char *label, size_t len) {
    size_t index;
    if (node->child_index.cap) {
        return mt_lookup_get(&node->child_index, label, len, &index) ? node->children[index] : NULL;
    }
    for (size_t i = 0; i < node->n_children; i++) {
        if (strlen(node->children[i]->label) == len && !memcmp(node->children[i]->label, label, len)) {
            return node->children[i];
        }
    }
    return NULL;
}

static trie_node_t *trie_child(trie_node_t *node, const char *label,
                               size_t len, bool create)
{
    const trie_node_t *found = trie_find(node, label, len);
    if (found) { return (trie_node_t *)found; }
    if (!create) {
        return NULL;
    }
    trie_node_t *child = calloc(1, sizeof(*child));
    if (child == NULL) {
        return NULL;
    }
    child->label = strndup(label, len);
    if (child->label == NULL) {
        free(child);
        return NULL;
    }
    if (node->n_children == node->cap_children) {
        size_t cap = node->cap_children ? node->cap_children * 2 : 8;
        trie_node_t **grown = realloc(node->children, cap * sizeof(*grown));
        if (!grown) { free(child->label); free(child); return NULL; }
        node->children = grown;
        node->cap_children = cap;
    }
    if (node->n_children == 8) {
        for (size_t i = 0; i < node->n_children; i++) {
            const char *old = node->children[i]->label;
            if (mt_lookup_put(&node->child_index, old, strlen(old), i, NULL) != MT_OK) {
                free(child->label); free(child); return NULL;
            }
        }
    }
    if (node->n_children >= 8 &&
        mt_lookup_put(&node->child_index, label, len, node->n_children, NULL) != MT_OK) {
        free(child->label); free(child); return NULL;
    }
    node->children[node->n_children++] = child;
    return child;
}

static void trie_node_clear(trie_node_t *node)
{
    for (size_t i = 0; i < node->n_children; i++) {
        trie_node_clear(node->children[i]);
        free(node->children[i]);
    }
    mt_lookup_clear(&node->child_index);
    free(node->children);
    free(node->label);
}

static mt_err_t ns_trie_insert(ns_trie_t *t, const char *rule)
{
    size_t len = strlen(rule);
    if (len == 0) {
        t->has_empty_rule = true;
        return MT_OK;
    }
    /* walk labels right to left */
    trie_node_t *node = &t->root;
    const char *end = rule + len;
    while (end > rule) {
        const char *start = end;
        while (start > rule && start[-1] != '.') {
            start--;
        }
        node = trie_child(node, start, (size_t)(end - start), true);
        if (node == NULL) {
            return MT_ERR_NOMEM;
        }
        end = start > rule ? start - 1 : rule;
    }
    node->terminal = true;
    return MT_OK;
}

static bool ns_trie_match(const ns_trie_t *t, const char *domain)
{
    size_t len = strlen(domain);
    if (t->has_empty_rule) {
        /* namespace rule "": matches "" or any domain ending with '.' */
        if (len == 0 || domain[len - 1] == '.') {
            return true;
        }
    }
    const trie_node_t *node = &t->root;
    const char *end = domain + len;
    while (end > domain) {
        const char *start = end;
        while (start > domain && start[-1] != '.') {
            start--;
        }
        size_t lab_len = (size_t)(end - start);
        const trie_node_t *next = trie_find(node, start, lab_len);
        if (next == NULL) {
            return false;
        }
        if (next->terminal) {
            /* matched full rule; must be exact or at a dot boundary
             * (start == domain covers exact and ".rule" via empty label) */
            return true;
        }
        node = next;
        end = start > domain ? start - 1 : domain;
    }
    return false;
}

