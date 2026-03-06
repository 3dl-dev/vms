/*
 * rms_idx.c - RMS Indexed (ISAM) File Organization
 *
 * Implements a simple B-tree index for keyed record access.
 * The data file stores variable-length records with a 2-byte
 * length prefix and a 1-byte status (active/deleted).
 * The index is maintained in a separate .rms_idx sidecar file.
 *
 * B-tree Design:
 *   - Each node contains up to BTREE_ORDER-1 keys
 *   - Keys are stored as variable-length byte strings
 *   - Each key entry maps to a record offset in the data file
 *   - Internal nodes have child pointers
 *   - The index file is read/written as a whole (kept in memory)
 *
 * Limitations vs. real VMS ISAM:
 *   - Single index file (not area-based)
 *   - In-memory index (rewritten on flush/close)
 *   - Primary key only for this implementation
 *   - No bucket-level locking
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "rms/rms.h"
#include "rms_internal.h"

/* B-tree order (max children per node) */
#define BTREE_ORDER     64
/* Max keys per node = BTREE_ORDER - 1 */
#define BTREE_MAX_KEYS  (BTREE_ORDER - 1)
/* Max key size we support */
#define MAX_KEY_SIZE     256
/* Index sidecar suffix */
#define IDX_SUFFIX       ".rms_idx"
/* Index file magic */
#define IDX_MAGIC        0x49445831  /* "IDX1" */

/* Data record status */
#define IDX_REC_ACTIVE   0x01
#define IDX_REC_DELETED  0x02

/* B-tree node */
typedef struct btree_node {
    int      num_keys;                      /* Number of keys in this node */
    int      leaf;                          /* 1 if leaf, 0 if internal */
    uint8_t  keys[BTREE_MAX_KEYS][MAX_KEY_SIZE]; /* Key values */
    uint16_t key_lens[BTREE_MAX_KEYS];      /* Key lengths */
    off_t    offsets[BTREE_MAX_KEYS];        /* Data file offsets for each key */
    struct btree_node *children[BTREE_ORDER]; /* Child pointers (internal only) */
} btree_node_t;

/* B-tree root structure (the in-memory index) */
typedef struct {
    btree_node_t *root;
    int           num_records;
    uint16_t      key_size;     /* Total primary key size */
    uint16_t      key_pos;      /* Key position in record */
    uint8_t       key_dtp;      /* Key data type */
    uint16_t      key_flags;    /* Key flags (DUP, CHG, etc.) */
    int           dirty;        /* Index modified since last save */
    char          idx_path[1088]; /* Path to index sidecar file */
} btree_t;

/*
 * Forward declarations for B-tree operations.
 */
static btree_node_t *btree_create_node(int leaf);
static void btree_free_node(btree_node_t *node);
static void btree_free(btree_t *tree);
static int btree_compare_keys(const uint8_t *k1, uint16_t l1,
                              const uint8_t *k2, uint16_t l2,
                              uint8_t dtp);
static int btree_search(btree_node_t *node, const uint8_t *key,
                        uint16_t key_len, uint8_t dtp, off_t *offset);
static int btree_search_gte(btree_node_t *node, const uint8_t *key,
                            uint16_t key_len, uint8_t dtp,
                            off_t *offset, uint8_t *found_key,
                            uint16_t *found_len);
static int btree_insert(btree_t *tree, btree_node_t *node,
                        const uint8_t *key, uint16_t key_len,
                        off_t offset);
static void btree_split_child(btree_node_t *parent, int idx);
static int btree_remove(btree_t *tree, btree_node_t *node,
                        const uint8_t *key, uint16_t key_len,
                        uint8_t dtp);
static int btree_write_node(FILE *f, btree_node_t *node);
static void btree_save(btree_t *tree);
static btree_t *btree_load(const char *data_path, struct FAB *fab);

/*
 * Helper: extract the key from a record buffer based on XABKEY definition.
 */
static void extract_key(const char *record, uint16_t rec_len,
                        struct FAB *fab, uint8_t *key_out,
                        uint16_t *key_len_out)
{
    struct XABKEY *xab = fab->fab$l_xab;
    uint16_t pos = 0;
    uint16_t siz = 0;

    if (xab && xab->xab$b_cod == XAB$C_KEY) {
        pos = xab->xab$w_pos0;
        siz = xab->xab$b_siz0;

        /* Multi-segment key: concatenate all segments */
        uint16_t total = 0;
        const uint16_t *positions = &xab->xab$w_pos0;
        const uint8_t *sizes = &xab->xab$b_siz0;

        for (uint8_t seg = 0; seg < xab->xab$b_nseg && seg < 8; seg++) {
            uint16_t seg_pos = positions[seg];
            uint8_t seg_siz = sizes[seg];
            if (seg_pos + seg_siz <= rec_len && total + seg_siz <= MAX_KEY_SIZE) {
                memcpy(key_out + total, record + seg_pos, seg_siz);
                total += seg_siz;
            }
        }
        *key_len_out = total;
    } else {
        /* No key definition - use first MRS bytes or available data */
        siz = (rec_len < MAX_KEY_SIZE) ? rec_len : MAX_KEY_SIZE;
        if (pos + siz > rec_len) siz = (rec_len > pos) ? (rec_len - pos) : 0;
        memcpy(key_out, record + pos, siz);
        *key_len_out = siz;
    }

    (void)pos;
    (void)siz;
}

