CC     = x86_64-linux-gnu-gcc-13
CFLAGS = -std=c99 -Wall -Wextra -I ../include -I fake_sdk

SRC_ROOT = ..

COMMO_SRCS = \
    $(SRC_ROOT)/core/commo.c \
    gpio_sim.c \
    test_commo.c

DRIVER_SRCS = \
    $(SRC_ROOT)/drivers/driver.c \
    $(SRC_ROOT)/utils/timer.c \
    gpio_sim.c \
    test_driver.c

.PHONY: all run run_commo run_driver clean

all: test_commo test_driver

test_commo: $(COMMO_SRCS)
	$(CC) $(CFLAGS) $^ -o $@

test_driver: $(DRIVER_SRCS)
	$(CC) $(CFLAGS) $^ -o $@

run_commo: test_commo
	./test_commo

run_driver: test_driver
	./test_driver

run: all
	@echo ""; ./test_commo; echo ""; ./test_driver

clean:
	rm -f test_commo test_driver
