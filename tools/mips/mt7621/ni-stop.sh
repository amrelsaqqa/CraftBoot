#!/bin/sh
# Stop every netinstall-cli instance (root ones need the sudoers rule).
# Kept in a file so the caller's own command line does not contain the pattern
# that "pkill -f netinstall-cli" matches.
exec sudo -n /usr/sbin/pkill -f netinstall-cli
