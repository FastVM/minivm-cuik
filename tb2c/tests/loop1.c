
#include <stddef.h>
#include <stdio.h>

int main() {
    size_t iter = 0;
    size_t stop = 1000 * 1000 * 10;
    size_t step = 1;
    size_t *ptr_iter = &iter;
    size_t *ptr_stop = &stop;
    size_t *ptr_step = &step;
    while (*ptr_iter < *ptr_stop) {
        *ptr_iter += 1;
    }
    printf("%zu", *ptr_iter);
    return 0;
}
