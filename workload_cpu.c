#include <stdio.h>
#include <math.h>
#include <time.h>

int main() {
    printf("CPU-bound workload starting...\n");
    double result = 0;
    for (long i = 0; i < 1e8; i++) {
        result += sqrt(i % 100 + 1);
    }
    printf("CPU-bound workload done. Result: %f\n", result);
    return 0;
}
