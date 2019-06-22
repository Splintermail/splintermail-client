#include "link.h"

void link_init(link_t *l){
    l->prev = l;
    l->next = l;
};

void link_list_prepend(link_t *head, link_t *link){
    // for an empty list, old_next is just head
    link_t *old_next = head->next;
    // in the empty list case, this set's head's .next and .prev
    head->next = link;
    old_next->prev = link;
    // in the empty list case, this points link's .next and .prev to head
    link->prev = head;
    link->next = old_next;
}

void link_list_append(link_t *head, link_t *link){
    link_t *old_prev = head->prev;
    old_prev->next = link;
    head->prev = link;
    link->prev = old_prev;
    link->next = head;
}

void link_remove(link_t *link){
    // for a link not in a list, this will reduce to link->prev = link->prev
    link->next->prev = link->prev;
    link->prev->next = link->next;
    link->next = link;
    link->prev = link;
}