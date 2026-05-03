"""Utilities for converting between target dicts (server.py schema) and TargetResponse proto bytes."""

import base64
import time

from target_proto_pb2 import (
    TARGET_STATE_UNKNOWN,
    TARGET_STATE_CONFIRMED,
    TARGET_STATE_NEUTRALIZED,
    TARGET_STATE_INACTIVE,
    TargetResponse,
    TargetResponseDeltaList,
    Timestamp,
    TrackingLocationDto,
)

_STATE_TO_ENUM = {
    "TARGET_STATE_UNKNOWN":     TARGET_STATE_UNKNOWN,
    "TARGET_STATE_CONFIRMED":   TARGET_STATE_CONFIRMED,
    "TARGET_STATE_NEUTRALIZED": TARGET_STATE_NEUTRALIZED,
    "TARGET_STATE_INACTIVE":    TARGET_STATE_INACTIVE,
}

# State machine: UNKNOWN → CONFIRMED | INACTIVE; CONFIRMED → NEUTRALIZED; terminals: INACTIVE, NEUTRALIZED
_VALID_TRANSITIONS: dict[str, set[str]] = {
    "TARGET_STATE_UNKNOWN":     {"TARGET_STATE_CONFIRMED", "TARGET_STATE_INACTIVE"},
    "TARGET_STATE_CONFIRMED":   {"TARGET_STATE_NEUTRALIZED"},
    "TARGET_STATE_NEUTRALIZED": set(),
    "TARGET_STATE_INACTIVE":    set(),
}


def is_valid_state_transition(from_state: str, to_state: str) -> bool:
    """Return True if the from→to transition is allowed by the state machine."""
    if from_state == to_state:
        return True
    return to_state in _VALID_TRANSITIONS.get(from_state, set())

_ENUM_TO_STATE = {v: k for k, v in _STATE_TO_ENUM.items()}


def target_dict_to_bytestring(target: dict, workspace_id: int = 0) -> bytes:
    """Serialize a target dict (server.py schema) into a TargetResponse proto bytestring."""
    loc = target.get("tracking_location") or {}
    upd = target.get("updated_date") or {}
    state_str = target.get("state", "TARGET_STATE_UNKNOWN")
    try:
        workspace_id = int(target.get("workspace_id") or 0)
    except (ValueError, TypeError):
        workspace_id = 0
    try:
        target_id = int(target.get("id") or 0)
    except (ValueError, TypeError):
        target_id = 0

    proto = TargetResponse(
        id=target_id,
        updated_date=Timestamp(
            seconds=upd.get("seconds", int(time.time())),
            nanos=upd.get("nanos", 0),
        ),
        tracking_location=TrackingLocationDto(
            longitude=loc.get("longitude", 0),
            latitude=loc.get("latitude", 0),
            altitude=loc.get("altitude", 0),
        ),
        state=_STATE_TO_ENUM.get(state_str, TARGET_STATE_UNKNOWN),
        workspace_id=workspace_id,
    )
    return proto.SerializeToString()


def _target_response_to_dict(proto: TargetResponse) -> dict:
    """Convert a parsed TargetResponse proto into the server.py target dict shape."""
    loc = proto.tracking_location
    upd = proto.updated_date
    return {
        "id": proto.id,
        "updated_date": {
            "seconds": upd.seconds,
            "nanos":   upd.nanos,
        },
        "tracking_location": {
            "longitude": loc.longitude,
            "latitude":  loc.latitude,
            "altitude":  loc.altitude,
        },
        "state":        _ENUM_TO_STATE.get(proto.state, "TARGET_STATE_UNKNOWN"),
        "workspace_id": proto.workspace_id,
    }


def bytestring_to_target_dict(data: bytes) -> dict:
    """Deserialize a TargetResponse proto bytestring into a target dict (server.py schema)."""
    proto = TargetResponse()
    proto.ParseFromString(data)
    return _target_response_to_dict(proto)


def _delta_to_target_dict(d, base, batch_workspace_id: int) -> dict:
    """Reconstruct a full target dict from a TargetResponseDelta plus the
    list's base. Fields absent on the delta inherit from the base. Location
    components are summed (base + zigzag-decoded delta); id is base.id +
    sign-extended id_delta."""
    base_loc = base.tracking_location
    target_id = base.id + d.id_delta   # protobuf already zigzag-decoded sint32

    loc_long = base_loc.longitude
    loc_lat  = base_loc.latitude
    loc_alt  = base_loc.altitude
    if d.HasField("location_delta"):
        ld = d.location_delta
        loc_long += ld.longitudeDelta
        loc_lat  += ld.latitudeDelta
        loc_alt  += ld.altitudeDelta

    state_enum = d.state if d.HasField("state") else base.state
    upd        = d.updated_date if d.HasField("updated_date") else base.updated_date

    return {
        "id": target_id,
        "updated_date": {
            "seconds": upd.seconds,
            "nanos":   upd.nanos,
        },
        "tracking_location": {
            "longitude": loc_long,
            "latitude":  loc_lat,
            "altitude":  loc_alt,
        },
        "state":        _ENUM_TO_STATE.get(state_enum, "TARGET_STATE_UNKNOWN"),
        "workspace_id": batch_workspace_id or base.workspace_id,
    }


def bytestring_to_target_dicts(data: bytes) -> list[dict]:
    """Decode a TargetResponseDeltaList: base + repeated TargetResponseDelta.
    Returns [base_dict, *delta_dicts] with each delta reconstructed against
    the base. Falls back to a single TargetResponse for legacy senders.
    Returns [] on total failure so callers can distinguish via len()."""
    # Current format: TargetResponseDeltaList (base + deltas).
    # Note: cannot gate on `base_target.id != 0` since 0 is a valid packed
    # id under the new 16-bit format (class_id=0, object_id=0). Use
    # HasField for message presence and an empty-deltas/empty-base guard.
    try:
        batch = TargetResponseDeltaList()
        batch.ParseFromString(data)
        if batch.HasField("base_target"):
            base = batch.base_target
            # Guard against an entirely-default base (parsed from random bytes
            # that happened to look like a single empty submessage).
            if (base.id != 0 or base.HasField("tracking_location")
                    or base.HasField("updated_date") or base.state != 0
                    or len(batch.deltas) > 0):
                results = [_target_response_to_dict(base)]
                ws = batch.workspace_id
                for d in batch.deltas:
                    results.append(_delta_to_target_dict(d, base, ws))
                return results
    except Exception:
        pass
    # Legacy: single TargetResponse on the wire (pre-batch senders).
    try:
        single = TargetResponse()
        single.ParseFromString(data)
        if single.id != 0:
            return [_target_response_to_dict(single)]
    except Exception:
        pass
    return []


def base64_to_target_dict(b64: str) -> dict:
    """Decode a base64-encoded TargetResponse proto string into a target dict."""
    return bytestring_to_target_dict(base64.b64decode(b64))


def base64_to_target_dicts(b64: str) -> list[dict]:
    """Decode a base64-encoded TargetResponseList (or single TargetResponse,
    legacy fallback) into a list of target dicts."""
    return bytestring_to_target_dicts(base64.b64decode(b64))
