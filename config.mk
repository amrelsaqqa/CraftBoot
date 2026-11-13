# SPDX-License-Identifier: GPL-2.0-or-later
# Shared build settings.
#
# The QEMU source is never stored here: each target pins a tag, downloads it
# once into QEMU_CACHE, applies its ordered series and builds.

QEMU_REPO      ?= https://gitlab.com/qemu-project/qemu.git
QEMU_TAG_DEFAULT ?= v10.2.2

# Shared download cache: one subdirectory per tag.
QEMU_CACHE     ?= $(HOME)/.cache/qemu-src

# Extra arguments for QEMU's configure, common to every target.
CONFIGURE_ARGS ?= --disable-docs --disable-tools --disable-guest-agent \
                  --disable-werror
