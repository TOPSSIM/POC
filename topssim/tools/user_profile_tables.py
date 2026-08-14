#!/usr/bin/env python3
"""Generate TOPSSIM user-profile measurement tables from run logs."""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from statistics import mean, stdev
from typing import Iterable


SEPP_RE = re.compile(
    r"(?P<ts>\d\d/\d\d \d\d:\d\d:\d\d\.\d{3}).*"
    r"\[SEPP\]\[(?P<kind>REQ|RSP)\]\[RID:(?P<rid>\d+)\] (?P<flow>[^\n]+)\n"
    r"Service: (?P<service>[^\n]+)"
)

SDMCF_RE = re.compile(
    r"(?P<ts>\d\d/\d\d \d\d:\d\d:\d\d\.\d{3}).*"
    r"\[TOPSSIM\]\[(?:(?:AMF|SMF)-)?SDM-CF\]\[(?P<kind>HIT|MISS|SOURCE)\].*"
    r"resource\[(?P<resource>[^\]]+)\](?P<rest>[^\n]*)"
)

SBI_TIMING_RE = re.compile(
    r"(?P<ts>\d\d/\d\d \d\d:\d\d:\d\d\.\d{3}).*"
    r"\[TOPSSIM\]\[SBI-TIMING\]\[RSP\] "
    r"XID\[(?P<xid>[^\]]+)\] NF\[(?P<nf>[^\]]+)\] "
    r"service\[(?P<service>[^\]]+)\] method\[(?P<method>[^\]]+)\] "
    r"resource\[(?P<resource>[^\]]+)\] status\[(?P<status>-?\d+)\] "
    r"http_status\[(?P<http_status>\d+)\] elapsed_ms\[(?P<elapsed>[0-9.]+)\] "
    r"uri\[(?P<uri>[^\]]+)\]"
)

ELAPSED_RE = re.compile(r"elapsed_ms\[(?P<elapsed>[0-9.]+)\]")


RUNTIME_COMPONENTS = [
    (
        "Subscriber database discovery",
        "Nnrf_DISC_GET",
        None,
        lambda entry: "nudm-sdm" in entry.query,
    ),
    (
        "Access and mobility subscription data",
        "Nudm_SDM_GET",
        "am-data",
        lambda entry: "/am-data" in entry.query,
    ),
    (
        "SMF selection subscription data",
        "Nudm_SDM_GET",
        "smf-select-data",
        lambda entry: "/smf-select-data" in entry.query,
    ),
    (
        "UE context in SMF data",
        "Nudm_SDM_GET",
        "ue-context-in-smf-data",
        lambda entry: "/ue-context-in-smf-data" in entry.query,
    ),
    (
        "Session management subscription data",
        "Nudm_SDM_GET",
        "sm-data",
        lambda entry: "/sm-data" in entry.query,
    ),
    (
        "SDM subscription setup am-data",
        "Nudm_SDM_POST",
        None,
        lambda entry: "am-data" in entry.query,
    ),
    (
        "SDM subscription setup smf-select-data",
        "Nudm_SDM_POST",
        None,
        lambda entry: "smf-select-data" in entry.query,
    ),
    (
        "SDM subscription setup sm-data",
        "Nudm_SDM_POST",
        None,
        lambda entry: "sm-data" in entry.query,
    ),
]


@dataclass
class SeppEntry:
    rid: str
    req_ts: datetime | None = None
    rsp_ts: datetime | None = None
    flow: str = ""
    service: str = ""
    query: str = ""

    @property
    def duration_ms(self) -> float | None:
        if not self.req_ts or not self.rsp_ts:
            return None
        return (self.rsp_ts - self.req_ts).total_seconds() * 1000


@dataclass
class SbiTimingEntry:
    ts: datetime
    xid: str
    nf: str
    service: str
    method: str
    resource: str
    status: int
    http_status: int
    elapsed_ms: float
    uri: str


def parse_ts(value: str, year: int) -> datetime:
    return datetime.strptime(f"{year}/{value}", "%Y/%m/%d %H:%M:%S.%f")


def extract_block(text: str, start: int, next_start: int | None) -> str:
    end = next_start if next_start is not None else len(text)
    return text[start:end]


