#!/bin/sh

# Avoid adding value twice if nested.
case "$GLIBC_TUNABLES" in
    *glibc.rtld.optional_static_tls=4096*)
        return
    ;;
esac

export GLIBC_TUNABLES=glibc.rtld.optional_static_tls=4096
