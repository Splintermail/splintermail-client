/*
  Andersson tree library

    > Created (Julienne Walker): September 10, 2005
    > Corrections (James Bucanek): April 10, 2006
    > API Modified (Splintermail Dev) December 2, 2018

  This code is in the public domain. Anyone may
  use it or change it in any way that they see
  fit. The author assumes no responsibility for
  damages incurred through use of the original
  code or any variations thereof.

  It is requested, but not required, that due
  credit is given to the original author and
  anyone who has modified the code through
  a header comment, such as this one.
*/

// some precanned cmp_f's
int jsw_cmp_dstr(const void *a, const void *b);
int jsw_cmp_int(const void *a, const void *b);
int jsw_cmp_uint(const void *a, const void *b);
int jsw_cmp_ulong(const void *a, const void *b);

#define JSW_AHEIGHT_LIMIT 64 /* Tallest allowable tree */

typedef struct jsw_anode {
  int               level;   /* Horizontal level for balance */
  struct jsw_anode *link[2]; /* Left (0) and right (1) links */
  size_t count;              /* number of nodes in subtree */
} jsw_anode_t;

// cmp_f will compare two keys
typedef int (*cmp_f) ( const void*, const void* );
// get_f will fetch a key for the object that owns the node
typedef const void *(*get_f) ( const jsw_anode_t* );

typedef struct jsw_atree {
  jsw_anode_t *root; /* Top of the tree */
  jsw_anode_t  nil;  /* End of tree sentinel */
  cmp_f        cmp;  /* Compare two item values (user-defined) */
  get_f        get;  /* Get the value from a node (user-defined) */
  size_t       size; /* Number of items */
} jsw_atree_t;

typedef struct jsw_atrav {
  jsw_atree_t *tree;                    /* Paired tree */
  jsw_anode_t *it;                      /* Current node */
  jsw_anode_t *path[JSW_AHEIGHT_LIMIT]; /* Traversal path */
  size_t       top;                     /* Top of stack */
} jsw_atrav_t;

/* Andersson tree functions */
void         jsw_ainit ( jsw_atree_t *tree, cmp_f cmp, get_f get );
jsw_anode_t *jsw_afind ( jsw_atree_t *tree, const void *val, size_t *idx );
void         jsw_ainsert ( jsw_atree_t *tree, jsw_anode_t *node );
size_t       jsw_asize ( jsw_atree_t *tree );

// find a node with an alternate comparison function (second param will be val)
/* (you still have to compare against the sorted field of each node, but you
    might have a novel way to provide the key to compare) */
jsw_anode_t *jsw_afind_ex ( jsw_atree_t *tree, cmp_f alt_cmp, const void *val, size_t *idx );

// remove a node by value, returns the node if the value was found
jsw_anode_t *jsw_aerase ( jsw_atree_t *tree, const void *val );

/* The reason a function like this doesn't exist is because the tree does not
   have parent node pointers, so removing a random element is actually quite
   difficult.  (well... you could just call jsw_aerase(tree, tree->get(node)),
   it would be exactly the same) */
// remove a node by reference
// void         jsw_aremove ( jsw_atree_t *tree, jsw_anode_t *node);

// pop any element (actually the root node), or return null if tree is empty
jsw_anode_t *jsw_apop ( jsw_atree_t *tree );

// index into tree
jsw_anode_t *jsw_aindex ( jsw_atree_t *tree, size_t idx );

/* Traversal functions */
jsw_anode_t *jsw_atfirst ( jsw_atrav_t *trav, jsw_atree_t *tree );
jsw_anode_t *jsw_atlast ( jsw_atrav_t *trav, jsw_atree_t *tree );
jsw_anode_t *jsw_atnext ( jsw_atrav_t *trav );
jsw_anode_t *jsw_atprev ( jsw_atrav_t *trav );

/* traverse from an aribtrary node that is already in the tree */
jsw_anode_t *jsw_atnode ( jsw_atrav_t *trav, jsw_atree_t *tree, jsw_anode_t *node );

/* planned extensions, for iterating through subsections of the tree:
jsw_anode_t *jsw_at_lt ();
jsw_anode_t *jsw_at_le ();
jsw_anode_t *jsw_at_gt ();
jsw_anode_t *jsw_at_ge ();
jsw_anode_t *jsw_at_lt_ex ();
jsw_anode_t *jsw_at_le_ex ();
jsw_anode_t *jsw_at_gt_ex ();
jsw_anode_t *jsw_at_ge_ex ();
*/

/* like atnext/atprev except the current value is removed from the tree */
jsw_anode_t *jsw_pop_atnext( jsw_atrav_t *trav );
jsw_anode_t *jsw_pop_atprev( jsw_atrav_t *trav );

// jsw-friendly wrapper types
typedef struct {
    dstr_t dstr;
    jsw_anode_t node;
} jsw_str_t;
DEF_CONTAINER_OF(jsw_str_t, node, jsw_anode_t);

const void *jsw_str_get_dstr(const jsw_anode_t *node);
derr_t jsw_str_new(const dstr_t bin, jsw_str_t **out);
void jsw_str_free(jsw_str_t **old);