/*
 * btree_create_node - Allocate a new B-tree node.
 */
static btree_node_t *btree_create_node(int leaf)
{
    btree_node_t *node = calloc(1, sizeof(btree_node_t));
    if (node) {
        node->leaf = leaf;
        node->num_keys = 0;
    }
    return node;
}

/*
 * btree_free_node - Recursively free a B-tree node and its children.
 */
static void btree_free_node(btree_node_t *node)
{
    if (!node) return;
    if (!node->leaf) {
        for (int i = 0; i <= node->num_keys; i++) {
            btree_free_node(node->children[i]);
        }
    }
    free(node);
}

/*
 * btree_free - Free the entire B-tree.
 */
static void btree_free(btree_t *tree)
{
    if (!tree) return;
    btree_free_node(tree->root);
    free(tree);
}

/*
 * btree_compare_keys - Compare two keys.
 * Returns <0, 0, or >0 (like memcmp/strcmp).
 */
static int btree_compare_keys(const uint8_t *k1, uint16_t l1,
                              const uint8_t *k2, uint16_t l2,
                              uint8_t dtp)
{
    switch (dtp) {
        case XAB$C_IN2: {
            int16_t v1 = 0, v2 = 0;
            if (l1 >= 2) memcpy(&v1, k1, 2);
            if (l2 >= 2) memcpy(&v2, k2, 2);
            return (v1 < v2) ? -1 : (v1 > v2) ? 1 : 0;
        }
        case XAB$C_BN2: {
            uint16_t v1 = 0, v2 = 0;
            if (l1 >= 2) memcpy(&v1, k1, 2);
            if (l2 >= 2) memcpy(&v2, k2, 2);
            return (v1 < v2) ? -1 : (v1 > v2) ? 1 : 0;
        }
        case XAB$C_IN4: {
            int32_t v1 = 0, v2 = 0;
            if (l1 >= 4) memcpy(&v1, k1, 4);
            if (l2 >= 4) memcpy(&v2, k2, 4);
            return (v1 < v2) ? -1 : (v1 > v2) ? 1 : 0;
        }
        case XAB$C_BN4: {
            uint32_t v1 = 0, v2 = 0;
            if (l1 >= 4) memcpy(&v1, k1, 4);
            if (l2 >= 4) memcpy(&v2, k2, 4);
            return (v1 < v2) ? -1 : (v1 > v2) ? 1 : 0;
        }
        case XAB$C_STG:
        default: {
            /* String comparison - compare up to shorter length, then by length */
            uint16_t min_len = (l1 < l2) ? l1 : l2;
            int cmp = memcmp(k1, k2, min_len);
            if (cmp != 0) return cmp;
            return (l1 < l2) ? -1 : (l1 > l2) ? 1 : 0;
        }
    }
}

/*
 * btree_search - Search for an exact key match in the B-tree.
 * Returns 1 if found (offset set), 0 if not found.
 */
static int btree_search(btree_node_t *node, const uint8_t *key,
                        uint16_t key_len, uint8_t dtp, off_t *offset)
{
    if (!node) return 0;

    int i = 0;
    while (i < node->num_keys) {
        int cmp = btree_compare_keys(key, key_len,
                                     node->keys[i], node->key_lens[i], dtp);
        if (cmp == 0) {
            *offset = node->offsets[i];
            return 1;
        }
        if (cmp < 0) break;
        i++;
    }

    if (node->leaf) return 0;
    return btree_search(node->children[i], key, key_len, dtp, offset);
}

/*
 * btree_search_gte - Search for key >= given key (for KGE/KGT operations).
 * Returns 1 if found, 0 if not found.
 */
