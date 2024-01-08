
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

int is_prime(size_t n) {
    if (n % 2 == 0) {
        return 0;
    }
    for (size_t i = 3; i * i <= n; i += 2) {
        if (n % i == 0) {
            return 0;
        }
    }
    return 1;
}

int main() {
    size_t max = 1000000;
    size_t total = 0;
    if (max >= 2) {
        total += 1;
    }
    for (size_t i = 3; i <= max; i += 2) {
        if (is_prime(i)) {
            total += 1;
        }
    }
    printf("number of primes under %zu = %zu\n", max, total);
    return 0;
}
