
typedef struct {
    long a;
    struct {
        short b;
        short c;
    };
} thign_t;

int main() {
    thign_t a;
    a.b = 7;
    a.c = 13;
    a.a = a.b * a.c;
    thign_t *b = &a;
    thign_t **c = &b;
    thign_t *d = *c;
    thign_t e = *d;
    return e.a - e.b * e.c;
}