#!/bin/bash
# q2pro - launcher with Discord SDK support
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
export DYLD_LIBRARY_PATH="$DIR:$DYLD_LIBRARY_PATH"
exec "$DIR/q2pro.bin" "$@"