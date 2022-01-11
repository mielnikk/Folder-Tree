#ifndef NODE_H
#define NODE_H

#include <pthread.h>
#include "HashMap.h"

typedef struct Node Node;

struct Node {
    HashMap *children;

    pthread_mutex_t mutex;
    pthread_cond_t read_cond; /* condition for readers to wait on */
    pthread_cond_t write_cond; /* condition for writers to wait on */
    int readers_count;
    int writers_count;

    int writers_waiting;
    int readers_waiting;

    /* Helps to indicate whether a woken process was actually signaled.
     * If set to WRITE_ACCESS, indicates that a signaled writer may access the critical section.
     * If set to 0, any process can access the critical section if there are no other processes that
     * forbid it.
     * If the value is >0, a signaled reader may access the critical section. */
    int change;

    /* a process waiting on this condition is going to be the last process to access the node */
    pthread_cond_t move_cond;
};

/* Acquires read access to `node`. */
void get_read_access(Node *node);

/* Releases read access to `node`. */
void give_up_read_access(Node *node);

/* Acquires write access to `node` */
void get_write_access(Node *node);

/* Releases write access to `node`. */
void give_up_write_access(Node *node);

/* Acquires write access to `node`. If any other processes are waiting or working,
 * lets them access `node` first.
 * To avoid starvation, a write lock on `node`'s parent is needed, as it blocks
 * new incoming processes. */
void get_move_access(Node *node);

#endif //NODE_H