static int btree_search_gte(btree_node_t *node, const uint8_t *key,
                            uint16_t key_len, uint8_t dtp,
                            off_t *offset, uint8_t *found_key,
                            uint16_t *found_len)
{
    if (!node) return 0;

    int i = 0;
    while (i < node->num_keys) {
        int cmp = btree_compare_keys(key, key_len,
                                     node->keys[i], node->key_lens[i], dtp);
        if (cmp <= 0) {
            /* This key is >= our search key */
            /* But first check if there's something in the left subtree */
            if (!node->leaf) {
                if (btree_search_gte(node->children[i], key, key_len,
                                     dtp, offset, found_key, found_len)) {
                    return 1;
                }
            }
            /* Use this key */
            *offset = node->offsets[i];
            memcpy(found_key, node->keys[i], node->key_lens[i]);
            *found_len = node->key_lens[i];
            return 1;
        }
        i++;
    }

    if (node->leaf) return 0;
    return btree_search_gte(node->children[i], key, key_len,
                            dtp, offset, found_key, found_len);
}

/*
 * btree_split_child - Split a full child node.
 * The parent must not be full.
 */
static void btree_split_child(btree_node_t *parent, int idx)
{
    btree_node_t *child = parent->children[idx];
    int mid = child->num_keys / 2;

    btree_node_t *new_node = btree_create_node(child->leaf);
    if (!new_node) return;

    /* Copy upper half of keys to new node */
    new_node->num_keys = child->num_keys - mid - 1;
    for (int j = 0; j < new_node->num_keys; j++) {
        memcpy(new_node->keys[j], child->keys[mid + 1 + j], child->key_lens[mid + 1 + j]);
        new_node->key_lens[j] = child->key_lens[mid + 1 + j];
        new_node->offsets[j] = child->offsets[mid + 1 + j];
    }

    if (!child->leaf) {
        for (int j = 0; j <= new_node->num_keys; j++) {
            new_node->children[j] = child->children[mid + 1 + j];
        }
    }

    /* Insert median key into parent */
    for (int j = parent->num_keys; j > idx; j--) {
        memcpy(parent->keys[j], parent->keys[j - 1], parent->key_lens[j - 1]);
        parent->key_lens[j] = parent->key_lens[j - 1];
        parent->offsets[j] = parent->offsets[j - 1];
        parent->children[j + 1] = parent->children[j];
    }

    memcpy(parent->keys[idx], child->keys[mid], child->key_lens[mid]);
    parent->key_lens[idx] = child->key_lens[mid];
    parent->offsets[idx] = child->offsets[mid];
    parent->children[idx + 1] = new_node;
    parent->num_keys++;

    child->num_keys = mid;
}

/*
 * btree_insert - Insert a key into the B-tree.
 * Returns 0 on success, -1 on duplicate (if dups not allowed), -2 on error.
 */
static int btree_insert(btree_t *tree, btree_node_t *node,
                        const uint8_t *key, uint16_t key_len,
                        off_t offset)
{
    if (!node) return -2;

    /* Find position for key */
    int i = node->num_keys - 1;
    while (i >= 0 && btree_compare_keys(key, key_len,
                                         node->keys[i], node->key_lens[i],
                                         tree->key_dtp) < 0) {
        i--;
    }

    /* Check for duplicate */
    if (i >= 0 && btree_compare_keys(key, key_len,
                                      node->keys[i], node->key_lens[i],
                                      tree->key_dtp) == 0) {
        if (!(tree->key_flags & XAB$M_DUP)) {
            return -1;  /* Duplicate not allowed */
        }
        /* For duplicates, insert after existing */
    }

    if (node->leaf) {
        /* Insert into leaf */
        i++;
        if (node->num_keys >= BTREE_MAX_KEYS) return -2;

        /* Shift keys right */
        for (int j = node->num_keys; j > i; j--) {
            memcpy(node->keys[j], node->keys[j - 1], node->key_lens[j - 1]);
            node->key_lens[j] = node->key_lens[j - 1];
            node->offsets[j] = node->offsets[j - 1];
        }

        memcpy(node->keys[i], key, key_len);
        if (key_len < MAX_KEY_SIZE) {
            memset(node->keys[i] + key_len, 0, MAX_KEY_SIZE - key_len);
        }
        node->key_lens[i] = key_len;
        node->offsets[i] = offset;
        node->num_keys++;
        tree->dirty = 1;
        return 0;
    }

    /* Internal node: descend to appropriate child */
    i++;
    if (node->children[i]->num_keys >= BTREE_MAX_KEYS) {
        btree_split_child(node, i);
        int cmp = btree_compare_keys(key, key_len,
                                      node->keys[i], node->key_lens[i],
                                      tree->key_dtp);
        if (cmp > 0) i++;
    }

    return btree_insert(tree, node->children[i], key, key_len, offset);
}

/*
 * btree_remove - Remove a key from the B-tree.
 * Simplified: just marks the entry as removed (lazy deletion).
 * Returns 0 if found and removed, -1 if not found.
 */
