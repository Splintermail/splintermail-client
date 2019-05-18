/*
  Andersson tree library

    > Created (Julienne Walker): September 10, 2005
    > Corrections (James Bucanek): April 10, 2006
      1) Typo in jsw_aerase:
           up != 0 should be top != 0
      2) Bug in jsw_aerase:
           skew ( path[top] ) should be skew ( up )
           split ( path[top] ) should be split ( up )
      3) Bug in skew and split macros:
           Condition should test for nil
      4) Bug in jsw_aerase:
           Search for successor should save the path
*/

#include <stdlib.h>
#include "jsw_atree.h"
#include "common.h"
#include "logger.h"

/* Remove left horizontal links */
#define skew(t) do {                                      \
  if ( t->link[0]->level == t->level && t->level != 0 ) { \
    /* fix count */                                                \
    t->link[0]->count = t->count;                                  \
    t->count = t->link[0]->link[1]->count + t->link[1]->count + 1; \
    /* continue with skew operation */                             \
    jsw_anode_t *save = t->link[0];                       \
    t->link[0] = save->link[1];                           \
    save->link[1] = t;                                    \
    t = save;                                             \
  }                                                       \
} while(0)

/* Remove consecutive horizontal links */
#define split(t) do {                                              \
  if ( t->link[1]->link[1]->level == t->level && t->level != 0 ) { \
    /* fix count */                                                \
    t->link[1]->count = t->count;                                  \
    t->count = t->link[0]->count + t->link[1]->link[0]->count + 1; \
    /* continue with split operation */                            \
    jsw_anode_t *save = t->link[1];                                \
    t->link[1] = save->link[0];                                    \
    save->link[0] = t;                                             \
    t = save;                                                      \
    ++t->level;                                                    \
  }                                                                \
} while(0)

static void place_node ( jsw_atree_t *tree, jsw_anode_t *node )
{
  node->level = 1;
  node->link[0] = node->link[1] = tree->nil;
  node->count = 1;
}

derr_t jsw_ainit ( jsw_atree_t *tree, cmp_f cmp, rel_f rel )
{
  derr_t e = E_OK;
  /* Initialize sentinel */
  tree->nil = (jsw_anode_t *)malloc ( sizeof *tree->nil );
  if ( tree->nil == NULL ) {
    ORIG(e, E_NOMEM, "unable to allocate sentinal node for andersson tree");
  }

  tree->nil->data = NULL; /* Simplifies some ops */
  tree->nil->level = 0;
  tree->nil->link[0] = tree->nil->link[1] = tree->nil;
  tree->nil->count = 0;

  /* Initialize tree */
  tree->root = tree->nil;
  tree->cmp = cmp;
  tree->rel = rel;
  tree->size = 0;

  return E_OK;
}

void jsw_adelete ( jsw_atree_t *tree )
{
  jsw_anode_t *it = tree->root;
  jsw_anode_t *save;

  /* Destruction by rotation */
  while ( it != tree->nil ) {
    if ( it->link[0] == tree->nil ) {
      /* Remove node */
      save = it->link[1];
      tree->rel ( it->data );
    }
    else {
      /* Rotate right */
      save = it->link[0];
      it->link[0] = save->link[1];
      save->link[1] = it;
    }

    it = save;
  }

  /* Finalize destruction */
  free ( tree->nil );
}

void *jsw_afind ( jsw_atree_t *tree, void *data, size_t *idx )
{
  jsw_anode_t *it = tree->root;
  size_t left_count = 0;

  while ( it != tree->nil ) {
    int cmp = tree->cmp ( it->data, data );

    if ( cmp == 0 )
      break;

    int dir = ( cmp < 0 );
    if ( dir ) left_count += it->link[0]->count + 1;
    it = it->link[dir];
  }

  /* nil->data == NULL, nil->count = 0 */
  if ( idx ) *idx = left_count + it->link[0]->count;
  return it->data;
}

void jsw_ainsert ( jsw_atree_t *tree, jsw_anode_t *node )
{
  if ( tree->root == tree->nil ) {
    /* Empty tree case */
    place_node ( tree, node );
    tree->root = node;
  }
  else {
    jsw_anode_t *it = tree->root;
    jsw_anode_t *path[JSW_AHEIGHT_LIMIT];
    int top = 0, dir;

    /* Find a spot and save the path */
    for ( ; ; ) {
      it->count++;
      path[top++] = it;
      dir = tree->cmp ( it->data, node->data ) < 0;

      if ( it->link[dir] == tree->nil )
        break;

      it = it->link[dir];
    }

    /* Create a new item */
    place_node ( tree, node );
    it->link[dir] = node;

    /* Walk back and rebalance */
    while ( --top >= 0 ) {
      /* Which child? */
      if ( top != 0 )
        dir = path[top - 1]->link[1] == path[top];

      skew ( path[top] );
      split ( path[top] );

      /* Fix the parent */
      if ( top != 0 )
        path[top - 1]->link[dir] = path[top];
      else
        tree->root = path[top];
    }
  }

  ++tree->size;
}

