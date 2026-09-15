# Aegir build entry point. See specs/build.md for the environment and targets.
#
# Long-running steps (toolchain fetch, builds, QEMU boots, test suites) are
# wrapped in `timeout` so a wedged process is detected instead of hanging the
# session (project rule).

SHELL := /bin/bash
PYTHON ?= python3

# Wall-clock limits. Generous, but finite.
TOOLS_TIMEOUT ?= 1800
BUILD_TIMEOUT ?= 1800
DEPS_TIMEOUT ?= 3600
BOOT_TIMEOUT ?= 300
TEST_TIMEOUT ?= 1200

.PHONY: all help tools tools-check lock-tools deps deps-force deps-check build run test clean distclean

all: help

help: ## list available targets
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "} {printf "  %-14s %s\n", $$1, $$2}'

tools: ## fetch the pinned RISC-V toolchain and host build tools
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/fetch_toolchain.py
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/setup_tools.py
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/build_dtc.py

tools-check: ## verify the fetched tools match their pins
	$(PYTHON) scripts/fetch_toolchain.py --check
	$(PYTHON) scripts/setup_tools.py --check
	$(PYTHON) scripts/build_dtc.py --check
	$(PYTHON) scripts/lock_tools.py --check

lock-tools: ## regenerate the hashed host-tool lock file (network)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/lock_tools.py

deps: ## fetch vendored sources at their pinned revisions
	timeout $(DEPS_TIMEOUT) $(PYTHON) scripts/sync_deps.py

deps-force: ## re-fetch, discarding local changes in vendored trees
	timeout $(DEPS_TIMEOUT) $(PYTHON) scripts/sync_deps.py --force

deps-check: ## verify vendored trees match their pins, patches and licenses
	$(PYTHON) scripts/check_pins.py

build: ## configure and build Aegir's own root task
	timeout $(BUILD_TIMEOUT) $(PYTHON) scripts/run_target.py --target aegir --build-only

run: ## boot Aegir under QEMU, stopping once it reports online
	timeout $(BOOT_TIMEOUT) $(PYTHON) scripts/run_target.py --target aegir

test: ## build and boot the seL4 test suite on qemu-riscv-virt (acceptance test)
	timeout $(TEST_TIMEOUT) $(PYTHON) scripts/run_target.py --target sel4test

clean: ## remove build output, keep fetched tools
	rm -rf build out

distclean: clean ## also remove fetched tools and the download cache
	rm -rf third_party/toolchain third_party/tools third_party/download
