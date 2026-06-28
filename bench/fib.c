/* Hand-written C baseline (speed-of-light) for the fib benchmark.
 * Build:  cc -O2 bench/fib.c -o build/fib_c */
#include <stdio.h>

static long long fib(long long n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main(void) {
    printf("%lld\n", fib(35));
    return 0;
}
