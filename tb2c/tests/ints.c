

int main() {
    int a = 0;
    int *b = &a;
    int **c = &b;
    int *d = *c;
    int e = *d;
    return e;
}