static int btree_remove(btree_t *tree, btree_node_t *node,
                        const uint8_t *key, uint16_t key_len,
                        uint8_t dtp)
{
    if (!node) return -1;

    int i = 0;
    while (i < node->num_keys) {
        int cmp = btree_compare_keys(key, key_len,
                                     node->keys[i], node->key_lens[i], dtp);
        if (cmp == 0) {
            /* Found - remove by shifting left */
            for (int j = i; j < node->num_keys - 1; j++) {
                memcpy(node->keys[j], node->keys[j + 1], node->key_lens[j + 1]);
                node->key_lens[j] = node->key_lens[j + 1];
                node->offsets[j] = node->offsets[j + 1];
                if (!node->leaf) {
                    node->children[j + 1] = node->children[j + 2];
                }
            }
            node->num_keys--;
            tree->dirty = 1;
            return 0;
        }
        if (cmp < 0) break;
        i++;
    }

    if (node->leaf) return -1;
    return btree_remove(tree, node->children[i], key, key_len, dtp);
}

/*
 * Serialization: write a B-tree node to a FILE.
 */
static int btree_write_node(FILE *f, btree_node_t *node)
{
    if (!node) {
        int32_t marker = -1;
        if (fwrite(&marker, sizeof(marker), 1, f) != 1) return -1;
        return 0;
    }

    int32_t marker = node->num_keys;
    if (fwrite(&marker, sizeof(marker), 1, f) != 1) return -1;
    if (fwrite(&node->leaf, sizeof(node->leaf), 1, f) != 1) return -1;

    for (int i = 0; i < node->num_keys; i++) {
        if (fwrite(&node->key_lens[i], sizeof(uint16_t), 1, f) != 1) return -1;
        if (fwrite(node->keys[i], node->key_lens[i], 1, f) != 1) return -1;
        if (fwrite(&node->offsets[i], sizeof(off_t), 1, f) != 1) return -1;
    }

    if (!node->leaf) {
        for (int i = 0; i <= node->num_keys; i++) {
            if (btree_write_node(f, node->children[i]) < 0) return -1;
        }
    }
    return 0;
}

/*
 * Serialization: read a B-tree node from a FILE.
 */
static btree_node_t *btree_read_node(FILE *f)
{
    int32_t marker;
    if (fread(&marker, sizeof(marker), 1, f) != 1) return NULL;
    if (marker < 0) return NULL;

    btree_node_t *node = btree_create_node(0);
    if (!node) return NULL;

    node->num_keys = marker;
    if (marker > BTREE_MAX_KEYS) {
        free(node);
        return NULL;
    }
    if (fread(&node->leaf, sizeof(node->leaf), 1, f) != 1) {
        free(node);
        return NULL;
    }

    for (int i = 0; i < node->num_keys; i++) {
        if (fread(&node->key_lens[i], sizeof(uint16_t), 1, f) != 1) {
            btree_free_node(node);
            return NULL;
        }
        if (node->key_lens[i] > MAX_KEY_SIZE) {
            btree_free_node(node);
            return NULL;
        }
        if (fread(node->keys[i], node->key_lens[i], 1, f) != 1) {
            btree_free_node(node);
            return NULL;
        }
        if (fread(&node->offsets[i], sizeof(off_t), 1, f) != 1) {
            btree_free_node(node);
            return NULL;
        }
    }

    if (!node->leaf) {
        for (int i = 0; i <= node->num_keys; i++) {
            node->children[i] = btree_read_node(f);
        }
    }

    return node;
}

/*
 * btree_save - Write the index to the sidecar file.
 */
static void btree_save(btree_t *tree)
{
    if (!tree || !tree->dirty) return;

    FILE *f = fopen(tree->idx_path, "wb");
    if (!f) return;

    uint32_t magic = IDX_MAGIC;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&tree->num_records, sizeof(tree->num_records), 1, f);
    fwrite(&tree->key_size, sizeof(tree->key_size), 1, f);
    fwrite(&tree->key_pos, sizeof(tree->key_pos), 1, f);
    fwrite(&tree->key_dtp, sizeof(tree->key_dtp), 1, f);
    fwrite(&tree->key_flags, sizeof(tree->key_flags), 1, f);

    btree_write_node(f, tree->root);
    fclose(f);
    tree->dirty = 0;
}

/*
 * btree_load - Load the index from the sidecar file, or create
 * a new empty tree if no index file exists.
 */
