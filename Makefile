# Aegir build entry point. See specs/build.md for the environment and targets.
#
# Long-running steps (toolchain fetch, builds, QEMU boots, test suites) are
# wrapped in `timeout` so a wedged process is detected instead of hanging the
# session (project rule).

SHELL := /bin/bash
PYTHON ?= python3

# Which target make build/run act on. The others are the rest of the
# memory/cores envelope (scripts/targets.py); `make envelope` walks them.
TARGET ?= aegir

# The hosted C++ runtime and the GUI toolkit are build switches, not target
# properties (specs/cxx.md), both on by default, so the greeter and bureau are
# the real ones. The lean build is `make run HOSTED_CXX=0`, where they are
# freestanding placeholders; `TOOLKIT=0` keeps the hosted runtime but leaves the
# toolkit out. Both also work from the environment (AEGIR_HOSTED_CXX /
# AEGIR_TOOLKIT). They are exported to scripts/run_target.py, which passes them
# to cmake and builds the runtime when they are on.
HOSTED_CXX ?= $(AEGIR_HOSTED_CXX)
TOOLKIT ?= $(AEGIR_TOOLKIT)
ifeq ($(TOOLKIT),1)
HOSTED_CXX := 1
endif
ifeq ($(HOSTED_CXX),0)
TOOLKIT := 0
endif
export AEGIR_HOSTED_CXX := $(HOSTED_CXX)
export AEGIR_TOOLKIT := $(TOOLKIT)

# Wall-clock limits. Generous, but finite.
TOOLS_TIMEOUT ?= 1800
BUILD_TIMEOUT ?= 1800
DEPS_TIMEOUT ?= 3600
BOOT_TIMEOUT ?= 300
TEST_TIMEOUT ?= 1200

.PHONY: all help tools tools-check lock-tools deps deps-force deps-check check-bidi check-locale check-translation check-terminal check-args check-script check-limits check-allocator check-fonts check-regions check-font-probe check-atlas check-layout check-scrollbar check-slider check-cycle check-theme theme-preview theme-preset build run run-ui envelope test clean distclean

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

check-bidi: ## run UAX #9 against Unicode's conformance suites (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_bidi.py

check-locale: ## run Locale formatting against the generated CLDR data (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_locale.py

check-translation: ## run the gettext .mo parser against in-memory images (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_translation.py

check-terminal: ## run the terminal grid against its host conformance cases (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_terminal.py

check-args: ## run aegir::args against its host conformance cases (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_args.py

check-script: ## run aegir::script against its host conformance cases (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_script.py

check-limits: ## run aegir::limits against its host conformance cases (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_limits.py

check-allocator: ## run aegir-mem's allocator against its host conformance cases (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_allocator.py

check-fonts: ## run the toolkit's font catalog against its host conformance cases (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_fonts.py

check-regions: ## run the heap's free-region list against its host conformance cases (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_regions.py

check-font-probe: ## run the font service's probe over the vendored faces (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_font_probe.py

check-atlas: ## run the toolkit's glyph atlas against its host conformance cases (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_atlas.py

check-layout: ## run the toolkit's sizing contract and group layout against its host cases (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_layout.py

check-scrollbar: ## run the scrollbar's thumb geometry against its host cases (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_scrollbar.py

check-slider: ## run the slider's knob geometry against its host cases (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_slider.py

check-cycle: ## run the cycle's stepping against its host cases (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_cycle.py

check-theme: ## render the toolkit's gadgets on the host and compare to the reference (host)
	timeout $(TOOLS_TIMEOUT) $(PYTHON) scripts/check_theme.py

theme-preview: check-theme ## alias for check-theme: render the theme and write the preview (host)

theme-preset: ## regenerate the theme's artwork table from MUI's XEN preset (host)
	$(PYTHON) scripts/convert_prefs.py

build: ## configure and build Aegir's own root task
	timeout $(BUILD_TIMEOUT) $(PYTHON) scripts/run_target.py --target $(TARGET) --build-only

run: ## boot Aegir under QEMU, stopping once it reports online
	timeout $(BOOT_TIMEOUT) $(PYTHON) scripts/run_target.py --target $(TARGET)

# run-ui is attended: QEMU's GTK window shows the gpu heads (one tab each),
# the serial console stays here, and the guest's test bed paces itself on
# keys *you* press -- the console says when, and which ('a', then 'b', then
# 'c'). QEMU exits when the window closes, so the project rule's `timeout`
# wrapper is the watched batch run's, not this one's: there is no wedged
# process to detect when the operator is sitting in front of it.
run-ui: ## boot Aegir with a GTK window on the displays; you press the keys
	$(PYTHON) scripts/run_target.py --target $(TARGET) --interactive

envelope: ## build and boot every target in the memory/cores envelope
	@for target in aegir aegir-2g-smp2 aegir-2g-smp4 aegir-8g-smp4; do \
		echo "== $$target"; \
		timeout $(BUILD_TIMEOUT) $(PYTHON) scripts/run_target.py --target $$target || exit 1; \
	done

test: ## build and boot the seL4 test suite on qemu-riscv-virt (acceptance test)
	timeout $(TEST_TIMEOUT) $(PYTHON) scripts/run_target.py --target sel4test

clean: ## remove build output, keep fetched tools
	rm -rf build out

distclean: clean ## also remove fetched tools and the download cache
	rm -rf third_party/toolchain third_party/tools third_party/download