int jsw_aerase ( jsw_atree_t *tree, void *data )
{
  if ( tree->root == tree->nil )
    return 0;
  else {
    jsw_anode_t *it = tree->root;
    jsw_anode_t *path[JSW_AHEIGHT_LIMIT];
    int top = 0, dir = -1, cmp;

    /* Find node to remove and save path */
    for ( ; ; ) {
      path[top++] = it;

      if ( it == tree->nil )
        return 0;

      cmp = tree->cmp ( it->data, data );
      if ( cmp == 0 )
        break;

      dir = cmp < 0;
      it = it->link[dir];
    }

    /* Remove the found node */
    if ( it->link[0] == tree->nil
      || it->link[1] == tree->nil )
    {
      /* Single child case */
      int dir2 = it->link[0] == tree->nil;

      /* Unlink the item */
      if ( --top != 0 )
        path[top - 1]->link[dir] = it->link[dir2];
      else
        tree->root = it->link[1];

      tree->rel ( it->data );
    }
    else {
      /* Two child case */
      int top_upon_entry = top;
      // heir will be the next-in-order node
      jsw_anode_t *heir = it->link[1];
      // prev is the parent of the heir
      jsw_anode_t *prev = it;

      while ( heir->link[0] != tree->nil ) {
        path[top++] = prev = heir;
        heir = heir->link[0];
      }

      /* prev (parent of the heir) accepts heir's child.  There can only be one
         child because heir is a leaf or it has one right-horizontal link. */
      prev->link[prev == it] = heir->link[1];
      // heir takes place of deleted node
      heir->link[0] = it->link[0];
      heir->link[1] = it->link[1];
      heir->level = it->level;
      heir->count = it->count;
      // fix pointer-to-the-deleted-element stored in the path
      path[top_upon_entry - 1] = heir;
      // fix pointer-to-the-deleted-element in the deleted element's parent
      if(top_upon_entry > 1){
        path[top_upon_entry - 2]->link[dir] = heir;
      }
      else{
        tree->root = heir;
      }
      // done with deleted node
      tree->rel(it->data);
    }

    /* Walk back up and rebalance */
    while ( --top >= 0 ) {
      jsw_anode_t *up = path[top];
      up->count--;

      if ( top != 0 )
        dir = path[top - 1]->link[1] == up;

      /* Rebalance (aka. black magic) */
      if ( up->link[0]->level < up->level - 1
        || up->link[1]->level < up->level - 1 )
      {
        if ( up->link[1]->level > --up->level )
          up->link[1]->level = up->level;

        /* Order is important! */
        skew ( up );
        skew ( up->link[1] );
        skew ( up->link[1]->link[1] );
        split ( up );
        split ( up->link[1] );
      }

      /* Fix the parent */
      if ( top != 0 )
        path[top - 1]->link[dir] = up;
      else
        tree->root = up;
    }
  }

  --tree->size;

  return 1;
}

size_t jsw_asize ( jsw_atree_t *tree )
{
  return tree->size;
}

/*
  First step in traversal,
  handles min and max
*/
static void *start ( jsw_atrav_t *trav,
  jsw_atree_t *tree, int dir )
{
  trav->tree = tree;
  trav->it = tree->root;
  trav->top = 0;

  /* Build a path to work with */
  if ( trav->it != tree->nil ) {
    while ( trav->it->link[dir] != tree->nil ) {
      trav->path[trav->top++] = trav->it;
      trav->it = trav->it->link[dir];
    }
  }

  /* Could be nil, but nil->data == NULL */
  return trav->it->data;
}

/*
  Subsequent traversal steps,
  handles ascending and descending
*/
static void *move ( jsw_atrav_t *trav, int dir )
{
  jsw_anode_t *nil = trav->tree->nil;

  if ( trav->it->link[dir] != nil ) {
    /* Continue down this branch */
    trav->path[trav->top++] = trav->it;
    trav->it = trav->it->link[dir];

    while ( trav->it->link[!dir] != nil ) {
      trav->path[trav->top++] = trav->it;
      trav->it = trav->it->link[!dir];
    }
  }
  else {
    /* Move to the next branch */
    jsw_anode_t *last;

    do {
      if ( trav->top == 0 ) {
        trav->it = nil;
        break;
      }

      last = trav->it;
      trav->it = trav->path[--trav->top];
    } while ( last == trav->it->link[dir] );
  }

  /* Could be nil, but nil->data == NULL */
  return trav->it->data;
}

void *jsw_atfirst ( jsw_atrav_t *trav, jsw_atree_t *tree )
{
  return start ( trav, tree, 0 ); /* Min value */
}

void *jsw_atlast ( jsw_atrav_t *trav, jsw_atree_t *tree )
{
  return start ( trav, tree, 1 ); /* Max value */
}

void *jsw_atnext ( jsw_atrav_t *trav )
{
  return move ( trav, 1 ); /* Toward larger items */
}

void *jsw_atprev ( jsw_atrav_t *trav )
{
  return move ( trav, 0 ); /* Toward smaller items */
}

void *jsw_aindex ( jsw_atree_t *tree, size_t idx )
{
    /* if count idx is too high, return NULL */
    if(idx >= tree->size)
        return NULL;

    /* walk through tree making decisions based on counts */
    jsw_anode_t *it = tree->root;
    size_t left_count = 0;
    while ( true ){
        /* index of the current node */
        size_t cur = left_count + it->link[0]->count;
        if ( cur == idx )
            return it->data;

        int dir = ( cur < idx );
        /* add to left_count only if we move right */
        if ( dir ) left_count += it->link[0]->count + 1;
        it = it->link[dir];
    }
}
