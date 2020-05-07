// circularly linked lists, where the head element is not part of the list

typedef struct link_t {
    struct link_t *prev;
    struct link_t *next;
} link_t;

void link_init(link_t *l);

/* prepend/append a single element.  You must ensure that link is not in a list
   via link_remove() before calling these; the new and old lists may have
   different thread safety requirements */
void link_list_prepend(link_t *head, link_t *link);
void link_list_append(link_t *head, link_t *link);

// empty the donor list into the recip list
void link_list_append_list(link_t *recip, link_t *donor);

// pop a single element, or return NULL if there is none
link_t *link_list_pop_first(link_t *head);
link_t *link_list_pop_last(link_t *head);

void link_remove(link_t *link);

bool link_list_isempty(link_t *head);

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
