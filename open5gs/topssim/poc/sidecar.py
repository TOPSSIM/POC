#!/usr/bin/env python3
"""Minimal TOPSSIM proof-of-concept sidecar.

The sidecar intentionally uses only the Python standard library.  It provides a
manual Mobility Prediction Function trigger, turns valid triggers into bounded
preparation artifacts, and records an append-only audit stream.
"""

from __future__ import annotations

import argparse
import json
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, Tuple
from urllib.parse import parse_qs, urlparse


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 7777


def now_ms() -> int:
    return int(time.time() * 1000)


def canonical_key(record: Dict[str, Any]) -> str:
    return "|".join(
        [
            str(record.get("supi", "")),
            str(record.get("target_plmn", "")),
            str(record.get("dnn", "")),
            str(record.get("snssai", "")),
            str(record.get("roaming_mode", "")),
        ]
    )


class TopssimState:
    def __init__(self, state_dir: Path):
        self.state_dir = state_dir
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.state_file = self.state_dir / "state.json"
        self.audit_file = self.state_dir / "audit.jsonl"
        self.predictions: Dict[str, Dict[str, Any]] = {}
        self.artifacts: Dict[str, Dict[str, Any]] = {}
        self.load()

    def load(self) -> None:
        if not self.state_file.exists():
            return
        with self.state_file.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
        self.predictions = data.get("predictions", {})
        self.artifacts = data.get("artifacts", {})

    def save(self) -> None:
        payload = {
            "predictions": self.predictions,
            "artifacts": self.artifacts,
            "updated_at_ms": now_ms(),
        }
        tmp_file = self.state_file.with_suffix(".json.tmp")
        with tmp_file.open("w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
            handle.write("\n")
        tmp_file.replace(self.state_file)

    def audit(self, event: str, payload: Dict[str, Any]) -> None:
        entry = {
            "event": event,
            "timestamp_ms": now_ms(),
            "payload": payload,
        }
        with self.audit_file.open("a", encoding="utf-8") as handle:
            json.dump(entry, handle, sort_keys=True)
            handle.write("\n")

    def create_prediction(self, payload: Dict[str, Any]) -> Dict[str, Any]:
        required = ["supi", "target_plmn"]
        missing = [name for name in required if not payload.get(name)]
        if missing:
            raise ValueError("missing required field(s): " + ", ".join(missing))

        ttl_seconds = int(payload.get("ttl_seconds", 60))
        if ttl_seconds <= 0:
            raise ValueError("ttl_seconds must be positive")

        created_at_ms = now_ms()
        valid_until_ms = created_at_ms + ttl_seconds * 1000
        prediction_id = str(uuid.uuid4())
        prediction = {
            "id": prediction_id,
            "supi": payload["supi"],
            "target_plmn": payload["target_plmn"],
            "dnn": payload.get("dnn", ""),
            "snssai": payload.get("snssai", ""),
            "roaming_mode": payload.get("roaming_mode", "lbo"),
            "confidence": float(payload.get("confidence", 1.0)),
            "created_at_ms": created_at_ms,
            "valid_until_ms": valid_until_ms,
            "source": payload.get("source", "manual"),
        }
        prediction["key"] = canonical_key(prediction)
        self.predictions[prediction_id] = prediction
        self.audit("mpf.prediction.created", prediction)

        artifact_id = str(uuid.uuid4())
        artifact = {
            "id": artifact_id,
            "prediction_id": prediction_id,
            "type": "prepared_discovery",
            "key": prediction["key"],
            "supi": prediction["supi"],
            "target_plmn": prediction["target_plmn"],
            "dnn": prediction["dnn"],
            "snssai": prediction["snssai"],
            "roaming_mode": prediction["roaming_mode"],
            "valid_until_ms": valid_until_ms,
            "producer": "topssim-poc-rcf",
            "status": "prepared",
        }
        self.artifacts[artifact_id] = artifact
        self.audit("artifact.installed", artifact)
        self.save()

        return {"prediction": prediction, "artifact": artifact}

    def active_artifacts(self) -> Dict[str, Dict[str, Any]]:
        current_ms = now_ms()
        return {
            artifact_id: artifact
            for artifact_id, artifact in self.artifacts.items()
            if int(artifact.get("valid_until_ms", 0)) > current_ms
        }

    def discover(self, query: Dict[str, str]) -> Dict[str, Any]:
        lookup = {
            "supi": query.get("supi", ""),
            "target_plmn": query.get("target_plmn", ""),
            "dnn": query.get("dnn", ""),
            "snssai": query.get("snssai", ""),
            "roaming_mode": query.get("roaming_mode", "lbo"),
        }
        key = canonical_key(lookup)
        for artifact in self.active_artifacts().values():
            if artifact.get("key") == key:
                result = {"result": "hit", "artifact": artifact}
                self.audit("rcf.discovery.hit", {"key": key, "artifact_id": artifact["id"]})
                return result

        self.audit("rcf.discovery.miss", {"key": key})
        return {"result": "miss", "fallback": "normal-open5gs-nrf", "key": key}

    def audit_tail(self, limit: int = 100) -> list[Dict[str, Any]]:
        if not self.audit_file.exists():
            return []
        lines = self.audit_file.read_text(encoding="utf-8").splitlines()
        return [json.loads(line) for line in lines[-limit:]]


class TopssimHandler(BaseHTTPRequestHandler):
    server_version = "TOPSSIMPoC/0.1"

    def _state(self) -> TopssimState:
        return self.server.topssim_state  # type: ignore[attr-defined]

    def _read_json(self) -> Dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        if length == 0:
            return {}
        raw = self.rfile.read(length)
        return json.loads(raw.decode("utf-8"))

    def _write_json(self, code: int, payload: Dict[str, Any]) -> None:
        body = json.dumps(payload, indent=2, sort_keys=True).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/health":
            self._write_json(
                200,
                {
                    "status": "ok",
                    "component": "topssim-poc-sidecar",
                    "timestamp_ms": now_ms(),
                },
            )
            return

        if parsed.path == "/artifacts":
            self._write_json(200, {"artifacts": self._state().active_artifacts()})
            return

        if parsed.path == "/audit":
            query = parse_qs(parsed.query)
            limit = int(query.get("limit", ["100"])[0])
            self._write_json(200, {"audit": self._state().audit_tail(limit)})
            return

        if parsed.path == "/rcf/discover":
            query = {key: values[0] for key, values in parse_qs(parsed.query).items()}
            self._write_json(200, self._state().discover(query))
            return

        self._write_json(404, {"error": "not found"})

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path != "/mpf/trigger":
            self._write_json(404, {"error": "not found"})
            return

        try:
            result = self._state().create_prediction(self._read_json())
        except (ValueError, json.JSONDecodeError) as exc:
            self._write_json(400, {"error": str(exc)})
            return

        self._write_json(201, result)

    def log_message(self, fmt: str, *args: Tuple[Any, ...]) -> None:
        print("[TOPSSIM][HTTP] " + fmt % args, flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the TOPSSIM PoC sidecar")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--state-dir", type=Path, default=Path("build/topssim"))
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    state = TopssimState(args.state_dir)
    server = ThreadingHTTPServer((args.host, args.port), TopssimHandler)
    server.topssim_state = state  # type: ignore[attr-defined]
    print(
        "[TOPSSIM] sidecar listening on http://%s:%d state_dir=%s"
        % (args.host, args.port, args.state_dir),
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        print("[TOPSSIM] sidecar stopping", flush=True)
        server.server_close()


if __name__ == "__main__":
    main()
