#ifndef LINK_H
#define LINK_H

#include "common.h"

// circularly linked lists, where the head element is not part of the list

typedef struct link_t {
    struct link_t *prev;
    struct link_t *next;
} link_t;

void link_init(link_t *l);

// append a single element.  This does not combine lists.
void link_list_prepend(link_t *head, link_t *link);
void link_list_append(link_t *head, link_t *link);

// pop a single element, or return NULL if there is none
link_t *link_list_pop_first(link_t *head);
link_t *link_list_pop_last(link_t *head);

void link_remove(link_t *link);

// automate for-loops which call CONTAINER_OF for each link in list
#define LINK_FOR_EACH(var, head, structure, member) \
    for(var = CONTAINER_OF((head)->next, structure, member); \
        &var->member != (head); \
        var = CONTAINER_OF(var->member.next, structure, member))

// same thing but use a temporary variable to be safe against link_remove
#define LINK_FOR_EACH_SAFE(var, temp, head, structure, member) \
    for(var = CONTAINER_OF((head)->next, structure, member), \
        temp = CONTAINER_OF(var->member.next, structure, member); \
        &var->member != (head); \
        var = temp, \
        temp = CONTAINER_OF(var->member.next, structure, member))

#endif // LINK_H
