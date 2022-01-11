#include "Node.h"
#include "err.h"

#define WRITE_ACCESS -1

void get_read_access(Node *node) {
    if (!node)
        return;

    if (pthread_mutex_lock(&node->mutex) != 0)
        syserr("lock failed");

    node->readers_waiting++;

    while (node->writers_count + node->writers_waiting > 0 && node->change <= 0) {
        if (pthread_cond_wait(&node->read_cond, &node->mutex) != 0)
            syserr("read cond wait failed");
    }
    node->readers_waiting--;

    if (node->change > 0)
        node->change--;

    node->readers_count++;

    if (node->change > 0)
        if (pthread_cond_signal(&node->read_cond) != 0)
            syserr("read cond signal failed");

    if (pthread_mutex_unlock(&node->mutex) != 0)
        syserr("unlock failed");
}


void give_up_read_access(Node *node) {
    if (pthread_mutex_lock(&node->mutex) != 0)
        syserr("mutex lock failed");

    node->readers_count--;

    if (node->readers_count == 0 && node->writers_waiting > 0) {
        node->change = WRITE_ACCESS;
        if (pthread_cond_signal(&node->write_cond) != 0)
            syserr("modify cond wait failed");
    }
    else if (node->readers_count == 0 && node->writers_waiting == 0) {
        if (pthread_cond_signal(&node->move_cond) != 0)
            syserr("move cond wait failed");
    }

    if (pthread_mutex_unlock(&node->mutex) != 0)
        syserr("unlock failed");
}


void get_write_access(Node *node) {
    if (!node)
        return;

    if (pthread_mutex_lock(&node->mutex) != 0)
        syserr("lock failed");

    node->writers_waiting++;
    while (node->writers_count + node->readers_count > 0 && node->change != WRITE_ACCESS) {
        if (pthread_cond_wait(&node->write_cond, &node->mutex) != 0)
            syserr("modify cond wait failed");
    }
    node->writers_waiting--;

    node->change = 0;
    node->writers_count++;

    if (pthread_mutex_unlock(&node->mutex) != 0)
        syserr("unlock failed");
}

void give_up_write_access(Node *node) {
    if (pthread_mutex_lock(&node->mutex) != 0)
        syserr("lock failed");

    node->writers_count--;

    if (node->readers_waiting > 0) {
        node->change = node->readers_waiting;
        if (pthread_cond_signal(&node->read_cond) != 0)
            syserr("read cond signal failed");
    }
    else if (node->writers_waiting > 0) {
        node->change = WRITE_ACCESS;
        if (pthread_cond_signal(&node->write_cond) != 0)
            syserr("write cond signal failed");
    }
    else {
        if (pthread_cond_signal(&node->move_cond) != 0)
            syserr("move cond wait failed");
    }

    if (pthread_mutex_unlock(&node->mutex) != 0)
        syserr("unlock failed");
}


void get_move_access(Node *node) {
    if (pthread_mutex_lock(&node->mutex) != 0)
        syserr("lock failed");

    while (node->writers_waiting + node->writers_count
           + node->readers_waiting + node->readers_count > 0) {
        if (pthread_cond_wait(&node->move_cond, &node->mutex) != 0)
            syserr("modify cond wait failed");
    }
    node->change = 0;

    if (pthread_mutex_unlock(&node->mutex) != 0)
        syserr("unlock failed");
}