# SPDX-License-Identifier: GPL-2.0-or-later
# Build recipes for QEMU board emulation.
#
#   target  = what builds into one qemu-system-<arch> executable (mips, arm)
#   machine = a -M type inside that executable (mt7621, ipq4019)
#   board   = a machine property/preset (-M mt7621,board=lg-gapm7200)
#
# Selection (comma-separated; a bare architecture means all its machines):
#   make TARGET=mipsel                  all machines of mipsel
#   make TARGET=mipsel/mt7621           one machine
#   make TARGET=mipsel/*,arm/*          whole architectures
#   make TARGET=mipsel/mt7621,mips/mt7622
#   make                              whatever .config selects
#
# Actions: check-patches, prep, build, sync, export-patches, list, defconfig,
# clean (build/), distclean (build/ + cache).  `build` implies prep when the
# tree does not exist yet; once it does, `build` and `sync` are incremental
# (machine sources are re-copied, ninja rebuilds what changed).  The QEMU tree
# is never committed: it is downloaded into a shared cache, patched in a
# throw-away tree, and built.

SHELL := /bin/bash
include config.mk
include $(wildcard targets/*/config.mk)
include $(wildcard targets/*/machines/*/config.mk)

TARGET   ?=
TARGETS  ?=
MACHINES ?= $($(TARGET)_MACHINES)
SPECS     = $(if $(TARGET),$(TARGET),$(TARGETS))
SEL       = scripts/selected.sh

T        = $(TARGET)
TDIR     = targets/$(T)
TAG      = $(or $($(T)_QEMU_TAG),$(QEMU_TAG_DEFAULT))
CACHE    = $(QEMU_CACHE)/qemu-$(TAG)
SUBDIR   = build/$(T)
SRC      = $(SUBDIR)/src
OBJ      = $(SUBDIR)/build
BINARIES  = $($(T)_BINARIES)
DEV      ?= dev/qemu
DEV_BRANCH ?= $($(T)_DEV_BRANCH)

# Patch series, applied in this order: shared, target-wide, then per machine.
SERIES   = $(wildcard patches/generic/series) $(TDIR)/series \
           $(wildcard $(foreach m,$(MACHINES),$(TDIR)/machines/$(m)/series))

.PHONY: all list defconfig fetch prep build sync check-patches export-patches \
        clean distclean

DIRECT := $(wildcard targets/$(TARGET)/config.mk)

# =========================================================================
# Selector mode: TARGET is empty, an architecture, or arch/machine[, …]
# =========================================================================
ifeq ($(DIRECT),)

ACTIONS = prep build sync check-patches export-patches

all: build

$(ACTIONS):
	@out=$$($(SEL) "$(SPECS)") || exit 1; \
	[ -n "$$out" ] || { echo "nothing selected"; exit 1; }; \
	echo "$$out" | while IFS='|' read -r t ms; do \
	    [ -z "$$t" ] && continue; \
	    $(MAKE) --no-print-directory TARGET="$$t" MACHINES="$$ms" $@ || exit 1; \
	done

list:
	@sel=$$($(SEL) "$(SPECS)") || exit 1; \
	sel=$$(echo "$$sel" | awk -F'|' '{n=split($$2,a," "); for(i=1;i<=n;i++) print $$1 "/" a[i]}' | tr '\n' ' '); \
	for d in targets/*/; do t=$$(basename $$d); \
	    for m in $$(ls $$d/machines 2>/dev/null); do \
	        case " $$sel " in *" $$t/$$m "*) mark='x';; *) mark=' ';; esac; \
	        if grep -qE "^$${t}_BINARIES[[:space:]]*:?=" "$$d/config.mk" 2>/dev/null; \
	            then w='wired'; else w='not wired'; fi; \
	        echo "[$$mark] $$t/$$m  ($$w)"; \
	    done; \
	done

