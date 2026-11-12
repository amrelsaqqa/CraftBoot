# Build recipes for QEMU board emulation.
#
#   target  = what builds into one qemu-system-<arch> executable (mips, arm)
#   machine = a -M type inside that executable (mt7621, ipq4019)
#   board   = a machine property/preset (-M mt7621,board=lg-gapm7200)
#
# The QEMU tree is never committed here: it is downloaded into a shared cache,
# patched in a throw-away tree, and built.
#
#   make TARGET=mips check-patches prep build
#   make TARGET=mips MACHINES=mt7621 prep build      # subset of machines
#   make TARGET=mips clean                           # build/ only
#   make distclean                                   # build/ + cache
#   make TARGET=mips export-patches DEV=<qemu clone> # branch -> patches/
#
# Layout produced:
#   build/<target>/src    the QEMU tag extracted, git-initialised, series
#                         applied as commits (git log / bisect work there)
#   build/<target>/build  configure/ninja output; binaries symlinked above

SHELL := /bin/bash
include config.mk
include $(wildcard targets/*/config.mk)
include $(wildcard targets/*/machines/*/config.mk)

TARGET   ?=
TARGETS  ?= $(TARGET)

T        = $(TARGET)
TDIR     = targets/$(T)
TAG      = $(or $($(T)_QEMU_TAG),$(QEMU_TAG_DEFAULT))
CACHE    = $(QEMU_CACHE)/qemu-$(TAG)
SUBDIR   = build/$(T)
SRC      = $(SUBDIR)/src
OBJ      = $(SUBDIR)/build
MACHINES ?= $($(T)_MACHINES)
BINARIES  = $($(T)_BINARIES)
DEV      ?= dev/qemu
DEV_BRANCH ?= $($(T)_DEV_BRANCH)

# Patch series, applied in this order: shared, target-wide, then per machine.
SERIES   = $(wildcard patches/generic/series) $(TDIR)/series \
           $(wildcard $(foreach m,$(MACHINES),$(TDIR)/machines/$(m)/series))

.PHONY: all list fetch prep build check-patches export-patches clean distclean

all: $(TARGETS)

$(TARGETS):
	@$(MAKE) --no-print-directory TARGET=$@ prep build

list:
	@for d in targets/*/; do t=$$(basename $$d); \
	    echo "target $$t: machines=$$(ls $$d/machines 2>/dev/null | tr '\n' ' ')"; \
	done

# ---- fetch: download the pinned QEMU tag into the shared cache ------------
fetch:
	@if [ ! -d "$(CACHE)" ]; then \
	    echo "fetch $(TAG) -> $(CACHE)"; \
	    mkdir -p "$(QEMU_CACHE)"; \
	    git clone --depth 1 --branch $(TAG) $(QEMU_REPO) "$(CACHE)"; \
	fi

# ---- prep: extract the tag, git-init, copy machines, apply the series ----
prep: fetch
	@test -n "$(TARGET)" || { echo "usage: make TARGET=<target> prep"; exit 1; }
	@test -n "$(MACHINES)" || { echo "target $(TARGET) lists no machines"; exit 1; }
	@rm -rf "$(SUBDIR)"; mkdir -p "$(SRC)"
	@echo "prep $(TARGET): $(MACHINES) on $(TAG)"
	@git -C "$(CACHE)" archive --format=tar $(TAG) | tar -x -C "$(SRC)"
	@cd "$(SRC)" && git init -q && git add -A && \
	    git -c user.email=build@local -c user.name=build commit -q \
	        -m "qemu $(TAG) baseline"
	@set -e; $(foreach m,$(MACHINES),echo "  machine $(m)"; cp "$(TDIR)/machines/$(m)/$(m).c" "$(SRC)/$($(m)_MACHINE_DST)";)
	@set -e; for s in $(SERIES); do \
	    while read -r p; do \
	        [ -z "$$p" ] && continue; \
	        case "$$p" in \#*) continue;; esac; \
	        echo "  am $$p"; \
	        git -C "$(SRC)" -c user.email=build@local -c user.name=build \
	            am -3 --keep-cr "$(PWD)/$$p" >/dev/null || { \
	            echo "FAILED: $$p"; git -C "$(SRC)" am --abort 2>/dev/null; exit 1; }; \
	    done < "$$s"; \
	done
	@touch "$(SUBDIR)/.prepped"

# ---- build: configure once, then ninja the target's binaries -------------
build: $(SUBDIR)/.prepped
	@if [ ! -f "$(OBJ)/build.ninja" ]; then \
	    mkdir -p "$(OBJ)"; \
	    echo "configure $(TARGET)"; \
	    (cd "$(OBJ)" && "$(PWD)/$(SRC)/configure" \
	        --target-list=$($(T)_TARGET_LIST) \
	        $(CONFIGURE_ARGS) $($(T)_CONFIGURE)); \
	fi
	ninja -C "$(OBJ)" $(BINARIES)
	@for b in $(BINARIES); do ln -sf "$(PWD)/$(OBJ)/$$b" "$(SUBDIR)/$$b"; done
	@echo "built in $(SUBDIR): $(BINARIES)"

# ---- export-patches: dev branch -> target patches + machine sources ------
export-patches:
	@test -n "$(TARGET)" || { echo "usage: make TARGET=<target> export-patches"; exit 1; }
	@test -d "$(DEV)" || { echo "no dev checkout at $(DEV) (set DEV=…)"; exit 1; }
	@test -n "$(DEV_BRANCH)" || { echo "no DEV_BRANCH for target $(TARGET)"; exit 1; }
	@mkdir -p "$(TDIR)/patches"; rm -f "$(TDIR)"/patches/*.patch
	git -C "$(DEV)" format-patch --no-signature $(TAG)..$(DEV_BRANCH) \
	    -o "$(PWD)/$(TDIR)/patches" -- . \
	    $(foreach m,$(MACHINES),':(exclude)'$($(m)_MACHINE_DST))
	@set -e; $(foreach m,$(MACHINES),cp "$(DEV)/$($(m)_MACHINE_DST)" "$(TDIR)/machines/$(m)/$(m).c";)
	@{ echo "# Target $(TARGET) — applied in this order to $(TAG)."; \
	   echo "# 'make TARGET=$(TARGET) check-patches' proves they apply cleanly."; \
	   ls "$(TDIR)"/patches/*.patch; } > "$(TDIR)/series"
	@echo "exported: $(TARGET) from $(DEV_BRANCH) ($(TAG))"

# ---- check-patches: prove every series applies to a pristine tag ---------
check-patches: fetch
	@test -n "$(TARGET)" || { echo "usage: make TARGET=<target> check-patches"; exit 1; }
	@mkdir -p build; tmp=$$(mktemp -d build/.check-XXXXXX); \
	git -C "$(CACHE)" archive --format=tar $(TAG) | tar -x -C "$$tmp"; \
	(cd "$$tmp" && git init -q && git add -A && \
	    git -c user.email=build@local -c user.name=build commit -q \
	        -m "qemu $(TAG) baseline"); \
	ok=1; \
	for s in $(SERIES); do \
	    while read -r p; do \
	        [ -z "$$p" ] && continue; \
	        case "$$p" in \#*) continue;; esac; \
	        if ! git -C "$$tmp" -c user.email=build@local -c user.name=build \
	                am -3 --keep-cr "$(PWD)/$$p" >/dev/null 2>&1; then \
	            echo "series FAILED at $$p"; ok=0; break 2; \
	        fi; \
	    done < "$$s"; \
	done; \
	rm -rf "$$tmp"; \
	[ $$ok = 1 ] && echo "series applies cleanly: $(TARGET) ($(MACHINES)) on $(TAG)"

# ---- clean / distclean ---------------------------------------------------
clean:
	@rm -rf build
	@echo "cleaned build/ (QEMU cache kept in $(QEMU_CACHE))"

distclean: clean
	@rm -rf "$(QEMU_CACHE)"
	@echo "removed QEMU cache $(QEMU_CACHE)"
