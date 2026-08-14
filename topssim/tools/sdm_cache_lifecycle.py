#!/usr/bin/env python3
"""Maintain TOPSSIM SDM-CF cache artifacts after setup."""

from __future__ import annotations

import argparse
import json
import subprocess
import time
from pathlib import Path
from urllib.parse import quote


DEFAULT_RESOURCES = "am-data,smf-select-data,ue-context-in-smf-data,sm-data"
DEFAULT_NF_INSTANCE_ID = "00000000-0000-4000-8000-000000000001"


def now_ms() -> int:
    return int(time.time() * 1000)


def sanitize(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "-_." else "_" for ch in value)


def audit(cache_dir: Path, event: str, payload: dict) -> None:
    cache_dir.mkdir(parents=True, exist_ok=True)
    record = {
        "event": event,
        "timestamp_ms": now_ms(),
        "payload": payload,
    }
    with (cache_dir / "audit.jsonl").open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, sort_keys=True))
        handle.write("\n")


def matching_paths(cache_dir: Path, supi: str, resources: list[str]) -> list[Path]:
    safe_supi = sanitize(supi)
    wanted = {sanitize(resource) for resource in resources}
    paths = []

    if not cache_dir.exists():
        return paths

    for path in cache_dir.glob(f"{safe_supi}__*.json"):
        parts = path.stem.split("__")
        if len(parts) < 2:
            continue
        if parts[1] in wanted:
            paths.append(path)

    return sorted(paths)


def refresh(cache_dir: Path, supi: str, resources: list[str], ttl_seconds: int) -> int:
    paths = matching_paths(cache_dir, supi, resources)
    expires_at_ms = now_ms() + ttl_seconds * 1000
    for path in paths:
        path.with_suffix(path.suffix + ".expires").write_text(
            f"{expires_at_ms}\n", encoding="utf-8"
        )
    audit(
        cache_dir,
        "post_setup.sdm_cache.refresh",
        {
            "supi": supi,
            "resources": resources,
            "ttl_seconds": ttl_seconds,
            "artifacts": len(paths),
        },
    )
    return len(paths)


def delete(cache_dir: Path, supi: str, resources: list[str]) -> int:
    paths = matching_paths(cache_dir, supi, resources)
    removed = 0
    for path in paths:
        expires = path.with_suffix(path.suffix + ".expires")
        path.unlink(missing_ok=True)
        expires.unlink(missing_ok=True)
        removed += 1
    audit(
        cache_dir,
        "post_setup.sdm_cache.delete",
        {
            "supi": supi,
            "resources": resources,
            "artifacts": removed,
        },
    )
    return removed


def normalise_apiroot(h_udm: str) -> str:
    h_udm = h_udm.rstrip("/")
    if h_udm.endswith("/nudm-sdm/v2"):
        return h_udm
    return h_udm + "/nudm-sdm/v2"


def subscribe(
    cache_dir: Path,
    supi: str,
    resources: list[str],
    h_udm: str,
    callback_reference: str,
    nf_instance_id: str,
    timeout: int,
) -> int:
    apiroot = normalise_apiroot(h_udm)
    created = 0

    for resource in resources:
        start_ms = now_ms()
        url = f"{apiroot}/{quote(supi, safe='')}/sdm-subscriptions"
        body = {
            "nfInstanceId": nf_instance_id,
            "implicitUnsubscribe": True,
            "callbackReference": callback_reference,
            "monitoredResourceUris": [f"{supi}/{resource}"],
            "supportedFeatures": "1000",
            "uniqueSubscription": True,
        }
        proc = subprocess.run(
            [
                "curl",
                "--http2-prior-knowledge",
                "--fail-with-body",
                "-sS",
                "-X",
                "POST",
                "-H",
                "Content-Type: application/json",
                "-d",
                json.dumps(body, separators=(",", ":")),
                url,
            ],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
        elapsed_ms = now_ms() - start_ms
        if proc.returncode == 0:
            created += 1
            audit(
                cache_dir,
                "post_setup.sdm_subscription.created",
                {
                    "supi": supi,
                    "resource": resource,
                    "url": url,
                    "elapsed_ms": elapsed_ms,
                    "bytes": len(proc.stdout.encode("utf-8")),
                },
            )
            print(f"subscribe {resource} elapsed_ms={elapsed_ms}")
        else:
            audit(
                cache_dir,
                "post_setup.sdm_subscription.failed",
                {
                    "supi": supi,
                    "resource": resource,
                    "url": url,
                    "elapsed_ms": elapsed_ms,
                    "returncode": proc.returncode,
                    "stderr": proc.stderr.strip(),
                },
            )
            print(f"subscribe-failed {resource} elapsed_ms={elapsed_ms} {proc.stderr.strip()}")

    return created


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Maintain TOPSSIM SDM-CF cache entries")
    parser.add_argument("action", choices=["refresh", "delete", "subscribe"])
    parser.add_argument("--supi", required=True)
    parser.add_argument("--resources", default=DEFAULT_RESOURCES)
    parser.add_argument("--cache-dir", type=Path, default=Path("build/topssim/sdm-cache"))
    parser.add_argument("--ttl-seconds", type=int, default=1800)
    parser.add_argument("--h-udm")
    parser.add_argument(
        "--callback-reference",
        default="http://sdm-cf.localdomain/topssim/sdm-subscription-notify",
    )
    parser.add_argument("--nf-instance-id", default=DEFAULT_NF_INSTANCE_ID)
    parser.add_argument("--timeout", type=int, default=5)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    resources = [item.strip() for item in args.resources.split(",") if item.strip()]
    start_ms = now_ms()
    if args.action == "refresh":
        count = refresh(args.cache_dir, args.supi, resources, args.ttl_seconds)
    elif args.action == "delete":
        count = delete(args.cache_dir, args.supi, resources)
    else:
        if not args.h_udm:
            raise SystemExit("--h-udm is required for subscribe")
        count = subscribe(
            args.cache_dir,
            args.supi,
            resources,
            args.h_udm,
            args.callback_reference,
            args.nf_instance_id,
            args.timeout,
        )
    elapsed_ms = now_ms() - start_ms
    audit(
        args.cache_dir,
        "post_setup.sdm_cache.action_completed",
        {
            "action": args.action,
            "supi": args.supi,
            "resources": resources,
            "artifacts": count,
            "elapsed_ms": elapsed_ms,
        },
    )
    print(f"{args.action} artifacts={count} elapsed_ms={elapsed_ms}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
