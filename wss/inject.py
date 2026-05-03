#!/usr/bin/env python3
"""
Mimic an inbound TargetUpdate over the target-manager UDS.

Examples:
  # Suppress a track by its packed id (as logged by target-manager):
  python3 inject.py --socket /tmp/tm-inbound.sock --id 4294967296 --state inactive

  # Or by (class_id, object_id) pair — script does the packing:
  python3 inject.py --socket /tmp/tm-inbound.sock --class-id 0 --object-id 12 --state inactive
"""

import argparse
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from target_proto_pb2 import (  # noqa: E402
    TargetUpdate,
    TARGET_STATE_ACTIVE,
    TARGET_STATE_INACTIVE,
    TARGET_STATE_ACQUIRED,
    TARGET_STATE_LOST,
    TARGET_STATE_NEUTRALIZED,
    TARGET_STATE_UNKNOWN,
)

STATES = {
    "unknown":     TARGET_STATE_UNKNOWN,
    "active":      TARGET_STATE_ACTIVE,
    "inactive":    TARGET_STATE_INACTIVE,
    "acquired":    TARGET_STATE_ACQUIRED,
    "lost":        TARGET_STATE_LOST,
    "neutralized": TARGET_STATE_NEUTRALIZED,
}


def pack_id(class_id: int, object_id: int) -> int:
    # Matches target_manager.hpp Target ctor: 16-bit packed id where the
    # upper byte is class_id and the lower byte is object_id mod 256.
    return ((class_id & 0xFF) << 8) | (object_id & 0xFF)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--socket", required=True, help="path to target-manager UDS")
    ap.add_argument("--state", choices=list(STATES), default="inactive")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--id", type=lambda x: int(x, 0), help="packed uint64 id")
    g.add_argument("--class-id", type=int, help="class id (use with --object-id)")
    ap.add_argument("--object-id", type=int, help="tracker object id (use with --class-id)")
    args = ap.parse_args()

    if args.id is None:
        if args.object_id is None:
            ap.error("--object-id required when using --class-id")
        packed = pack_id(args.class_id, args.object_id)
    else:
        packed = args.id

    msg = TargetUpdate(id=packed, state=STATES[args.state])
    payload = msg.SerializeToString()

    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        s.sendto(payload, args.socket)
    finally:
        s.close()
    print(f"sent {len(payload)}B id={packed} state={args.state} -> {args.socket}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