static btree_t *btree_load(const char *data_path, struct FAB *fab)
{
    btree_t *tree = calloc(1, sizeof(btree_t));
    if (!tree) return NULL;

    snprintf(tree->idx_path, sizeof(tree->idx_path),
             "%s%s", data_path, IDX_SUFFIX);

    /* Get key definition from XAB chain */
    struct XABKEY *xab = fab->fab$l_xab;
    if (xab && xab->xab$b_cod == XAB$C_KEY) {
        tree->key_size = xab->xab$b_siz0;
        tree->key_pos = xab->xab$w_pos0;
        tree->key_dtp = xab->xab$b_dtp;
        tree->key_flags = xab->xab$w_flg;
        /* Compute total key size from all segments */
        uint16_t tks = 0;
        const uint8_t *sizes = &xab->xab$b_siz0;
        for (uint8_t s = 0; s < xab->xab$b_nseg && s < 8; s++) {
            tks += sizes[s];
        }
        if (tks > 0) tree->key_size = tks;
    }

    /* Try to load existing index */
    FILE *f = fopen(tree->idx_path, "rb");
    if (f) {
        uint32_t magic;
        if (fread(&magic, sizeof(magic), 1, f) == 1 && magic == IDX_MAGIC) {
            if (fread(&tree->num_records, sizeof(tree->num_records), 1, f) != 1 ||
                fread(&tree->key_size, sizeof(tree->key_size), 1, f) != 1 ||
                fread(&tree->key_pos, sizeof(tree->key_pos), 1, f) != 1 ||
                fread(&tree->key_dtp, sizeof(tree->key_dtp), 1, f) != 1 ||
                fread(&tree->key_flags, sizeof(tree->key_flags), 1, f) != 1) {
                fclose(f);
                tree->root = NULL;
                return tree;
            }
            tree->root = btree_read_node(f);
        }
        fclose(f);
    }

    if (!tree->root) {
        tree->root = btree_create_node(1);  /* Empty leaf */
    }

    return tree;
}

/*
 * get_tree - Get or create the B-tree index for a FAB.
 * The tree is stored in fab->_rms_state.
 */
static btree_t *get_tree(struct FAB *fab)
{
    if (fab->_rms_state) {
        return (btree_t *)fab->_rms_state;
    }

    btree_t *tree = btree_load(fab->_resolved_path, fab);
    fab->_rms_state = tree;
    return tree;
}

/* Use shared rms_read_exact / rms_write_exact from rms_core.c */
#define idx_read_exact  rms_read_exact
#define idx_write_exact rms_write_exact

/*
 * rms_idx_get - Read a record from an indexed file.
 *
 * Access modes:
 *   RAB$C_KEY: Look up by key in rab$l_kbf/rab$b_ksz.
 *              Supports KGE (key >=) and KGT (key >) via rab$l_rop.
 *   RAB$C_SEQ: Sequential scan through the data file,
 *              skipping deleted records.
 *
 * Returns:
 *   RMS$_NORMAL - Record found and read
 *   RMS$_RNF    - Record not found
 *   RMS$_EOF    - End of file
 *   RMS$_KEY    - Invalid key
 *   RMS$_RTB    - Record too big
 */
