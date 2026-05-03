"""Utilities for converting between target dicts (server.py schema) and TargetResponse proto bytes."""

import base64
import time
import uuid

from target_proto_pb2 import (
    TARGET_STATE_UNKNOWN,
    TARGET_STATE_ACTIVE,
    TARGET_STATE_INACTIVE,
    TARGET_STATE_ACQUIRED,
    TARGET_STATE_LOST,
    TARGET_STATE_NEUTRALIZED,
    TargetResponse,
    Timestamp,
    TrackingLocationDto,
)

_STATE_TO_ENUM = {
    "TARGET_STATE_UNKNOWN":     TARGET_STATE_UNKNOWN,
    "TARGET_STATE_ACTIVE":      TARGET_STATE_ACTIVE,
    "TARGET_STATE_INACTIVE":    TARGET_STATE_INACTIVE,
    "TARGET_STATE_ACQUIRED":    TARGET_STATE_ACQUIRED,
    "TARGET_STATE_LOST":        TARGET_STATE_LOST,
    "TARGET_STATE_NEUTRALIZED": TARGET_STATE_NEUTRALIZED,
}

_ENUM_TO_STATE = {v: k for k, v in _STATE_TO_ENUM.items()}


def target_dict_to_bytestring(target: dict) -> bytes:
    """Serialize a target dict (server.py schema) into a TargetResponse proto bytestring."""
    loc = target.get("tracking_location") or {}
    upd = target.get("updated_date") or {}
    state_str = target.get("state", "TARGET_STATE_UNKNOWN")

    proto = TargetResponse(
        id=target.get("id", str(uuid.uuid4())),
        updated_date=Timestamp(
            seconds=upd.get("seconds", int(time.time())),
            nanos=upd.get("nanos", 0),
        ),
        tracking_location=TrackingLocationDto(
            longitude=loc.get("longitude", 0),
            latitude=loc.get("latitude", 0),
            timestamp=loc.get("timestamp", int(time.time())),
            altitude=loc.get("altitude", 0),
            speedOverGround=loc.get("speed_over_ground", 0),
            courseOverGround=loc.get("course_over_ground", 0),
        ),
        state=_STATE_TO_ENUM.get(state_str, TARGET_STATE_UNKNOWN),
        workspace_id=target.get("workspace_id", ""),
    )
    return proto.SerializeToString()


def bytestring_to_target_dict(data: bytes) -> dict:
    """Deserialize a TargetResponse proto bytestring into a target dict (server.py schema)."""
    proto = TargetResponse()
    proto.ParseFromString(data)

    loc = proto.tracking_location
    upd = proto.updated_date

    return {
        "id": proto.id,
        "updated_date": {
            "seconds": upd.seconds,
            "nanos":   upd.nanos,
        },
        "tracking_location": {
            "longitude":         loc.longitude,
            "latitude":          loc.latitude,
            "timestamp":         loc.timestamp,
            "altitude":          loc.altitude,
            "speed_over_ground": loc.speedOverGround,
            "course_over_ground": loc.courseOverGround,
        },
        "state":        _ENUM_TO_STATE.get(proto.state, "TARGET_STATE_UNKNOWN"),
        "workspace_id": proto.workspace_id,
    }


def base64_to_target_dict(b64: str) -> dict:
    """Decode a base64-encoded TargetResponse proto string into a target dict."""
    return bytestring_to_target_dict(base64.b64decode(b64))
