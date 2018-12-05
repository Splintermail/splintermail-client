#ifndef JSW_ATREE_H
#define JSW_ATREE_H

#include <stdio.h>
#include "common.h"

/*
  Andersson tree library

    > Created (Julienne Walker): September 10, 2005
    > Corrections (James Bucanek): April 10, 2006
    > API Modified: Ryan Beethe December 2, 2018

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

#define JSW_AHEIGHT_LIMIT 64 /* Tallest allowable tree */

/* User-defined item handling */
typedef int   (*cmp_f) ( const void *p1, const void *p2 );
typedef void  (*rel_f) ( void *p );

typedef struct jsw_anode {
  int               level;   /* Horizontal level for balance */
  void             *data;    /* User-defined content */
  struct jsw_anode *link[2]; /* Left (0) and right (1) links */
} jsw_anode_t;

typedef struct jsw_atree {
  jsw_anode_t *root; /* Top of the tree */
  jsw_anode_t *nil;  /* End of tree sentinel */
  cmp_f        cmp;  /* Compare two items */
  rel_f        rel;  /* Destroy an item (user-defined) */
  size_t       size; /* Number of items (user-defined) */
} jsw_atree_t;

typedef struct jsw_atrav {
  jsw_atree_t *tree;                    /* Paired tree */
  jsw_anode_t *it;                      /* Current node */
  jsw_anode_t *path[JSW_AHEIGHT_LIMIT]; /* Traversal path */
  size_t       top;                     /* Top of stack */
} jsw_atrav_t;

/* Andersson tree functions */
derr_t       jsw_ainit ( jsw_atree_t *tree, cmp_f cmp, rel_f rel );
void         jsw_adelete ( jsw_atree_t *tree );
void        *jsw_afind ( jsw_atree_t *tree, void *data );
void         jsw_ainsert ( jsw_atree_t *tree, jsw_anode_t *node );
int          jsw_aerase ( jsw_atree_t *tree, void *data );
size_t       jsw_asize ( jsw_atree_t *tree );

/* Traversal functions */
void        *jsw_atfirst ( jsw_atrav_t *trav, jsw_atree_t *tree );
void        *jsw_atlast ( jsw_atrav_t *trav, jsw_atree_t *tree );
void        *jsw_atnext ( jsw_atrav_t *trav );
void        *jsw_atprev ( jsw_atrav_t *trav );

#endif
