#include <errno.h>
#include "Tree.h"
#include <pthread.h>
#include <string.h>
#include <malloc.h>
#include <assert.h>
#include "HashMap.h"
#include "path_utils.h"
#include "err.h"

#define WRITE_ACCESS -1

typedef struct Node Node;

struct Node {
    HashMap *children;

    pthread_mutex_t mutex;
    pthread_cond_t read_cond; // readers waiting
    pthread_cond_t write_cond; // writers waiting
    int readers_count;
    int writers_count;

    int writers_waiting;
    int readers_waiting;

    int change;

    // a process waiting on this condition is going to be the last process to access the node
    pthread_cond_t move_cond;
};

struct Tree {
    Node *root;
};


void delete_node(Node *node) {
    if (pthread_mutex_destroy(&node->mutex) != 0)
        syserr("mutex destroy failed");
    if (pthread_cond_destroy(&node->read_cond) != 0)
        syserr("read cond destroy failed");
    if (pthread_cond_destroy(&node->write_cond) != 0)
        syserr("modify cond destroy failed");

    hmap_free(node->children);
    free(node);
}

Node *new_node() {
    Node *node = (Node *) malloc(sizeof(Node));

    if (pthread_mutex_init(&node->mutex, 0) != 0)
        syserr("mutex init failed");
    if (pthread_cond_init(&node->read_cond, 0) != 0)
        syserr("read cond init failed");
    if (pthread_cond_init(&node->write_cond, 0) != 0)
        syserr("modify cond init failed");
    if (pthread_cond_init(&node->move_cond, 0) != 0)
        syserr("move cond init failed");

    node->change = 0;
    node->writers_waiting = 0;
    node->readers_waiting = 0;
    node->writers_count = 0;
    node->readers_count = 0;
    node->children = hmap_new();

    return node;
}

void get_read_access(Node *node) {
    if (!node)
        return;

    if (pthread_mutex_lock(&node->mutex) != 0)
        syserr("lock failed");

    node->readers_waiting++;
    assert(node->readers_waiting >= 0);
    while (node->writers_count + node->writers_waiting > 0 && node->change <= 0) {
        if (pthread_cond_wait(&node->read_cond, &node->mutex) != 0)
            syserr("read cond wait failed");
    }
    node->readers_waiting--;
    assert(node->readers_waiting >= 0);
    if (node->change > 0)
        node->change--;

    node->readers_count++;

    assert(node->writers_count == 0);
    assert(node->change >= 0);
    assert(node->readers_count > 0);

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
    assert(node->readers_count >= 0);
    assert(node->writers_count == 0);
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

    assert(node->readers_count == 0);
    assert(node->writers_count == 0);

    node->change = 0;
    node->writers_count++;

    if (pthread_mutex_unlock(&node->mutex) != 0)
        syserr("unlock failed");
}

