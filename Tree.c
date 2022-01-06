#include <errno.h>
#include "Tree.h"
#include <pthread.h>
#include <string.h>
#include <malloc.h>
#include <err.h>
#include <assert.h>
#include "HashMap.h"
#include "path_utils.h"
#include "err.h"

#define MODIFIER_ACCESS -1

typedef struct Node Node;

struct Node {
    char *name;
    HashMap *children;
    Node *parent;

    pthread_mutex_t mutex;
    pthread_cond_t read_cond; // readers waiting
    pthread_cond_t modify_cond; // writers waiting
    int readers_count;
    int modifiers_count;

    int modifiers_waiting;
    int readers_waiting;

    int change;
};

struct Tree {
    Node *root;
};


void delete_node(Node *node) {
    if (pthread_mutex_destroy(&node->mutex) != 0)
        syserr("mutex destroy failed");
    if (pthread_cond_destroy(&node->read_cond) != 0)
        syserr("read cond destroy failed");
    if (pthread_cond_destroy(&node->modify_cond) != 0)
        syserr("modify cond destroy failed");

    hmap_free(node->children);
    free(node);
}

Node *new_node(){
    Node *node = (Node *) malloc(sizeof(Node));

    if (pthread_mutex_init(&node->mutex, 0) != 0)
        syserr("mutex init failed");
    if (pthread_cond_init(&node->read_cond, 0) != 0)
        syserr("read cond init failed");
    if (pthread_cond_init(&node->modify_cond, 0) != 0)
        syserr("modify cond init failed");

    node->change = 0;
    node->modifiers_waiting = 0;
    node->readers_waiting = 0;
    node->modifiers_count = 0;
    node->readers_count = 0;
    node->children = hmap_new();

    return node;
}

void get_read_access(Node *node) {
    int err;
    if ((err = pthread_mutex_lock(&node->mutex)) != 0)
        syserr("lock failed");

    node->readers_waiting++;
    while (node->readers_waiting + node->modifiers_waiting > 0 && node->change <= 0) {
        if ((err = pthread_cond_wait(&node->read_cond, &node->mutex)) != 0)
            syserr("read cond wait failed");
    }
    node->readers_waiting--;

    if (node->change > 0)
        node->change--;

    node->readers_count++;

    if (node->change > 0)
        if ((err = pthread_cond_signal(&node->read_cond)) != 0)
            syserr("read cond signal failed");

    if((err = pthread_mutex_unlock(&node->mutex)) != 0)
        syserr("unlock failed");
}

void give_up_read_access(Node *node) {
    int err;
    node->readers_count--;
    if (node->readers_count == 0 && node->modifiers_waiting > 0) {
        node->change = MODIFIER_ACCESS;
        if((err = pthread_cond_signal(&node->modify_cond)) != 0)
            syserr("modify cond wait failed");
    }

    if ((err = pthread_mutex_unlock(&node->mutex)) != 0)
        syserr("unlock failed");
}

Node *traverse_down(Tree *tree, Node *node, const char *component) {
    Node *new_node = NULL;
    get_read_access(node);

    new_node = (Node *) hmap_get(node->children, component); // reading

    give_up_read_access(node);

    return new_node;
}

void get_modify_access(Node *node) {
    int err;
    if ((err = pthread_mutex_lock(&node->mutex)) != 0)
        syserr("lock failed");

    node->modifiers_waiting++;
    while (node->modifiers_count + node->readers_count > 0 && node->change != MODIFIER_ACCESS) {
        if ((err = pthread_cond_wait(&node->modify_cond, &node->mutex)) != 0)
            syserr("modify cond wait failed");
    }
    node->modifiers_waiting--;

    node->change = 0;
    node->modifiers_count++;

    if ((err = pthread_mutex_unlock(&node->mutex)) != 0)
        syserr("unlock failed");
}

void give_up_modify_access(Node *node) {
    int err;
    if ((err = pthread_mutex_lock(&node->mutex)))
        syserr("lock failed");

    node->modifiers_count--;

    if (node->readers_waiting > 0) {
        node->change = node->readers_waiting;
        if ((err = pthread_cond_signal(&node->read_cond)) != 0)
            syserr("read cond signal failed");
    }
    else if (node->modifiers_waiting > 0) {
        node->change = MODIFIER_ACCESS;
        if ((err = pthread_cond_signal(&node->modify_cond)) != 0)
            syserr("modify cond signal failed");
    }

    if ((err = pthread_mutex_unlock(&node->mutex)) != 0)
        syserr("unlock failed");
}

int tree_remove(Tree *tree, const char *path) {
    if (!is_path_valid(path))
        return EINVAL;

    char component[MAX_FOLDER_NAME_LENGTH + 1];
    char last_component[MAX_FOLDER_NAME_LENGTH + 1];
    const char *subpath = make_path_to_parent(path, last_component);

    if (!subpath) // tried to remove the root
        return EBUSY;

    Node *node = tree->root;

    while ((subpath = split_path(subpath, component)) && node) {
        node = traverse_down(tree, node, component);
    }

    if (!node)
        return ENOENT;

    get_modify_access(node);

    Node *child = (Node *) hmap_get(node->children, last_component);

    if (!child){
        give_up_modify_access(node);
        return ENOENT;
    }

    if (hmap_size(child->children) > 0) {
        give_up_modify_access(node);
        return ENOTEMPTY;
    }

    assert(hmap_remove(node->children, last_component));
    delete_node(child);
    give_up_modify_access(node); // todo: chyba trzeba zaczekać, aż procesy w dziecku się skończą
    return 0;
}

int tree_create(Tree* tree, const char* path) {
    if (!is_path_valid(path))
        return EINVAL;

    char component[MAX_FOLDER_NAME_LENGTH + 1];
    char last_component[MAX_FOLDER_NAME_LENGTH + 1];
    const char *subpath = make_path_to_parent(path, last_component);

    if (!subpath)
        return EEXIST;

    Node *node = tree->root;

    while ((subpath = split_path(subpath, component)) && node) {
        node = traverse_down(tree, node, component);
    }

    if (!node)
        return ENOENT;

    get_modify_access(node);

    Node *child = (Node *) hmap_get(node->children, last_component);

    if (child){
        give_up_modify_access(node);
        return EEXIST;
    }

    Node *new_folder = new_node();
    hmap_insert(node->children, last_component, new_folder);

    give_up_modify_access(node);
    return 0;
}
