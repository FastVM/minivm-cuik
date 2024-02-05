
#include <stddef.h>
#include <stdio.h>

int main() {
    size_t iters = 1000 * 1000 * 100;
    void *x = NULL;
    x = &x;
    for (size_t i = 0; i < iters; i++) {
        x = *(void**)x;
    }
    printf("%p -> %p\n", &x, x);
    return 0;
}
