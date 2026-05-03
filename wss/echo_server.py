"""
Tiny stdlib-only echo server for smoke-testing target-manager → Beam outbound.

  python3 echo_server.py            # listens on :9099, accepts any POST path

Logs each POST: path, byte count, parsed JSON (if any), and decoded
TargetResponse if --content arg is base64 of a serialized proto.
"""
import base64
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, ".")
try:
    from target_proto_pb2 import TargetResponse
    HAVE_PROTO = True
except Exception as exc:
    print(f"[echo] warning: target_proto_pb2 not importable ({exc}); "
          f"will not decode proto payloads", file=sys.stderr)
    HAVE_PROTO = False


class EchoHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n) if n > 0 else b""

        print(f"\n=== POST {self.path}  ({len(body)} bytes) ===", flush=True)

        try:
            j = json.loads(body)
        except Exception:
            print(f"  raw body: {body[:500]!r}", flush=True)
        else:
            print(f"  json: {json.dumps(j, indent=2)}", flush=True)

            args = j.get("args") or []
            if HAVE_PROTO and isinstance(args, list) and "--content" in args:
                try:
                    b64 = args[args.index("--content") + 1]
                    raw = base64.b64decode(b64)
                    tr = TargetResponse()
                    tr.ParseFromString(raw)
                    print(f"  decoded TargetResponse:\n{tr}", flush=True)
                except Exception as exc:
                    print(f"  proto decode failed: {exc}", flush=True)

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"status":"ok"}')

    def log_message(self, format, *args):
        # Quiet the default access-log noise; we have our own per-POST log.
        pass


def main():
    server = ThreadingHTTPServer(("0.0.0.0", 9099), EchoHandler)
    print("[echo] listening on http://localhost:9099 (any path, POST)", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
