#include <stdio.h>

struct Node;
struct Node {
    int v;
    struct Node *next;
};

typedef struct Node Node;

struct Box {
    struct Node *head;
    int count;
};

static int sum_list(struct Node *n) {
    int sum = 0;
    while (n) {
        sum += n->v;
        n = n->next;
    }
    return sum;
}

int main(void) {
    Node a;
    Node b;
    Node c;
    a.v = 1;
    b.v = 2;
    c.v = 3;
    a.next = &b;
    b.next = &c;
    c.next = 0;

    printf("%d\n", sum_list(&a));

    struct Box box = {&a, 3};
    printf("%d %d\n", box.head->v, box.count);
    printf("%d\n", box.head->next->next->v);

    struct Node *p = &b;
    p->v = 20;
    printf("%d\n", a.next->v);

    struct Node *q = p;
    q->next->v = 30;
    printf("%d\n", sum_list(&a));
    return 0;
}
