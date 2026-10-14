#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
ADAPTER="${ADAPTER_SO:-/usr/lib/libsai_opi_adapter.so}"
if [ -f "$ADAPTER" ]; then
  # Always absolute — relative basename links break if cwd != /usr/lib.
  ln -sfn "$ADAPTER" /usr/lib/libsai.so
  echo "syncd-lab: libsai.so -> $ADAPTER"
  if command -v ldd >/dev/null 2>&1; then
    if ! ldd "$ADAPTER" >/tmp/ldd.adapter 2>&1; then
      echo "syncd-lab: WARNING ldd failed on $ADAPTER:" >&2
      cat /tmp/ldd.adapter >&2 || true
    elif grep -q 'not found' /tmp/ldd.adapter; then
      echo "syncd-lab: WARNING missing shared libs for $ADAPTER:" >&2
      grep 'not found' /tmp/ldd.adapter >&2 || true
      echo "syncd-lab: rebuild adapter on this host or install matching libgrpc++/libprotobuf" >&2
    fi
  fi
else
  echo "syncd-lab: WARNING $ADAPTER not mounted; SAI calls will fail until it is" >&2
fi
export LD_LIBRARY_PATH="/usr/lib:/usr/local/lib:${LD_LIBRARY_PATH:-}"
exec python -u /opt/syncd/syncd.py
