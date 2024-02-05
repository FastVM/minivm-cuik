
#include <stddef.h>
#include <stdio.h>

int main() {
    size_t iter = 0;
    size_t stop = 1000 * 1000 * 10;
    size_t step = 1;
    size_t *ptr_iter = &iter;
    size_t *ptr_stop = &stop;
    size_t *ptr_step = &step;
    size_t **ptr_ptr_iter = &ptr_iter;
    size_t **ptr_ptr_stop = &ptr_stop;
    size_t **ptr_ptr_step = &ptr_step;
    while (**ptr_ptr_iter < **ptr_ptr_stop) {
        **ptr_ptr_iter += 1;
    }
    printf("%zu", **ptr_ptr_iter);
    return 0;
}
