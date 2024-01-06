
#include <stdio.h>

int fib(int x) {
    if (x < 2) {
        return x;
    } else {
        return fib(x-2) + fib(x-1);
    }
}

int main() {
    printf("%i", fib(35));
    return 0;
}