def parse_sepp_log(path: Path, year: int) -> list[SeppEntry]:
    text = path.read_text(encoding="utf-8", errors="replace")
    matches = list(SEPP_RE.finditer(text))
    by_rid: dict[str, SeppEntry] = {}

    for index, match in enumerate(matches):
        next_start = matches[index + 1].start() if index + 1 < len(matches) else None
        block = extract_block(text, match.start(), next_start)
        rid = match.group("rid")
        entry = by_rid.setdefault(rid, SeppEntry(rid=rid))
        entry.flow = match.group("flow")
        entry.service = match.group("service")
        if match.group("kind") == "REQ":
            entry.req_ts = parse_ts(match.group("ts"), year)
            entry.query = block
        else:
            entry.rsp_ts = parse_ts(match.group("ts"), year)

    return list(by_rid.values())


def values_for_component(entries: Iterable[SeppEntry], service: str, predicate) -> list[float]:
    values = []
    for entry in entries:
        if entry.service != service:
            continue
        if not predicate(entry):
            continue
        if entry.duration_ms is not None:
            values.append(entry.duration_ms)
    return values


def parse_sbi_timing_log(path: Path, year: int) -> list[SbiTimingEntry]:
    text = path.read_text(encoding="utf-8", errors="replace")
    entries = []

    for match in SBI_TIMING_RE.finditer(text):
        entries.append(
            SbiTimingEntry(
                ts=parse_ts(match.group("ts"), year),
                xid=match.group("xid"),
                nf=match.group("nf"),
                service=match.group("service"),
                method=match.group("method"),
                resource=match.group("resource"),
                status=int(match.group("status")),
                http_status=int(match.group("http_status")),
                elapsed_ms=float(match.group("elapsed")),
                uri=match.group("uri"),
            )
        )

    return entries


def values_for_sbi_timing(
    entries: Iterable[SbiTimingEntry], service: str, predicate
) -> list[float]:
    values = []
    for entry in entries:
        if entry.service != service:
            continue
        if entry.status != 0:
            continue
        if entry.http_status < 200 or entry.http_status >= 300:
            continue
        if not predicate(entry):
            continue
        values.append(entry.elapsed_ms)
    return values


def fmt_mean_sd(values: list[float]) -> str:
    if not values:
        return "Removed"
    if len(values) == 1:
        return f"{values[0]:.3f} ± 0.000"
    return f"{mean(values):.3f} ± {stdev(values):.3f}"


def parse_audit(path: Path | None) -> list[dict]:
    if not path or not path.exists():
        return []
    events = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return events


def parse_sdmcf_metrics(
    paths: list[Path],
) -> tuple[Counter, Counter, defaultdict[str, list[float]], defaultdict[str, list[float]]]:
    hits: Counter = Counter()
    misses: Counter = Counter()
    hit_values: defaultdict[str, list[float]] = defaultdict(list)
    miss_values: defaultdict[str, list[float]] = defaultdict(list)

    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in SDMCF_RE.finditer(text):
            resource = match.group("resource")
            if match.group("kind") == "SOURCE":
                continue
            elapsed_match = ELAPSED_RE.search(match.group("rest"))
            elapsed = float(elapsed_match.group("elapsed")) if elapsed_match else None
            if match.group("kind") == "HIT":
                hits[resource] += 1
                if elapsed is not None:
                    hit_values[resource].append(elapsed)
            else:
                misses[resource] += 1
                if elapsed is not None:
                    miss_values[resource].append(elapsed)

    return hits, misses, hit_values, miss_values


def parse_sdmcf_source_metrics(paths: list[Path]) -> defaultdict[str, list[float]]:
    values: defaultdict[str, list[float]] = defaultdict(list)

    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in SDMCF_RE.finditer(text):
            if match.group("kind") != "SOURCE":
                continue
            elapsed_match = ELAPSED_RE.search(match.group("rest"))
            if elapsed_match:
                values[match.group("resource")].append(
                    float(elapsed_match.group("elapsed"))
                )

    return values


def markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    lines = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(row) + " |")
    return "\n".join(lines)


def preparation_table(audit_events: list[dict]) -> str:
    rows = []
    by_resource: defaultdict[str, list[float]] = defaultdict(list)
    failures: Counter = Counter()

    for event in audit_events:
        name = event.get("event")
        payload = event.get("payload", {})
        resource = payload.get("resource")
        if name == "preparation.sdm_prefetch.installed" and resource:
            by_resource[resource].append(float(payload.get("elapsed_ms", 0)))
        if name == "preparation.sdm_prefetch.failed" and resource:
            failures[resource] += 1

    for resource in sorted(set(by_resource) | set(failures)):
        rows.append(
            [
                f"Nudm_SDM_Get {resource}",
                fmt_mean_sd(by_resource[resource]) if by_resource[resource] else "Failed",
                str(len(by_resource[resource])),
                str(failures[resource]),
            ]
        )

    if not rows:
        rows.append(["No preparation audit events found", "-", "0", "0"])

    return markdown_table(
        ["Preparation component", "TOPSSIM Mean ± SD (ms)", "Installed", "Failures"],
        rows,
    )