uint32_t rms_idx_get(struct FAB *fab, struct RAB *rab)
{
    int fd = fab->_linux_fd;
    if (fd < 0) return RMS$_ACC;
    if (!rab->rab$l_ubf || rab->rab$w_usz == 0) return RMS$_RAB;

    btree_t *tree = get_tree(fab);
    if (!tree) return RMS$_DME;

    if (rab->rab$b_rac == RAB$C_KEY) {
        /* Keyed access */
        if (!rab->rab$l_kbf || rab->rab$b_ksz == 0) return RMS$_KEY;

        off_t rec_offset = 0;
        int found = 0;

        if (rab->rab$l_rop & (RAB$M_KGE | RAB$M_KGT)) {
            /* Key >= or Key > search */
            uint8_t found_key[MAX_KEY_SIZE];
            uint16_t found_len = 0;

            found = btree_search_gte(tree->root,
                                     (const uint8_t *)rab->rab$l_kbf,
                                     rab->rab$b_ksz, tree->key_dtp,
                                     &rec_offset, found_key, &found_len);

            if (found && (rab->rab$l_rop & RAB$M_KGT)) {
                /* For KGT, the found key must be strictly greater */
                if (btree_compare_keys((const uint8_t *)rab->rab$l_kbf,
                                       rab->rab$b_ksz,
                                       found_key, found_len,
                                       tree->key_dtp) == 0) {
                    /* Exact match found but we want strictly greater.
                     * This simplified implementation does not handle
                     * this case perfectly - return RNF for now. */
                    return RMS$_RNF;
                }
            }
        } else {
            /* Exact key match */
            found = btree_search(tree->root,
                                 (const uint8_t *)rab->rab$l_kbf,
                                 rab->rab$b_ksz, tree->key_dtp,
                                 &rec_offset);
        }

        if (!found) return RMS$_RNF;

        /* Read the record from the data file at the found offset */
        lseek(fd, rec_offset, SEEK_SET);

        uint8_t status;
        if (read(fd, &status, 1) != 1) return RMS$_RER;
        if (status != IDX_REC_ACTIVE) return RMS$_DEL;

        uint16_t reclen;
        if (idx_read_exact(fd, &reclen, 2) < 2) return RMS$_RER;

        if (reclen > rab->rab$w_usz) {
            rab->rab$l_stv = reclen;
            return RMS$_RTB;
        }

        if (reclen > 0) {
            if (idx_read_exact(fd, rab->rab$l_ubf, reclen) < reclen) {
                return RMS$_RER;
            }
        }

        rab->rab$w_rsz = reclen;
        rab->_last_rec_offset = rec_offset;
        rab->_last_rec_size = reclen;
        rab->_current_offset = lseek(fd, 0, SEEK_CUR);
        return RMS$_NORMAL;

    } else {
        /* Sequential access - scan data file */
        lseek(fd, rab->_current_offset, SEEK_SET);

        for (;;) {
            off_t pos = lseek(fd, 0, SEEK_CUR);

            uint8_t status;
            ssize_t n = read(fd, &status, 1);
            if (n <= 0) return RMS$_EOF;

            uint16_t reclen;
            if (idx_read_exact(fd, &reclen, 2) < 2) return RMS$_EOF;

            if (status == IDX_REC_ACTIVE) {
                if (reclen > rab->rab$w_usz) {
                    /* Skip this record */
                    lseek(fd, reclen, SEEK_CUR);
                    rab->_current_offset = lseek(fd, 0, SEEK_CUR);
                    continue;
                }

                if (reclen > 0) {
                    if (idx_read_exact(fd, rab->rab$l_ubf, reclen) < reclen) {
                        return RMS$_RER;
                    }
                }

                rab->rab$w_rsz = reclen;
                rab->_last_rec_offset = pos;
                rab->_last_rec_size = reclen;
                rab->_current_offset = lseek(fd, 0, SEEK_CUR);
                return RMS$_NORMAL;
            }

            /* Skip deleted/inactive record */
            lseek(fd, reclen, SEEK_CUR);
        }
    }
}

/*
 * rms_idx_put - Write a record to an indexed file.
 *
 * Appends the record to the data file, then inserts the key
 * into the B-tree index. For duplicate keys, behavior depends
 * on the XAB$M_DUP flag.
 *
 * Data file format per record:
 *   [1-byte status] [2-byte length] [record data]
 *
 * Returns:
 *   RMS$_NORMAL  - Record written
 *   RMS$_DUP     - Duplicate key (if dups not allowed)
 *   RMS$_WER     - Write error
 *   RMS$_DME     - Memory error
 */
uint32_t rms_idx_put(struct FAB *fab, struct RAB *rab)
{
    int fd = fab->_linux_fd;
    if (fd < 0) return RMS$_ACC;

    char *buf = rab->rab$l_rbf ? rab->rab$l_rbf : rab->rab$l_ubf;
    if (!buf) return RMS$_RAB;
    uint16_t len = rab->rab$w_rsz;

    btree_t *tree = get_tree(fab);
    if (!tree) return RMS$_DME;

    /* Extract key from record */
    uint8_t key[MAX_KEY_SIZE];
    uint16_t key_len = 0;
    extract_key(buf, len, fab, key, &key_len);

    if (key_len == 0) return RMS$_KEY;

    /* Append record to data file */
    off_t rec_offset = lseek(fd, 0, SEEK_END);

    uint8_t status = IDX_REC_ACTIVE;
    if (idx_write_exact(fd, &status, 1) < 0) return RMS$_WER;
    if (idx_write_exact(fd, &len, 2) < 0) return RMS$_WER;
    if (len > 0) {
        if (idx_write_exact(fd, buf, len) < 0) return RMS$_WER;
    }

    /* Insert key into B-tree */
    /* Handle root split if needed */
    if (tree->root->num_keys >= BTREE_MAX_KEYS) {
        btree_node_t *new_root = btree_create_node(0);
        if (!new_root) return RMS$_DME;
        new_root->children[0] = tree->root;
        btree_split_child(new_root, 0);
        tree->root = new_root;
    }

    int rc = btree_insert(tree, tree->root, key, key_len, rec_offset);
    if (rc == -1) {
        /* Duplicate key - we already wrote the record, so mark it deleted */
        lseek(fd, rec_offset, SEEK_SET);
        uint8_t del = IDX_REC_DELETED;
        idx_write_exact(fd, &del, 1);
        return RMS$_DUP;
    }

    tree->num_records++;
    tree->dirty = 1;

    /* Save index periodically (every 100 inserts) or on dirty flag */
    if (tree->num_records % 100 == 0) {
        btree_save(tree);
    }

    rab->_last_rec_offset = rec_offset;
    rab->_last_rec_size = len;
    rab->_current_offset = lseek(fd, 0, SEEK_CUR);

    return RMS$_NORMAL;
}

