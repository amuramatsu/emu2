#! /bin/sh
AR=ar
PLUGIN=
uname_s="$(uname -s)"
case $uname_s in
    FreeBSD)
        AR=llvm-ar;;
    NetBSD|OpenBSD)
        PLUGIN=/usr/libexec/liblto_plugin.so;;
    Darwin)
        AR=ar;;
    Linux)
        # When CC contains clang and llvm-ar is exist, llvm-ar is used
        if [ ! -z "$CC" ] && (echo $CC | grep clang >/dev/null 2>&1) && 
               type llvm-ar >/dev/null 2>&1; then
            AR=llvm-ar

        # Else when gcc-ar is exist, use it
        elif type gcc-ar >/dev/null 2>&1; then
            AR=gcc-ar
        fi;;
    *)
        ;;
esac
if [ ! -z "$PLUGIN" ] && [ -r "$PLUGIN" ]; then
    exec $AR --plugin "$PLUGIN" "$@"
else
    exec $AR "$@"
fi