defconfig:
	@{ echo "# build selection; TARGET=... on the command line overrides it"; \
	   for d in targets/*/; do t=$$(basename $$d); \
	       grep -qE "^$${t}_BINARIES[[:space:]]*:?=" "$$d/config.mk" 2>/dev/null || continue; \
	       echo "CONFIG_TARGET_$$t=y"; \
	       for m in $$(ls $$d/machines 2>/dev/null); do echo "CONFIG_MACHINE_$$m=y"; done; \
	   done; } > .config
	@echo ".config written (wired targets only)"

else
# =========================================================================
# Direct mode: TARGET names one architecture, MACHINES its machines
# =========================================================================

all: build

print-wired:
	@echo $(BINARIES)

# ---- fetch: download the pinned QEMU tag into the shared cache ------------
fetch:
	@if [ ! -d "$(CACHE)" ]; then \
	    echo "fetch $(TAG) -> $(CACHE)"; \
	    mkdir -p "$(QEMU_CACHE)"; \
	    git clone --depth 1 --branch $(TAG) $(QEMU_REPO) "$(CACHE)"; \
	fi

# ---- prep: extract the tag, git-init, copy machines, apply the series ----
prep: fetch
	@test -n "$(MACHINES)" || { echo "target $(TARGET) is not wired up (no machines)"; exit 1; }
	@rm -rf "$(SUBDIR)"; mkdir -p "$(SRC)"
	@echo "prep $(TARGET): $(MACHINES) on $(TAG)"
	@git -C "$(CACHE)" archive --format=tar $(TAG) | tar -x -C "$(SRC)"
	@cd "$(SRC)" && git init -q && git add -A && \
	    git -c user.email=build@local -c user.name=build commit -q \
	        -m "qemu $(TAG) baseline"
	@set -e; $(foreach m,$(MACHINES),echo "  machine $(m)"; $(foreach p,$($(m)_MACHINE_SRCS),mkdir -p "$(SRC)/$$(dirname $(word 2,$(subst :, ,$(p))))"; cp "$(TDIR)/machines/$(m)/$(word 1,$(subst :, ,$(p)))" "$(SRC)/$(word 2,$(subst :, ,$(p)))"; ))
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

# ---- build: prep on demand, configure once, then ninja the binaries ------
# Machine sources are re-copied on every build so editing <soc>.c is an
# incremental rebuild: only that file recompiles.  `prep` is the only thing
# that throws the tree away (needed when the pinned tag or a series changes).
build:
	@test -n "$(BINARIES)" || { echo "target $(TARGET) is not wired up (no BINARIES)"; exit 1; }
	@test -f "$(SUBDIR)/.prepped" || \
	    $(MAKE) --no-print-directory TARGET="$(TARGET)" MACHINES="$(MACHINES)" prep
	@set -e; $(foreach m,$(MACHINES),$(foreach p,$($(m)_MACHINE_SRCS),cmp -s "$(TDIR)/machines/$(m)/$(word 1,$(subst :, ,$(p)))" "$(SRC)/$(word 2,$(subst :, ,$(p)))" || { echo "  sync $(m)/$(word 1,$(subst :, ,$(p)))"; mkdir -p "$(SRC)/$$(dirname $(word 2,$(subst :, ,$(p))))"; cp "$(TDIR)/machines/$(m)/$(word 1,$(subst :, ,$(p)))" "$(SRC)/$(word 2,$(subst :, ,$(p)))"; };))
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

# ---- sync: the fast path — machine sources in, ninja out -----------------
# For iterating on <soc>.c: no prep, no configure, no fetch.  The machine
# source is copied over the one in the tree and ninja rebuilds just that.
sync:
	@test -f "$(SUBDIR)/.prepped" || { \
	    echo "no tree for $(TARGET): run make TARGET=$(TARGET) prep first"; \
	    exit 1; }
	@set -e; $(foreach m,$(MACHINES),$(foreach p,$($(m)_MACHINE_SRCS),cmp -s "$(TDIR)/machines/$(m)/$(word 1,$(subst :, ,$(p)))" "$(SRC)/$(word 2,$(subst :, ,$(p)))" || { echo "  sync $(m)/$(word 1,$(subst :, ,$(p)))"; mkdir -p "$(SRC)/$$(dirname $(word 2,$(subst :, ,$(p))))"; cp "$(TDIR)/machines/$(m)/$(word 1,$(subst :, ,$(p)))" "$(SRC)/$(word 2,$(subst :, ,$(p)))"; };))
	ninja -C "$(OBJ)" $(BINARIES)
	@for b in $(BINARIES); do ln -sf "$(PWD)/$(OBJ)/$$b" "$(SUBDIR)/$$b"; done
	@echo "synced $(SUBDIR): $(BINARIES)"

# ---- export-patches: dev branch -> target patches + machine sources ------
export-patches:
	@test -n "$(DEV_BRANCH)" || { echo "no DEV_BRANCH for target $(TARGET)"; exit 1; }
	@test -d "$(DEV)" || { echo "no dev checkout at $(DEV) (set DEV=…)"; exit 1; }
	@mkdir -p "$(TDIR)/patches"; rm -f "$(TDIR)"/patches/*.patch
	git -C "$(DEV)" format-patch --no-signature $(TAG)..$(DEV_BRANCH) \
	    -o "$(PWD)/$(TDIR)/patches" -- . \
	    $(foreach m,$(MACHINES),$(foreach p,$($(m)_MACHINE_SRCS),':(exclude)'$(word 2,$(subst :, ,$(p)))) \
	    )
	@set -e; $(foreach m,$(MACHINES),$(foreach p,$($(m)_MACHINE_SRCS),cp "$(DEV)/$(word 2,$(subst :, ,$(p)))" "$(TDIR)/machines/$(m)/$(word 1,$(subst :, ,$(p)))";))
	@{ echo "# Target $(TARGET) — applied in this order to $(TAG)."; \
	   echo "# 'make TARGET=$(TARGET) check-patches' proves they apply cleanly."; \
	   ls "$(TDIR)"/patches/*.patch; } > "$(TDIR)/series"
	@echo "exported: $(TARGET) from $(DEV_BRANCH) ($(TAG))"

# ---- check-patches: prove every series applies to a pristine tag ---------
check-patches: fetch
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

list:
	@echo "$(TARGET): machines=$(MACHINES) wired=$(if $(BINARIES),yes,no)"

endif

# =========================================================================
# Common
# =========================================================================
clean:
	@rm -rf build
	@echo "cleaned build/ (QEMU cache kept in $(QEMU_CACHE))"

distclean: clean
	@rm -rf "$(QEMU_CACHE)"
	@echo "removed QEMU cache $(QEMU_CACHE)"
