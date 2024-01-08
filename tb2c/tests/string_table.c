
#include <stdio.h>

int *get_ref() {
    static int x = 0;
    return &x;
}

int bad_strlen(const char *s) {
    static int x;
    snprintf(NULL, 0, "%s%n\n", s, get_ref());
    x = *get_ref();
    return x;
}

int main() {
    printf("%i\n", bad_strlen("Hello, World!"));
    return 0;
}