/*
 * rms_idx_delete - Delete a record from an indexed file.
 *
 * Marks the current record as deleted in the data file and
 * removes its key from the B-tree index.
 *
 * Returns:
 *   RMS$_NORMAL - Record deleted
 *   RMS$_CUR    - No current record
 *   RMS$_WER    - Write error
 */
uint32_t rms_idx_delete(struct FAB *fab, struct RAB *rab)
{
    int fd = fab->_linux_fd;
    if (fd < 0) return RMS$_ACC;

    if (rab->_last_rec_offset == 0 && rab->_last_rec_size == 0) {
        return RMS$_CUR;
    }

    btree_t *tree = get_tree(fab);
    if (!tree) return RMS$_DME;

    /* Read the record to get its key (for B-tree removal) */
    lseek(fd, rab->_last_rec_offset + 1, SEEK_SET);  /* Skip status */
    uint16_t reclen;
    if (idx_read_exact(fd, &reclen, 2) < 2) return RMS$_RER;

    char *rec_buf = NULL;
    if (reclen > 0) {
        rec_buf = malloc(reclen);
        if (!rec_buf) return RMS$_DME;
        if (idx_read_exact(fd, rec_buf, reclen) < reclen) {
            free(rec_buf);
            return RMS$_RER;
        }
    }

    /* Extract key and remove from index */
    if (rec_buf) {
        uint8_t key[MAX_KEY_SIZE];
        uint16_t key_len = 0;
        extract_key(rec_buf, reclen, fab, key, &key_len);
        free(rec_buf);

        if (key_len > 0) {
            btree_remove(tree, tree->root, key, key_len, tree->key_dtp);
        }
    }

    /* Mark record as deleted in data file */
    lseek(fd, rab->_last_rec_offset, SEEK_SET);
    uint8_t del = IDX_REC_DELETED;
    if (idx_write_exact(fd, &del, 1) < 0) return RMS$_WER;

    tree->num_records--;
    tree->dirty = 1;

    return RMS$_NORMAL;
}

/*
 * rms_idx_update - Update the current record in an indexed file.
 *
 * If the primary key has not changed, rewrites the record in place.
 * If the key changed (and CHG flag is set), removes old entry
 * and inserts new.
 *
 * Returns:
 *   RMS$_NORMAL - Record updated
 *   RMS$_CUR    - No current record
 *   RMS$_CHG    - Key value changed but CHG not permitted
 *   RMS$_WER    - Write error
 */
