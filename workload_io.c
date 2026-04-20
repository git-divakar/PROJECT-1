#include <stdio.h>
#include <unistd.h>
#include <time.h>

int main() {
    printf("I/O-bound workload starting...\n");
    FILE *f = fopen("io_test.log", "w");
    if (!f) {
        perror("fopen");
        return 1;
    }

    for (int i = 0; i < 100; i++) {
        fprintf(f, "Line %d: Testing I/O performance\n", i);
        fflush(f);
        usleep(50000); // simulate I/O delay
    }

    fclose(f);
    printf("I/O-bound workload done.\n");
    return 0;
}
