.PHONY: format lint lint-fix lint-verbose clean-lint help

SOURCES := $(shell find src tests -type f \( -name "*.c" -o -name "*.h" \))

format:
	@clang-format -i $(SOURCES)

format-file:
	@clang-format -i $(FILE)

lint:
	find src -name "*.c" | xargs clang-tidy -p build

lint-fix:
	@echo "Running clang-tidy with auto-fix..."
	@clang-tidy -p build $(SOURCES) --fix-errors

lint-file:
	@test -n "$(FILE)" || (echo "Usage: make lint-file FILE=path/to/file.c" && exit 1)
	@clang-tidy -p build $(FILE)

help:
	@printf "\nAvailable targets:\n\n"
	@printf "Formatting:\n"
	@printf "%-40s %s\n" "make format" "Run clang-format"
	@printf "%-40s %s\n" "make format-file FILE=src/file.c" "Format single file"
	@printf "\n"
	@printf "Linting:\n"
	@printf "%-40s %s\n" "make lint" "Run clang-tidy"
	@printf "%-40s %s\n" "make lint-fix" "Auto-fix issues where possible"
	@printf "%-40s %s\n" "make lint-file FILE=src/file.c" "Check single file"
	@printf "\n"
