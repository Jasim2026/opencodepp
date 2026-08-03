#include "math.h"

int add(int a, int b) {
    return a + b;
}

static int twice(int x) {
    return add(x, x);
}

int apply(int v) {
    return twice(v);
}
