#include "libdstr.h"

void link_init(link_t *l){
    l->prev = l;
    l->next = l;
};

void link_list_prepend(link_t *head, link_t *link){
    // require explicit safety, but don't leave a footgun laying around
    if(link->next != link){
        LOG_ERROR("YOU MUST CALL LINK_REMOVE() BEFORE LINK_LIST_PREPEND!!\n");
        link_remove(link);
    }

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
    // require explicit safety, but don't leave a footgun laying around
    if(link->next != link){
        LOG_ERROR("YOU MUST CALL LINK_REMOVE() BEFORE LINK_LIST_PREPEND!!\n");
        link_remove(link);
    }

    link_t *old_prev = head->prev;
    old_prev->next = link;
    head->prev = link;
    link->prev = old_prev;
    link->next = head;
}

void link_list_append_list(link_t *recip, link_t *donor){
    if(link_list_isempty(donor)) return;

    link_t *donor_first = donor->next;
    link_t *donor_last = donor->prev;

    recip->prev->next = donor_first;
    donor_first->prev = recip->prev;

    recip->prev = donor_last;
    donor_last->next = recip;

    link_init(donor);
}

link_t *link_list_pop_first(link_t *head){
    link_t *first = head->next;
    if(first == head){
        return NULL;
    }
    link_remove(first);
    return first;
}

link_t *link_list_pop_last(link_t *head){
    link_t *last = head->prev;
    if(last == head){
        return NULL;
    }
    link_remove(last);
    return last;
}

void link_remove(link_t *link){
    // for a link not in a list, this will reduce to link->prev = link->prev
    link->next->prev = link->prev;
    link->prev->next = link->next;
    link->next = link;
    link->prev = link;
}

bool link_list_isempty(link_t *head){
    return head == head->next;
}
