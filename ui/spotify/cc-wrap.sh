#!/bin/sh
# The C parts of the Spotify library's dependencies (ring) are built with the firmware's own C compiler (SPOT_REAL_CC,
# already aimed at the Disc); the cc crate adds a clang --target that compiler doesn't need, so it is dropped here.
for a in "$@"; do shift; case "$a" in --target=*) ;; *) set -- "$@" "$a";; esac; done
exec $SPOT_REAL_CC "$@"
