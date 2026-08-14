#!/usr/bin/env python3
"""Send a manual Mobility Prediction Function trigger to the TOPSSIM sidecar."""

from __future__ import annotations

import argparse
import json
from typing import Any, Dict
from urllib import request


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Trigger TOPSSIM MPF prediction")
    parser.add_argument("--url", default="http://127.0.0.1:7777/mpf/trigger")
    parser.add_argument("--supi", required=True)
    parser.add_argument("--target-plmn", required=True)
    parser.add_argument("--dnn", default="")
    parser.add_argument("--snssai", default="")
    parser.add_argument("--roaming-mode", default="lbo")
    parser.add_argument("--ttl-seconds", type=int, default=60)
    parser.add_argument("--confidence", type=float, default=1.0)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    payload: Dict[str, Any] = {
        "supi": args.supi,
        "target_plmn": args.target_plmn,
        "dnn": args.dnn,
        "snssai": args.snssai,
        "roaming_mode": args.roaming_mode,
        "ttl_seconds": args.ttl_seconds,
        "confidence": args.confidence,
        "source": "manual-cli",
    }
    body = json.dumps(payload).encode("utf-8")
    http_request = request.Request(
        args.url,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with request.urlopen(http_request, timeout=5) as response:
        print(response.read().decode("utf-8"))


if __name__ == "__main__":
    main()
