# Stage 3: tests, benchmark and demo

Everything here is built from the Stage 2 sources.

```bash
make test        # unit tests, then integration tests
make unit        # tests/test_units.c only
make integration # tests/integration.sh only
make bench       # throughput for each sync mode (built with -O2, no sanitizers)
./demo.sh        # step-by-step walkthrough (DEMO_NOPAUSE=1 to run straight through)
```

- `tests/test_units.c` links the engine directly. It tests the record codec, the tokenizer, the index and the
  store, using a temporary directory and a fake clock.
- `tests/integration.sh` builds Stage 1, then builds Stage 2 with `-fsanitize=address,undefined`. It runs both
  with scripts on stdin and checks their output, their exit status and the files they leave behind.
