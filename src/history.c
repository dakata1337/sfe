#include "history.h"

#define MAX_ENTRIES 100

struct SfeHistory {
    GQueue back; /* GFile*, most recent at the head */
    GQueue forward; /* GFile*, next at the head */
};

SfeHistory* sfe_history_new(void)
{
    return g_new0(SfeHistory, 1);
}

void sfe_history_free(SfeHistory* h)
{
    g_queue_clear_full(&h->back, g_object_unref);
    g_queue_clear_full(&h->forward, g_object_unref);
    g_free(h);
}

static void push_limited(GQueue* q, GFile* file)
{
    g_queue_push_head(q, g_object_ref(file));
    if (q->length > MAX_ENTRIES)
        g_object_unref(g_queue_pop_tail(q));
}

void sfe_history_push(SfeHistory* h, GFile* from)
{
    push_limited(&h->back, from);
    g_queue_clear_full(&h->forward, g_object_unref);
}

/* Moves one step from `src` to the caller, pushing `current` onto `dst`. */
static GFile* step(GQueue* src, GQueue* dst, GFile* current)
{
    if (g_queue_is_empty(src))
        return NULL;
    push_limited(dst, current);
    return g_queue_pop_head(src);
}

GFile* sfe_history_back(SfeHistory* h, GFile* current)
{
    return step(&h->back, &h->forward, current);
}

GFile* sfe_history_forward(SfeHistory* h, GFile* current)
{
    return step(&h->forward, &h->back, current);
}

bool sfe_history_can_back(const SfeHistory* h)
{
    return h->back.length > 0;
}

bool sfe_history_can_forward(const SfeHistory* h)
{
    return h->forward.length > 0;
}
