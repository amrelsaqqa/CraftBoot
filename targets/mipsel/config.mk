# SPDX-License-Identifier: GPL-2.0-or-later
# Target: mipsel (32-bit MIPS).  Builds qemu-system-mipsel; machines provide the -M types.
mipsel_TARGET_LIST := mipsel-softmmu
mipsel_BINARIES    := qemu-system-mipsel
mipsel_MACHINES    := mt7621
