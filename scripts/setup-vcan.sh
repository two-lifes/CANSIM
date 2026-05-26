#!/usr/bin/env bash
set -euo pipefail

IFACE="${1:-vcan0}"

if ip link show "$IFACE" &>/dev/null; then
  echo "$IFACE already exists"
else
  echo "Creating $IFACE (requires sudo)..."
  sudo ip link add dev "$IFACE" type vcan
fi

sudo ip link set "$IFACE" up
echo "$IFACE is up. Verify with: candump $IFACE"
