CC = gcc
CFLAGS = -Wall -pthread

KERNEL_MODULE = monitor.ko
USER_BIN = engine
WORKLOADS = workload_cpu workload_io

.PHONY: all clean

all: $(USER_BIN) $(WORKLOADS)

$(USER_BIN): engine.c
	$(CC) $(CFLAGS) engine.c -o $(USER_BIN)

workload_cpu: workload_cpu.c
	$(CC) $(CFLAGS) workload_cpu.c -o workload_cpu

workload_io: workload_io.c
	$(CC) $(CFLAGS) workload_io.c -o workload_io

$(KERNEL_MODULE): monitor.c
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	rm -f $(USER_BIN) $(WORKLOADS) *.o *.log
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