uint32_t rms_idx_update(struct FAB *fab, struct RAB *rab)
{
    int fd = fab->_linux_fd;
    if (fd < 0) return RMS$_ACC;

    char *buf = rab->rab$l_rbf ? rab->rab$l_rbf : rab->rab$l_ubf;
    if (!buf) return RMS$_RAB;

    if (rab->_last_rec_offset == 0 && rab->_last_rec_size == 0) {
        return RMS$_CUR;
    }

    btree_t *tree = get_tree(fab);
    if (!tree) return RMS$_DME;

    uint16_t new_len = rab->rab$w_rsz;

    /* Extract new key */
    uint8_t new_key[MAX_KEY_SIZE];
    uint16_t new_key_len = 0;
    extract_key(buf, new_len, fab, new_key, &new_key_len);

    /* Read old record to get old key */
    lseek(fd, rab->_last_rec_offset + 1, SEEK_SET);
    uint16_t old_len;
    if (idx_read_exact(fd, &old_len, 2) < 2) return RMS$_RER;

    char *old_buf = malloc(old_len > 0 ? old_len : 1);
    if (!old_buf) return RMS$_DME;
    if (old_len > 0) {
        if (idx_read_exact(fd, old_buf, old_len) < old_len) {
            free(old_buf);
            return RMS$_RER;
        }
    }

    uint8_t old_key[MAX_KEY_SIZE];
    uint16_t old_key_len = 0;
    extract_key(old_buf, old_len, fab, old_key, &old_key_len);
    free(old_buf);

    int key_changed = btree_compare_keys(old_key, old_key_len,
                                          new_key, new_key_len,
                                          tree->key_dtp) != 0;

    if (key_changed && !(tree->key_flags & XAB$M_CHG)) {
        return RMS$_CHG;
    }

    if (key_changed) {
        /* Delete old, insert new (as delete + put) */
        btree_remove(tree, tree->root, old_key, old_key_len, tree->key_dtp);

        /* Mark old record as deleted */
        lseek(fd, rab->_last_rec_offset, SEEK_SET);
        uint8_t del = IDX_REC_DELETED;
        idx_write_exact(fd, &del, 1);

        /* Append new record */
        off_t new_offset = lseek(fd, 0, SEEK_END);
        uint8_t status = IDX_REC_ACTIVE;
        idx_write_exact(fd, &status, 1);
        idx_write_exact(fd, &new_len, 2);
        if (new_len > 0) idx_write_exact(fd, buf, new_len);

        /* Handle root split */
        if (tree->root->num_keys >= BTREE_MAX_KEYS) {
            btree_node_t *new_root = btree_create_node(0);
            if (!new_root) return RMS$_DME;
            new_root->children[0] = tree->root;
            btree_split_child(new_root, 0);
            tree->root = new_root;
        }

        btree_insert(tree, tree->root, new_key, new_key_len, new_offset);
        rab->_last_rec_offset = new_offset;
    } else {
        /* Key unchanged - can we rewrite in place? */
        if (new_len == old_len) {
            /* Same size - rewrite in place */
            lseek(fd, rab->_last_rec_offset + 3, SEEK_SET);  /* skip status + len */
            if (idx_write_exact(fd, buf, new_len) < 0) return RMS$_WER;
        } else {
            /* Different size - delete and append */
            lseek(fd, rab->_last_rec_offset, SEEK_SET);
            uint8_t del = IDX_REC_DELETED;
            idx_write_exact(fd, &del, 1);

            off_t new_offset = lseek(fd, 0, SEEK_END);
            uint8_t status = IDX_REC_ACTIVE;
            idx_write_exact(fd, &status, 1);
            idx_write_exact(fd, &new_len, 2);
            if (new_len > 0) idx_write_exact(fd, buf, new_len);

            /* Update B-tree offset */
            btree_remove(tree, tree->root, new_key, new_key_len, tree->key_dtp);

            if (tree->root->num_keys >= BTREE_MAX_KEYS) {
                btree_node_t *new_root = btree_create_node(0);
                if (!new_root) return RMS$_DME;
                new_root->children[0] = tree->root;
                btree_split_child(new_root, 0);
                tree->root = new_root;
            }

            btree_insert(tree, tree->root, new_key, new_key_len, new_offset);
            rab->_last_rec_offset = new_offset;
        }
    }

    tree->dirty = 1;
    rab->_last_rec_size = new_len;
    return RMS$_NORMAL;
}

/*
 * rms_idx_find - Position to a record by key without reading data.
 *
 * Returns:
 *   RMS$_NORMAL - Positioned to record
 *   RMS$_RNF    - Key not found
 *   RMS$_KEY    - Invalid key
 */
uint32_t rms_idx_find(struct FAB *fab, struct RAB *rab)
{
    int fd = fab->_linux_fd;
    if (fd < 0) return RMS$_ACC;

    btree_t *tree = get_tree(fab);
    if (!tree) return RMS$_DME;

    if (!rab->rab$l_kbf || rab->rab$b_ksz == 0) return RMS$_KEY;

    off_t rec_offset = 0;
    int found;

    if (rab->rab$l_rop & (RAB$M_KGE | RAB$M_KGT)) {
        uint8_t found_key[MAX_KEY_SIZE];
        uint16_t found_len = 0;
        found = btree_search_gte(tree->root,
                                 (const uint8_t *)rab->rab$l_kbf,
                                 rab->rab$b_ksz, tree->key_dtp,
                                 &rec_offset, found_key, &found_len);
    } else {
        found = btree_search(tree->root,
                             (const uint8_t *)rab->rab$l_kbf,
                             rab->rab$b_ksz, tree->key_dtp,
                             &rec_offset);
    }

    if (!found) return RMS$_RNF;

    rab->_last_rec_offset = rec_offset;
    rab->_current_offset = rec_offset;

    /* Read the record length to set _last_rec_size */
    lseek(fd, rec_offset + 1, SEEK_SET);  /* Skip status */
    uint16_t reclen;
    if (idx_read_exact(fd, &reclen, 2) >= 2) {
        rab->_last_rec_size = reclen;
    }

    return RMS$_NORMAL;
}

/*
 * rms_idx_flush - Save the in-memory B-tree index to disk.
 * Called from sys$flush and sys$close.
 */
void rms_idx_flush(struct FAB *fab)
{
    if (fab->_rms_state) {
        btree_save((btree_t *)fab->_rms_state);
    }
}

/*
 * rms_idx_cleanup - Free the in-memory B-tree.
 * Called when the file is closed.
 */
void rms_idx_cleanup(struct FAB *fab)
{
    if (fab->_rms_state) {
        btree_t *tree = (btree_t *)fab->_rms_state;
        btree_save(tree);  /* Save before cleanup */
        btree_free(tree);
        fab->_rms_state = NULL;
    }
}
