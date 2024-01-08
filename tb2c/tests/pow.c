
typedef unsigned long long num_t;

int printf(const char *fmt, ...);

num_t test_pow(num_t base, num_t exp) {
    if (base == 0) {
        return 0;
    }
    num_t ret = 1;
    while (exp != 0) {
        if (exp % 2 != 0) {
            ret *= base;
        }
        exp /= 2;
        base *= base;
    }
    return ret;
}

int main() {
    num_t x = 0;
    for (num_t i = 0; i < 1000 * 1000 * 10; i++) {
        num_t got = i * i;
        num_t want = test_pow(i, 2);
        if (want != got) {
            printf("case %i: %i != %i (want != got)\n", i, want, got);
            return 1;
        }

    }
    return 0;
}
