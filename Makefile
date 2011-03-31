CFLAGS=-std=c99 -Wall -O2

tegra-power-query: power_tools.c seaboard.h power_tools.h
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm tegra-power-query

.PHONY: clean
