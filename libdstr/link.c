#include "libdstr.h"

void link_init(link_t *l){
    l->prev = l;
    l->next = l;
}

void link_list_prepend(link_t *head, link_t *link){
    // safe to call on zeroized head
    if(!head->next) link_init(head);
    // caller must ensure link is not in another list
    if(link->next && link->next != link){
        LOG_FATAL("link_list_prepend() called on link in another list\n");
    }

    // for an empty list, old_next is just head
    link_t *old_next = head->next;
    // in the empty list case, this sets head's .next and .prev
    head->next = link;
    old_next->prev = link;
    // in the empty list case, this points link's .next and .prev to head
    link->prev = head;
    link->next = old_next;
}

void link_list_append(link_t *head, link_t *link){
    // safe to call on zeroized head
    if(!head->next) link_init(head);
    // caller must ensure link is not in another list
    if(link->next && link->next != link){
        LOG_FATAL("link_list_append() called on link in another list\n");
    }

    link_t *old_prev = head->prev;
    old_prev->next = link;
    head->prev = link;
    link->prev = old_prev;
    link->next = head;
}

void link_list_append_list(link_t *recip, link_t *donor){
    if(!recip->next) link_init(recip);
    if(!donor->next) link_init(donor);
    if(link_list_isempty(donor)) return;

    link_t *donor_first = donor->next;
    link_t *donor_last = donor->prev;

    recip->prev->next = donor_first;
    donor_first->prev = recip->prev;

    recip->prev = donor_last;
    donor_last->next = recip;

    link_init(donor);
}

void link_list_prepend_list(link_t *recip, link_t *donor){
    if(!recip->next) link_init(recip);
    if(!donor->next) link_init(donor);
    if(link_list_isempty(donor)) return;

    link_t *donor_first = donor->next;
    link_t *donor_last = donor->prev;

    recip->next->prev = donor_last;
    donor_last->next = recip->next;

    recip->next = donor_first;
    donor_first->prev = recip;

    link_init(donor);
}

link_t *link_list_pop_first(link_t *head){
    // safe to call on a zeroized link
    if(head->next == NULL) return NULL;

    link_t *first = head->next;
    if(first == head){
        return NULL;
    }
    link_remove(first);
    return first;
}

link_t *link_list_pop_last(link_t *head){
    // safe to call on a zeroized link
    if(head->next == NULL) return NULL;

    link_t *last = head->prev;
    if(last == head){
        return NULL;
    }
    link_remove(last);
    return last;
}

bool _link_list_pop_first_n(_link_io_t *io, size_t nio){
    /* rather than check every list then pop every list, which would break down
       if any head were ever represented twice, do each pop as we go */
    for(size_t i = 0; i < nio; i++){
        link_t *link = link_list_pop_first(io[i].head);
        if(!link){
            // zeroize reamining outputs
            for(size_t j = i; j < nio; j++) *io[j].out = NULL;
            if(i == 0) return false;
            // put things back in reverse order
            size_t j = i-1;
            do {
                link_list_prepend(io[j].head, *io[j].out);
                *io[j].out = NULL;
            } while(j-- > 0);
            return false;
        }
        *io[i].out = link;
    }
    return true;
}

bool _link_list_pop_last_n(_link_io_t *io, size_t nio){
    for(size_t i = 0; i < nio; i++){
        link_t *link = link_list_pop_last(io[i].head);
        if(!link){
            for(size_t j = i; j < nio; j++) *io[j].out = NULL;
            if(i == 0) return false;
            size_t j = i-1;
            do {
                link_list_append(io[j].head, *io[j].out);
                *io[j].out = NULL;
            } while(j-- > 0);
            return false;
        }
        *io[i].out = link;
    }
    return true;
}

void link_remove(link_t *link){
    // safe to call on a zeroized link
    if(link->next == NULL) return;

    // for a link not in a list, this will reduce to link->prev = link->prev
    link->next->prev = link->prev;
    link->prev->next = link->next;
    link->next = link;
    link->prev = link;
}

bool link_list_isempty(link_t *head){
    // safe to call on a zeroized link
    return head == head->next || head->next == NULL;
}