def runtime_table(
    standard_entries: list[SeppEntry],
    topssim_entries: list[SeppEntry],
    local_sdm_values: defaultdict[str, list[float]],
) -> str:
    rows = []
    standard_total = 0.0
    topssim_total = 0.0

    for label, service, local_resource, predicate in RUNTIME_COMPONENTS:
        standard_values = values_for_component(standard_entries, service, predicate)
        topssim_values = values_for_component(topssim_entries, service, predicate)
        if not topssim_values and local_resource:
            topssim_values = local_sdm_values[local_resource]

        rows.append([label, fmt_mean_sd(standard_values), fmt_mean_sd(topssim_values)])
        standard_total += sum(standard_values)
        topssim_total += sum(topssim_values)

    rows.append(["Total measured user-profile path", f"{standard_total:.3f}", f"{topssim_total:.3f}"])

    return markdown_table(
        ["Runtime critical-path component", "Standard 5G Mean ± SD (ms)", "TOPSSIM Mean ± SD (ms)"],
        rows,
    )


def post_setup_table(
    topssim_entries: list[SeppEntry],
    hit_logs: list[Path],
    audit_events: list[dict],
) -> str:
    rows = []
    hits, misses, hit_values, miss_values = parse_sdmcf_metrics(hit_logs)

    for resource in sorted(set(hits) | set(misses)):
        value = fmt_mean_sd(hit_values[resource]) if hit_values[resource] else "No hits"
        rows.append(
            [
                f"SDM-CF local lookup {resource}",
                value,
                f"hits={hits[resource]} misses={misses[resource]}",
            ]
        )
        if miss_values[resource]:
            rows.append(
                [
                    f"SDM-CF cache miss {resource}",
                    fmt_mean_sd(miss_values[resource]),
                    str(len(miss_values[resource])),
                ]
            )

    for label, service in [
        ("SDM subscription creation", "Nudm_SDM_POST"),
        ("SDM subscription deletion", "Nudm_SDM_DELETE"),
    ]:
        values = [entry.duration_ms for entry in topssim_entries
                  if entry.service == service and entry.duration_ms is not None]
        rows.append([label, fmt_mean_sd([float(v) for v in values]), str(len(values))])

    lifecycle_values: defaultdict[str, list[float]] = defaultdict(list)
    lifecycle_artifacts: Counter = Counter()
    for event in audit_events:
        if event.get("event") != "post_setup.sdm_cache.action_completed":
            continue
        payload = event.get("payload", {})
        action = payload.get("action")
        if not action:
            continue
        lifecycle_values[action].append(float(payload.get("elapsed_ms", 0)))
        lifecycle_artifacts[action] += int(payload.get("artifacts", 0))

    for action in sorted(lifecycle_values):
        rows.append(
            [
                f"SDM-CF cache {action}",
                fmt_mean_sd(lifecycle_values[action]),
                f"artifacts={lifecycle_artifacts[action]}",
            ]
        )

    if not rows:
        rows.append(["No post-setup events found", "-", "0"])

    return markdown_table(["Post-setup component", "TOPSSIM value", "Count"], rows)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate preparation, runtime, and post-setup user-profile tables"
    )
    parser.add_argument("--standard-sepp-log", type=Path, required=True)
    parser.add_argument("--topssim-sepp-log", type=Path, required=True)
    parser.add_argument("--topssim-cache-audit", type=Path)
    parser.add_argument("--topssim-cache-dir", type=Path)
    parser.add_argument("--topssim-runtime-log", type=Path, action="append", default=[])
    parser.add_argument("--year", type=int, default=datetime.now().year)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.topssim_cache_audit and args.topssim_cache_dir:
        args.topssim_cache_audit = args.topssim_cache_dir / "audit.jsonl"

    standard_entries = parse_sepp_log(args.standard_sepp_log, args.year)
    topssim_entries = parse_sepp_log(args.topssim_sepp_log, args.year)
    audit_events = parse_audit(args.topssim_cache_audit)
    _, _, sdmcf_hit_values, _ = parse_sdmcf_metrics(args.topssim_runtime_log)

    print("## Preparation Phase")
    print(preparation_table(audit_events))
    print()
    print("## Runtime Critical Path")
    print(runtime_table(standard_entries, topssim_entries, sdmcf_hit_values))
    print()
    print("## Post-Setup Phase")
    print(post_setup_table(topssim_entries, args.topssim_runtime_log, audit_events))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
