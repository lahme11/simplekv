SRC      = ../Stage2/src
CFLAGS   = -Wall -Wextra -g -fsanitize=address,undefined -fno-sanitize-recover=all -I$(SRC)
ENGINE   = $(SRC)/record.c $(SRC)/parse.c $(SRC)/index.c $(SRC)/store.c

test: unit integration

unit: bin/test_units
	./bin/test_units

bin/test_units: tests/test_units.c $(ENGINE) $(SRC)/simplekv.h
	mkdir -p bin
	gcc $(CFLAGS) tests/test_units.c $(ENGINE) -o bin/test_units

bench: bin/bench
	./bin/bench -s never -n 20000
	./bin/bench -s batch -n 20000
	./bin/bench -s always -n 2000

bin/bench: bench/bench.c $(ENGINE) $(SRC)/simplekv.h
	mkdir -p bin
	gcc -Wall -Wextra -O2 -I$(SRC) bench/bench.c $(ENGINE) -o bin/bench

integration:
	./tests/integration.sh

.PHONY: test unit integration bench
