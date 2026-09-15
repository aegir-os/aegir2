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
RUN_TIMEOUT ?= 120

.PHONY: all help tools tools-check clean distclean

all: help

help: ## list available targets
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "} {printf "  %-14s %s\n", $$1, $$2}'

tools: ## fetch the pinned RISC-V toolchain and host build tools
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/fetch_toolchain.py
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/setup_tools.py

tools-check: ## verify the fetched tools match their pins
	$(PYTHON) scripts/fetch_toolchain.py --check
	$(PYTHON) scripts/setup_tools.py --check

clean: ## remove build output, keep fetched tools
	rm -rf build out

distclean: clean ## also remove fetched tools and the download cache
	rm -rf third_party/toolchain third_party/tools third_party/download
