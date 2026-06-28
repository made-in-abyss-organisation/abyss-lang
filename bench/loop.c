/* Hand-written C baseline (speed-of-light) for the loop benchmark.
 * Build:  cc -O2 bench/loop.c -o build/loop_c
 * Values stay < 2^31 via the modulo, so the 64-bit products never overflow. */
#include <stdio.h>

int main(void) {
    long long x = 1;
    for (long long i = 0; i < 100000000; i++) {
        x = (x * 1103515245 + 12345) % 2147483647;
    }
    printf("%lld\n", x);
    return 0;
}