void give_up_write_access(Node *node) {
    if (pthread_mutex_lock(&node->mutex) != 0)
        syserr("lock failed");

    node->writers_count--;
    assert(node->writers_count == 0);
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

void give_up_move_access(Node *node) {
    if (node->readers_waiting > 0) {
        node->change = node->readers_waiting;
        if (pthread_cond_signal(&node->read_cond) != 0)
            syserr("read cond signal failed");
    }
    else if (node->writers_waiting > 0) {
        node->change = WRITE_ACCESS;
        if (pthread_cond_signal(&node->write_cond) != 0)
            syserr("modify cond signal failed");
    }

    if (pthread_mutex_unlock(&node->mutex) != 0)
        syserr("unlock failed");
}

Node *modify_child(Node *node, const char *path, const bool root_access) {
    if (!node)
        return NULL;

    char component[MAX_FOLDER_NAME_LENGTH + 1];
    Node *root = node;
    const char *subpath = path;
    Node *new_node;

    subpath = split_path(subpath, component);
    if (!subpath) {
        if (!root_access)
            get_write_access(node);
        return node;
    }
    if (!root_access)
        get_read_access(node);

    do {
        new_node = (Node *) hmap_get(node->children, component);
        if (!root_access || node != root)
            give_up_read_access(node);

        if (!new_node)
            return new_node;

        subpath = split_path(subpath, component);
        if (subpath) {
            get_read_access(new_node);
        }
        else {
            get_write_access(new_node);
        }
        node = new_node;

    } while (node && subpath);

    return node;
}

/* Returns a node represented by the path and acquires a read access to it. */
Node *read_child(Tree *tree, const char *path) {
    char component[MAX_FOLDER_NAME_LENGTH + 1];
    const char *subpath = path;
    Node *node = tree->root;
    Node *new_node;

    get_read_access(node);

    while (node && (subpath = split_path(subpath, component))) {
        new_node = (Node *) hmap_get(node->children, component);
        if (new_node)
            get_read_access(new_node);
        give_up_read_access(node);
        node = new_node;
    }

    return node;
}

int tree_remove(Tree *tree, const char *path) {
    if (!is_path_valid(path))
        return EINVAL;

    char last_component[MAX_FOLDER_NAME_LENGTH + 1];
    char *initial_subpath = make_path_to_parent(path, last_component);
    const char *subpath = initial_subpath;

    if (!subpath) // tried to remove the root
        return EBUSY;

    Node *node = modify_child(tree->root, subpath, false);

    free(initial_subpath);
    if (!node)
        return ENOENT;

    void *child = hmap_get(node->children, last_component);

    if (!child) {
        give_up_write_access(node);
        return ENOENT;
    }

    if (hmap_size(((Node *) child)->children) > 0) {
        give_up_write_access(node);
        return ENOTEMPTY;
    }

    get_move_access(child);
    assert(hmap_remove(node->children, last_component));
    delete_node(child);
    give_up_write_access(node);
    return 0;
}

int tree_create(Tree *tree, const char *path) {
    if (!is_path_valid(path))
        return EINVAL;

    char last_component[MAX_FOLDER_NAME_LENGTH + 1];
    char *initial_subpath = make_path_to_parent(path, last_component);
    const char *subpath = initial_subpath;

    if (!subpath)
        return EEXIST;

    Node *node = modify_child(tree->root, subpath, false);

    free(initial_subpath);

    if (!node)
        return ENOENT;

    Node *child = (Node *) hmap_get(node->children, last_component);

    if (child) {
        give_up_write_access(node);
        return EEXIST;
    }

    Node *new_folder = new_node();
    assert(hmap_insert(node->children, last_component, new_folder));

    give_up_write_access(node);
    return 0;
}


void remove_nodes(Node *node) {
    HashMapIterator hm = hmap_iterator(node->children);
    const char *key;
    void *value;
    while (hmap_next(node->children, &hm, &key, &value)) {
        remove_nodes((Node *) value);
    }

    delete_node(node);
}

void tree_free(Tree *tree) {
    remove_nodes(tree->root);
    free(tree);
}

Tree *tree_new() {
    Tree *tree = (Tree *) malloc(sizeof(Tree));
    tree->root = new_node();
    return tree;
}

/* Creates a string consisting of comma-separated subfolders.
 * The calling thread shall have a read access to the node. */
char *list_subfolders(Node *node) {
    size_t string_length = 0;
    size_t buffer_length = 1;
    char *string = (char *) malloc(sizeof(char));

    HashMapIterator it = hmap_iterator(node->children);
    const char *key;
    void *value;

    while (hmap_next(node->children, &it, &key, &value)) {
        size_t key_length = strlen(key);
        if (string_length + key_length + 1 > buffer_length) {
            buffer_length += MAX_FOLDER_NAME_LENGTH + 1;
            string = (char *) realloc(string, buffer_length * sizeof(char));
        }

        if (string_length > 0)
            string[string_length++] = ',';

        strcpy(string + string_length, key);
        string_length += key_length;
    }

    string[string_length] = '\0';
    return string;
}

char *tree_list(Tree *tree, const char *path) {
    if (!is_path_valid(path))
        return NULL;

    Node *node = read_child(tree, path);

    if (!node) {
        return NULL;
    }

    char *string = list_subfolders(node);
    give_up_read_access(node);

    return string;
}

/* Checks whether `b` is a subfolder of `a`, considering both paths are valid. */
bool is_subfolder(const char *a, const char *b) {
    size_t a_length = strlen(a);
    size_t b_length = strlen(b);

    if (a_length != b_length && strncmp(a, b, a_length) == 0)
        return true;
    else
        return false;
}

/* Ensures there are no running processes in tree rooted in node. */
void bfs(Node *node) {
    get_move_access(node);
    HashMapIterator it = hmap_iterator(node->children);
    const char *key;
    void *value;
    while (hmap_next(node->children, &it, &key, &value)) {
        bfs((Node *) value);
    }
}

/* Finds the last common folder of two given paths.
 * Args:
 * - `path_a`, `path_b`: valid paths
 * - `path`:  longest common subpath of path_a and path_b
 * Returns length of string representation of `path`.
 * Copies the common path of `path_a` and `path_b` and saves it to `path`.
 * The caller should free `path` afterwards. */
size_t path_lca(const char *path_a, const char *path_b, char **path) {
    size_t last_dash_index = 0;
    size_t index = 0;
    while ((path_a + index)[0] != '\0' && (path_b + index)[0] != '\0') {
        if (path_a[index] != path_b[index])
            break;
        if (path_a[index] == '/')
            last_dash_index = index;
        index++;
    }
    char *common_path = (char *) malloc((last_dash_index + 2) * sizeof(char));
    strncpy(common_path, path_a, last_dash_index + 1);
    common_path[last_dash_index + 1] = '\0';
    *path = common_path;
    return last_dash_index + 1;
}

int tree_move(Tree *tree, const char *source, const char *target) {
    if (!is_path_valid(source) || !is_path_valid(target))
        return EINVAL;

    if (strcmp(source, "/") == 0)
        return EBUSY;

    if (is_subfolder(source, target)) //todo
        return -1;

    char *common_path;
    size_t common_length = path_lca(source, target, &common_path);
    Node *lca = modify_child(tree->root, common_path, false);
    // teraz żaden nowy move nie wejdzie nam do wierzchołka
    free(common_path);
    if (!lca)
        return ENOENT;

    char new_name[MAX_FOLDER_NAME_LENGTH + 1];
    char *initial_subpath_target = make_path_to_parent(target + common_length - 1, new_name);
    const char *target_subpath = initial_subpath_target;

    if (!initial_subpath_target) { // target is the lca
        give_up_write_access(lca);
        return EEXIST;
    }

    Node *target_parent = modify_child(lca, target_subpath, true);
    free(initial_subpath_target);

    if (!target_parent) { // target's parent doesn't exist
        give_up_write_access(lca);
        return ENOENT;
    }

    if (hmap_get(target_parent->children, new_name)) { // target already exists
        give_up_write_access(lca);
        if (lca != target_parent)
            give_up_write_access(target_parent);
        return EEXIST;
    }

    char source_name[MAX_FOLDER_NAME_LENGTH + 1];
    char *initial_subpath_source = make_path_to_parent(source + common_length - 1, source_name);
    const char *source_subpath = initial_subpath_source;

    if (!initial_subpath_source) // source is the lca
        return EBUSY;

    Node *source_parent = modify_child(lca, source_subpath, true);
    free(initial_subpath_source);

    if (lca != target_parent && lca != source_parent)
        give_up_write_access(lca);

    if (!source_parent) { // source doesn't exist
        give_up_write_access(target_parent);
        return ENOENT;
    }

    Node *source_node = hmap_get(source_parent->children, source_name);

    if (!source_node) { // source doesn't exist
        give_up_write_access(source_parent);
        if (source_parent != target_parent)
            give_up_write_access(target_parent);
        return ENOENT;
    }

    // Waiting for processes in source's subtree to finish.
    bfs(source_node);

    // Actually moving the subtree.
    hmap_remove(source_parent->children, source_name);
    hmap_insert(target_parent->children, new_name, source_node);

//    give_up_write_access(source_node);
    give_up_write_access(target_parent);
    if (target_parent != source_parent)
        give_up_write_access(source_parent);

    return 0;
}
