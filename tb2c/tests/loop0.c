
#include <stddef.h>
#include <stdio.h>

int main() {
    size_t iter = 0;
    size_t stop = 1000 * 1000 * 10;
    size_t step = 1;
    while (iter < stop) {
        iter += 1;
    }
    printf("%zu", iter);
    return 0;
}
