#!/usr/bin/env python3
"""Prefetch Nudm_SDM resources into the TOPSSIM SDM-CF cache."""

from __future__ import annotations

import argparse
import json
import subprocess
import time
from pathlib import Path
from typing import Iterable
from urllib.parse import quote, urlencode


DEFAULT_RESOURCES = (
    "am-data",
    "smf-select-data",
    "ue-context-in-smf-data",
    "sm-data",
)


def now_ms() -> int:
    return int(time.time() * 1000)


def sanitize(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "-_." else "_" for ch in value)


def cache_path(cache_dir: Path, supi: str, resource: str, plmn: str = "") -> Path:
    safe_supi = sanitize(supi)
    safe_resource = sanitize(resource)
    if plmn:
        return cache_dir / f"{safe_supi}__{safe_resource}__{sanitize(plmn)}.json"
    return cache_dir / f"{safe_supi}__{safe_resource}.json"


def audit(cache_dir: Path, event: str, payload: dict) -> None:
    record = {
        "event": event,
        "timestamp_ms": now_ms(),
        "payload": payload,
    }
    with (cache_dir / "audit.jsonl").open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, sort_keys=True))
        handle.write("\n")


def normalise_apiroot(h_udm: str) -> str:
    h_udm = h_udm.rstrip("/")
    if h_udm.endswith("/nudm-sdm/v2"):
        return h_udm
    return h_udm + "/nudm-sdm/v2"


def plmn_string(mcc: str, mnc: str) -> str:
    return f"{int(mcc):03d}{int(mnc):0{len(mnc)}d}"


def plmn_query_json(mcc: str, mnc: str) -> str:
    return json.dumps({"mcc": f"{int(mcc):03d}", "mnc": mnc}, separators=(",", ":"))


def build_url(
    apiroot: str,
    supi: str,
    resource: str,
    mcc: str,
    mnc: str,
    sm_data_dnn: str,
    sm_data_sst: int,
) -> str:
    params = {"plmn-id": plmn_query_json(mcc, mnc)}
    if resource == "sm-data":
        params["single-nssai"] = json.dumps(
            {"sst": sm_data_sst}, separators=(",", ":")
        )
        params["dnn"] = sm_data_dnn
    query = urlencode(params)
    return f"{apiroot}/{quote(supi, safe='')}/{resource}?{query}"


def run_curl(url: str, timeout: int) -> tuple[int, str, str, int]:
    start_ms = now_ms()
    proc = subprocess.run(
        [
            "curl",
            "--http2-prior-knowledge",
            "--fail-with-body",
            "-sS",
            url,
        ],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    return proc.returncode, proc.stdout, proc.stderr, now_ms() - start_ms


def write_cache_entry(
    cache_dir: Path,
    supi: str,
    resource: str,
    plmn: str,
    body: str,
    ttl_seconds: int,
) -> Path:
    cache_dir.mkdir(parents=True, exist_ok=True)
    path = cache_path(cache_dir, supi, resource, plmn)
    fallback_path = cache_path(cache_dir, supi, resource)
    expires_at_ms = now_ms() + ttl_seconds * 1000

    path.write_text(body, encoding="utf-8")
    path.with_suffix(path.suffix + ".expires").write_text(
        f"{expires_at_ms}\n", encoding="utf-8"
    )

    fallback_path.write_text(body, encoding="utf-8")
    fallback_path.with_suffix(fallback_path.suffix + ".expires").write_text(
        f"{expires_at_ms}\n", encoding="utf-8"
    )

    return path


def parse_resources(resources: str) -> Iterable[str]:
    for resource in resources.split(","):
        resource = resource.strip()
        if resource:
            yield resource


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prefetch H-UDM Nudm_SDM resources into the local SDM-CF cache"
    )
    parser.add_argument("--supi", required=True)
    parser.add_argument(
        "--h-udm",
        required=True,
        help="H-UDM origin or Nudm_SDM apiroot, for example http://udm.example:7777",
    )
    parser.add_argument("--vplmn-mcc", required=True)
    parser.add_argument("--vplmn-mnc", required=True)
    parser.add_argument(
        "--resources",
        default=",".join(DEFAULT_RESOURCES),
        help="Comma-separated Nudm_SDM resources to cache",
    )
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=Path("build/topssim/sdm-cache"),
    )
    parser.add_argument("--ttl-seconds", type=int, default=1800)
    parser.add_argument("--timeout", type=int, default=5)
    parser.add_argument("--sm-data-dnn", default="internet")
    parser.add_argument("--sm-data-sst", type=int, default=1)
    parser.add_argument(
        "--allow-empty-ue-context",
        action="store_true",
        help="Install {} for ue-context-in-smf-data if H-UDM does not return it",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    apiroot = normalise_apiroot(args.h_udm)
    plmn = plmn_string(args.vplmn_mcc, args.vplmn_mnc)
    args.cache_dir.mkdir(parents=True, exist_ok=True)

    audit(
        args.cache_dir,
        "preparation.sdm_prefetch.started",
        {
            "supi": args.supi,
            "h_udm": apiroot,
            "vplmn": plmn,
            "resources": list(parse_resources(args.resources)),
            "ttl_seconds": args.ttl_seconds,
        },
    )

    failures = 0
    for resource in parse_resources(args.resources):
        url = build_url(
            apiroot,
            args.supi,
            resource,
            args.vplmn_mcc,
            args.vplmn_mnc,
            args.sm_data_dnn,
            args.sm_data_sst,
        )
        returncode, stdout, stderr, elapsed_ms = run_curl(url, args.timeout)

        if returncode != 0 and resource == "ue-context-in-smf-data" and args.allow_empty_ue_context:
            stdout = "{}\n"
            returncode = 0

        if returncode != 0:
            failures += 1
            audit(
                args.cache_dir,
                "preparation.sdm_prefetch.failed",
                {
                    "supi": args.supi,
                    "resource": resource,
                    "url": url,
                    "elapsed_ms": elapsed_ms,
                    "returncode": returncode,
                    "stderr": stderr.strip(),
                },
            )
            print(f"FAIL {resource} {elapsed_ms}ms {stderr.strip()}")
            continue

        path = write_cache_entry(
            args.cache_dir, args.supi, resource, plmn, stdout, args.ttl_seconds
        )
        audit(
            args.cache_dir,
            "preparation.sdm_prefetch.installed",
            {
                "supi": args.supi,
                "resource": resource,
                "url": url,
                "elapsed_ms": elapsed_ms,
                "bytes": len(stdout.encode("utf-8")),
                "path": str(path),
            },
        )
        print(f"OK {resource} {elapsed_ms}ms {path}")

    audit(
        args.cache_dir,
        "preparation.sdm_prefetch.completed",
        {
            "supi": args.supi,
            "vplmn": plmn,
            "failures": failures,
        },
    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
