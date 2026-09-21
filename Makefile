# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# SMAOE Build System
# Three implementations: C11 POSIX, C89 Hybrid Lisp, Magazine Type-In

.PHONY: all clean verify cbmc help

all: smaoe smaoe_lisp smaoe_typein

# C11 POSIX engine (mmap, atomics, pthreads)
smaoe: smaoe.c
	@echo "=== Building SMAOE (C11 POSIX) ==="
	gcc -std=c11 -O2 -pthread -Wall -Wextra -o $@ $<
	@echo "OK: $@"

# C89 Hybrid Lisp orchestrator (fork, pipes, macro evaluator)
smaoe_lisp: smaoe_lisp.c
	@echo "=== Building SMAOE Lisp (C89) ==="
	gcc -std=c89 -pedantic -Wall -Wextra -o $@ $<
	@echo "OK: $@"

# Magazine type-in (C89, checksum-verified hex image)
smaoe_typein: smaoe_typein.c
	@echo "=== Building SMAOE Type-In (C89) ==="
	gcc -std=c89 -pedantic -Wall -Wextra -o $@ $<
	@echo "OK: $@"

# CBMC bounded model checking (Lisp engine)
cbmc: smaoe_lisp.c
	@echo "=== CBMC Verification ==="
	cbmc smaoe_lisp.c \
		--function cbmc_array_bounds_harness \
		--bounds-check \
		--pointer-check \
		--signed-overflow-check \
		--unsigned-overflow-check \
		--unwind 12 \
		--unwinding-assertions \
		--stop-on-fail

# Sanitizer build
verify: smaoe.c smaoe_lisp.c
	@echo "=== Building with Sanitizers ==="
	gcc -std=c11 -O0 -g -pthread -fsanitize=address,undefined -o smaoe_verify smaoe.c
	gcc -std=c89 -O0 -g -fsanitize=address,undefined -o smaoe_lisp_verify smaoe_lisp.c
	@echo "OK: sanitizer builds"

clean:
	rm -f smaoe smaoe_lisp smaoe_typein smaoe_verify smaoe_lisp_verify
	rm -f *.journal

help:
	@echo "SMAOE Build System"
	@echo "==================="
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build all three implementations"
	@echo "  smaoe        - C11 POSIX engine (mmap, atomics)"
	@echo "  smaoe_lisp   - C89 hybrid Lisp orchestrator (fork, pipes)"
	@echo "  smaoe_typein - Magazine type-in (hex image, checksum)"
	@echo "  verify       - Build with address/undefined sanitizers"
	@echo "  cbmc         - Run CBMC bounded model checking"
	@echo "  clean        - Remove build artifacts"
	@echo ""
	@echo "Requirements:"
	@echo "  - GCC (C11 for smaoe, C89 for others)"
	@echo "  - POSIX system (fork, pipe, mmap)"
	@echo "  - Optional: CBMC for formal verification"
	@echo ""
