#!/usr/bin/env python3
"""Generate multi-run TOPSSIM user-profile campaign reports."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from datetime import datetime
from html import escape
from pathlib import Path
from statistics import mean, stdev

from user_profile_tables import (
    RUNTIME_COMPONENTS,
    fmt_mean_sd,
    markdown_table,
    parse_audit,
    parse_sdmcf_metrics,
    parse_sdmcf_source_metrics,
    parse_sbi_timing_log,
    parse_sepp_log,
    values_for_component,
    values_for_sbi_timing,
)


TOPSSIM_REMOVED_RUNTIME_COMPONENTS = {
    "Subscriber database discovery",
    "SDM subscription setup am-data",
    "SDM subscription setup smf-select-data",
    "SDM subscription setup sm-data",
}

TOPSSIM_RUNTIME_LABELS = {
    "Access and mobility subscription data": "SDM-CF local lookup am-data",
    "SMF selection subscription data": "SDM-CF local lookup smf-select-data",
    "UE context in SMF data": "SDM-CF local lookup ue-context-in-smf-data",
}


def expand_paths(paths: list[Path] | None, patterns: list[str] | None) -> list[Path]:
    expanded: list[Path] = []
    if paths:
        expanded.extend(paths)
    if patterns:
        for pattern in patterns:
            expanded.extend(sorted(Path().glob(pattern)))
    return expanded


def sample_variance(values: list[float]) -> float:
    if len(values) < 2:
        return 0.0
    avg = mean(values)
    return sum((value - avg) ** 2 for value in values) / (len(values) - 1)


def stats_cells(values: list[float]) -> list[str]:
    if not values:
        return ["0", "Removed", "Removed", "Removed"]
    sd = stdev(values) if len(values) > 1 else 0.0
    return [
        str(len(values)),
        f"{mean(values):.3f}",
        f"{sample_variance(values):.3f}",
        f"{sd:.3f}",
    ]


def fmt_mean_sd_with_coverage(values: list[float], expected_runs: int) -> str:
    value = fmt_mean_sd(values)
    if values and len(values) != expected_runs:
        return f"{value} (n={len(values)}/{expected_runs})"
    return value


def total_delta_cells(standard_totals: list[float], topssim_totals: list[float]) -> list[str]:
    if not standard_totals or not topssim_totals:
        return ["-", "-"]

    standard_mean = mean(standard_totals)
    topssim_mean = mean(topssim_totals)
    delta = standard_mean - topssim_mean
    if standard_mean <= 0:
        reduction = "-"
    else:
        reduction = f"{(delta / standard_mean) * 100:.1f}%"

    return [f"{delta:.3f}", reduction]


def runtime_values_by_component(
    sepp_logs: list[Path],
    year: int,
    runtime_logs: list[Path] | None = None,
) -> tuple[dict[str, list[float]], list[float]]:
    by_component: dict[str, list[float]] = defaultdict(list)
    totals: list[float] = []

    if runtime_logs and len(runtime_logs) != len(sepp_logs):
        raise SystemExit(
            "When using per-run runtime logs, provide the same number as SEPP logs"
        )

    for index, sepp_log in enumerate(sepp_logs):
        entries = parse_sepp_log(sepp_log, year)
        local_values = defaultdict(list)
        if runtime_logs:
            _, _, local_values, _ = parse_sdmcf_metrics([runtime_logs[index]])

        run_total = 0.0
        run_has_value = False
        for label, service, local_resource, predicate in RUNTIME_COMPONENTS:
            values = values_for_component(entries, service, predicate)
            if not values and local_resource:
                values = local_values[local_resource]
            if values:
                run_value = sum(values)
                by_component[label].append(run_value)
                run_total += run_value
                run_has_value = True

        if run_has_value:
            totals.append(run_total)

    return by_component, totals


def local_runtime_values_by_component(
    logs: list[Path], year: int
) -> dict[str, list[float]]:
    by_component: dict[str, list[float]] = defaultdict(list)

    component_specs = [
        (
            "NSSSelection_Get",
            "nnssf-nsselection",
            lambda entry: entry.method == "GET",
        ),
        (
            "AMPolicyControl_Create",
            "npcf-am-policy-control",
            lambda entry: entry.method == "POST" and entry.resource == "policies",
        ),
        (
            "PDUSession_CreateSMContext",
            "nsmf-pdusession",
            lambda entry: entry.method == "POST" and entry.resource == "sm-contexts",
        ),
        (
            "SMPolicyControl_Create",
            "npcf-smpolicycontrol",
            lambda entry: entry.method == "POST" and entry.resource == "sm-policies",
        ),
    ]

    for log in logs:
        entries = parse_sbi_timing_log(log, year)
        for label, service, predicate in component_specs:
            values = values_for_sbi_timing(entries, service, predicate)
            if values:
                by_component[label].append(sum(values))

    return by_component


def totals_from_components(
    component_values: list[list[float]], expected_runs: int
) -> list[float]:
    totals = []

    for run_index in range(expected_runs):
        total = 0.0
        has_value = False
        for values in component_values:
            if run_index < len(values):
                total += values[run_index]
                has_value = True
        if has_value:
            totals.append(total)

    return totals


def estimated_standard_smf_select_subscribe_values(
    standard_values: dict[str, list[float]],
    expected_runs: int,
) -> tuple[list[float], str]:
    measured = standard_values["SDM subscription setup smf-select-data"]
    if measured:
        return measured, "Measured"

    sources = [
        (
            "SDM_Subscribe(am-data)",
            standard_values["SDM subscription setup am-data"],
        ),
        (
            "SDM_Subscribe(sm-data)",
            standard_values["SDM subscription setup sm-data"],
        ),
    ]
    active_sources = [(label, values) for label, values in sources if values]
    estimates: list[float] = []

    for run_index in range(expected_runs):
        run_values = [
            values[run_index]
            for _label, values in active_sources
            if run_index < len(values)
        ]
        if run_values:
            estimates.append(mean(run_values))

    if not estimates:
        return [], "Not observed in current Open5GS logs"

    source_labels = " and ".join(label for label, _values in active_sources)
    return estimates, f"Estimated from {source_labels}"


def preparation_values(
    audit_logs: list[Path],
) -> tuple[dict[str, list[float]], dict[str, list[float]], Counter]:
    by_component: dict[str, list[float]] = defaultdict(list)
    by_resource: dict[str, list[float]] = defaultdict(list)
    failures: Counter = Counter()

    for audit_log in audit_logs:
        for event in parse_audit(audit_log):
            name = event.get("event")
            payload = event.get("payload", {})
            resource = payload.get("resource")
            component = payload.get("component")
            if name in {
                "preparation.scope_determination.completed",
                "preparation.pre_discovery.completed",
            } and component:
                by_component[component].append(float(payload.get("elapsed_ms", 0)))
            if name == "preparation.sdm_prefetch.installed" and resource:
                by_resource[resource].append(float(payload.get("elapsed_ms", 0)))
            if name == "preparation.sdm_prefetch.failed" and resource:
                failures[resource] += 1

    return by_component, by_resource, failures


def pre_discovery_values(topssim_logs: list[Path], year: int) -> list[float]:
    values: list[float] = []

    for log in topssim_logs:
        entries = parse_sepp_log(log, year)
        matches = values_for_component(
            entries,
            "Nnrf_DISC_GET",
            lambda entry: "target-nf-type=UDM" in entry.query
            and "nudm-sdm" in entry.query,
        )
        if matches:
            values.append(matches[0])

    return values


def post_setup_values(
    topssim_logs: list[Path],
    audit_logs: list[Path],
    year: int,
) -> dict[str, list[float]]:
    by_component: dict[str, list[float]] = defaultdict(list)

    for audit_log in audit_logs:
        for event in parse_audit(audit_log):
            name = event.get("event")
            payload = event.get("payload", {})

            if name == "post_setup.sdm_subscription.created":
                resource = payload.get("resource", "unknown")
                by_component[f"SDM subscription creation {resource}"].append(
                    float(payload.get("elapsed_ms", 0))
                )
                continue
            if name == "post_setup.sdm_subscription.failed":
                resource = payload.get("resource", "unknown")
                by_component[f"SDM subscription creation {resource}"].extend([])
                continue

            if event.get("event") != "post_setup.sdm_cache.action_completed":
                continue
            action = payload.get("action")
            if action == "refresh":
                by_component[f"SDM-CF cache {action}"].append(
                    float(payload.get("elapsed_ms", 0))
                )

    return by_component


def preparation_table(
    audit_logs: list[Path],
    topssim_logs: list[Path],
    year: int,
) -> str:
    headers, rows = preparation_rows(audit_logs, topssim_logs, year)
    return markdown_table(headers, rows)


def preparation_rows(
    audit_logs: list[Path],
    topssim_logs: list[Path],
    year: int,
) -> tuple[list[str], list[list[str]]]:
    component_values, values, failures = preparation_values(audit_logs)
    pre_discovery = component_values["Subscriber database pre-discovery"]
    if not pre_discovery:
        pre_discovery = pre_discovery_values(topssim_logs, year)

    rows = [
        [
            "Preparation-scope determination",
            *stats_cells(component_values["Preparation-scope determination"]),
            "0",
        ],
        [
            "Subscriber database pre-discovery",
            *stats_cells(pre_discovery),
            "0",
        ],
    ]

    for resource in sorted(set(values) | set(failures)):
        rows.append(
            [
                f"Nudm_SDM_Get {resource}",
                *stats_cells(values[resource]),
                str(failures[resource]),
            ]
        )

    return ["Preparation component", "n", "Mean (ms)", "Variance", "SD", "Failures"], rows


def runtime_table_compact(
    standard_logs: list[Path],
    topssim_logs: list[Path],
    year: int,
) -> str:
    comparison, standard, topssim = runtime_tables_data(standard_logs, topssim_logs, year)
    return "\n\n".join(
        [
            markdown_table(*comparison),
            markdown_table(*standard),
            markdown_table(*topssim),
        ]
    )


def runtime_tables_data(
    standard_logs: list[Path],
    topssim_logs: list[Path],
    year: int,
) -> tuple[tuple[list[str], list[list[str]]], tuple[list[str], list[list[str]]], tuple[list[str], list[list[str]]]]:
    standard_values, standard_totals = runtime_values_by_component(standard_logs, year)
    topssim_values, topssim_totals = runtime_values_by_component(
        topssim_logs, year, topssim_logs
    )
    standard_local_values = local_runtime_values_by_component(standard_logs, year)
    topssim_local_values = local_runtime_values_by_component(topssim_logs, year)
    topssim_source_values = parse_sdmcf_source_metrics(topssim_logs)
    standard_totals = standard_runtime_totals(
        standard_values, standard_local_values, len(standard_logs)
    )
    topssim_active_totals = topssim_runtime_totals(
        topssim_values, topssim_local_values, topssim_source_values, len(topssim_logs)
    )
    delta, reduction = total_delta_cells(standard_totals, topssim_active_totals)

    return (
        runtime_total_comparison_rows(standard_totals, topssim_active_totals, delta, reduction),
        standard_runtime_rows(
            standard_values, standard_local_values, standard_totals, len(standard_logs)
        ),
        topssim_runtime_rows(
            topssim_values,
            topssim_local_values,
            topssim_source_values,
            topssim_active_totals,
            len(topssim_logs),
        ),
    )


def standard_runtime_totals(
    standard_values: dict[str, list[float]],
    standard_local_values: dict[str, list[float]],
    expected_runs: int,
) -> list[float]:
    smf_select_subscribe_values, _status = estimated_standard_smf_select_subscribe_values(
        standard_values, expected_runs
    )

    return totals_from_components(
        [
            standard_values["Subscriber database discovery"],
            standard_values["Access and mobility subscription data"],
            standard_values["SDM subscription setup am-data"],
            standard_values["SMF selection subscription data"],
            smf_select_subscribe_values,
            standard_local_values["NSSSelection_Get"],
            standard_local_values["AMPolicyControl_Create"],
            standard_local_values["PDUSession_CreateSMContext"],
            standard_values["Session management subscription data"],
            standard_values["SDM subscription setup sm-data"],
            standard_local_values["SMPolicyControl_Create"],
        ],
        expected_runs,
    )


def topssim_runtime_totals(
    topssim_values: dict[str, list[float]],
    topssim_local_values: dict[str, list[float]],
    topssim_source_values: dict[str, list[float]],
    expected_runs: int,
) -> list[float]:
    return totals_from_components(
        [
            topssim_source_values["am-data"],
            topssim_values["Access and mobility subscription data"],
            topssim_source_values["smf-select-data"],
            topssim_values["SMF selection subscription data"],
            topssim_local_values["NSSSelection_Get"],
            topssim_local_values["AMPolicyControl_Create"],
            topssim_local_values["PDUSession_CreateSMContext"],
            topssim_source_values["sm-data"],
            topssim_values["Session management subscription data"],
            topssim_local_values["SMPolicyControl_Create"],
        ],
        expected_runs,
    )


def active_topssim_totals(
    topssim_values: dict[str, list[float]], expected_runs: int
) -> list[float]:
    totals = []
    for run_index in range(expected_runs):
        total = 0.0
        has_value = False
        for label, _service, _local_resource, _predicate in RUNTIME_COMPONENTS:
            if label in TOPSSIM_REMOVED_RUNTIME_COMPONENTS:
                continue
            values = topssim_values[label]
            if run_index < len(values):
                total += values[run_index]
                has_value = True
        if has_value:
            totals.append(total)

    return totals


def runtime_total_comparison_table(
    standard_totals: list[float],
    topssim_totals: list[float],
    delta: str,
    reduction: str,
) -> str:
    return markdown_table(*runtime_total_comparison_rows(
        standard_totals, topssim_totals, delta, reduction
    ))


def runtime_total_comparison_rows(
    standard_totals: list[float],
    topssim_totals: list[float],
    delta: str,
    reduction: str,
) -> tuple[list[str], list[list[str]]]:
    return (
        ["Runtime flow", "n", "Mean (ms)", "Variance", "SD"],
        [
            [
                "Standard total measured runtime critical path",
                *stats_cells(standard_totals),
            ],
            [
                "TOPSSIM total measured runtime critical path",
                *stats_cells(topssim_totals),
            ],
            [
                "Delta standard - TOPSSIM",
                "-",
                delta,
                "-",
                "-",
            ],
            ["Reduction", "-", reduction, "-", "-"],
        ],
    )


def standard_runtime_table(
    standard_values: dict[str, list[float]],
    standard_local_values: dict[str, list[float]],
    standard_totals: list[float],
    expected_runs: int,
) -> str:
    return markdown_table(*standard_runtime_rows(
        standard_values, standard_local_values, standard_totals, expected_runs
    ))


def standard_runtime_rows(
    standard_values: dict[str, list[float]],
    standard_local_values: dict[str, list[float]],
    standard_totals: list[float],
    expected_runs: int,
) -> tuple[list[str], list[list[str]]]:
    smf_select_subscribe_values, smf_select_subscribe_status = (
        estimated_standard_smf_select_subscribe_values(standard_values, expected_runs)
    )

    rows = [
        flow_row(
            "Subscriber database discovery: NFDiscovery(UDM) + SearchResult(H-UDM)",
            standard_values["Subscriber database discovery"],
        ),
        flow_row(
            "SDM_Get(am-data)",
            standard_values["Access and mobility subscription data"],
        ),
        flow_row(
            "SDM_Subscribe(am-data)",
            standard_values["SDM subscription setup am-data"],
            "Measured" if standard_values["SDM subscription setup am-data"]
            else "Not observed in current Open5GS logs",
        ),
        flow_row(
            "SDM_Get(smf-select-data)",
            standard_values["SMF selection subscription data"],
        ),
        flow_row(
            "SDM_Subscribe(smf-select-data)",
            smf_select_subscribe_values,
            smf_select_subscribe_status,
        ),
        flow_row(
            "NSSSelection_Get",
            standard_local_values["NSSSelection_Get"],
            "Measured by SBI timing logs" if standard_local_values["NSSSelection_Get"]
            else "Not observed; requires instrumented rerun",
        ),
        flow_row(
            "AMPolicyControl_Create",
            standard_local_values["AMPolicyControl_Create"],
            "Measured by SBI timing logs" if standard_local_values["AMPolicyControl_Create"]
            else "Not observed; requires instrumented rerun",
        ),
        flow_row(
            "PDUSession_CreateSMContext",
            standard_local_values["PDUSession_CreateSMContext"],
            "Measured by SBI timing logs" if standard_local_values["PDUSession_CreateSMContext"]
            else "Not observed; set SKIP_PDU_SESSION=0 and rerun",
        ),
        flow_row(
            "Subscriber database discovery for sm-data: NFDiscovery(UDM) + SearchResult(H-UDM)",
            [],
            "Included in Subscriber database discovery aggregate when observed",
        ),
        flow_row(
            "SDM_Get(sm-data)",
            standard_values["Session management subscription data"],
            "Measured" if standard_values["Session management subscription data"]
            else "Not observed; set SKIP_PDU_SESSION=0 and rerun",
        ),
        flow_row(
            "SDM_Subscribe(sm-data)",
            standard_values["SDM subscription setup sm-data"],
            "Measured" if standard_values["SDM subscription setup sm-data"]
            else "Not observed; set SKIP_PDU_SESSION=0 and rerun",
        ),
        flow_row(
            "SMPolicyControl_Create",
            standard_local_values["SMPolicyControl_Create"],
            "Measured by SBI timing logs" if standard_local_values["SMPolicyControl_Create"]
            else "Not observed; set SKIP_PDU_SESSION=0 and rerun",
        ),
        flow_row(
            "Open5GS extra: SDM_Get(ue-context-in-smf-data)",
            standard_values["UE context in SMF data"],
            "Measured but not shown in the theoretical call flow",
        ),
        flow_row(
            "Total measured runtime critical path",
            standard_totals,
            "Measured total from observed user-profile exchanges",
        ),
    ]

    return (
        [
            "Standard runtime critical-path component",
            "Status",
            "n",
            "Mean (ms)",
            "Variance",
            "SD",
        ],
        rows,
    )


def topssim_runtime_table(
    topssim_values: dict[str, list[float]],
    topssim_local_values: dict[str, list[float]],
    topssim_source_values: dict[str, list[float]],
    topssim_totals: list[float],
    expected_runs: int,
) -> str:
    return markdown_table(*topssim_runtime_rows(
        topssim_values,
        topssim_local_values,
        topssim_source_values,
        topssim_totals,
        expected_runs,
    ))


def topssim_runtime_rows(
    topssim_values: dict[str, list[float]],
    topssim_local_values: dict[str, list[float]],
    topssim_source_values: dict[str, list[float]],
    topssim_totals: list[float],
    expected_runs: int,
) -> tuple[list[str], list[list[str]]]:
    initial_source_values = totals_from_components(
        [
            topssim_source_values["am-data"],
            topssim_source_values["smf-select-data"],
        ],
        expected_runs,
    )

    rows = [
        flow_row(
            "Prepared profile source selection",
            initial_source_values,
            "Measured by AMF-SDM-CF source-selection logs"
            if initial_source_values
            else "Not observed; requires instrumented rerun",
        ),
        flow_row(
            "Subscriber database discovery: NFDiscovery(UDM) + SearchResult(H-UDM)",
            [],
            "Moved to preparation phase",
        ),
        flow_row(
            "SDM-CF local lookup am-data",
            topssim_values["Access and mobility subscription data"],
            "Measured by AMF-SDM-CF timing logs" if topssim_values["Access and mobility subscription data"]
            else "Not observed; requires instrumented rerun",
        ),
        flow_row(
            "SDM-CF local lookup smf-select-data",
            topssim_values["SMF selection subscription data"],
            "Measured by AMF-SDM-CF timing logs" if topssim_values["SMF selection subscription data"]
            else "Not observed; requires instrumented rerun",
        ),
        flow_row(
            "NSSSelection_Get",
            topssim_local_values["NSSSelection_Get"],
            "Measured by SBI timing logs" if topssim_local_values["NSSSelection_Get"]
            else "Not observed; requires instrumented rerun",
        ),
        flow_row(
            "AMPolicyControl_Create",
            topssim_local_values["AMPolicyControl_Create"],
            "Measured by SBI timing logs" if topssim_local_values["AMPolicyControl_Create"]
            else "Not observed; requires instrumented rerun",
        ),
        flow_row(
            "PDUSession_CreateSMContext",
            topssim_local_values["PDUSession_CreateSMContext"],
            "Measured by SBI timing logs" if topssim_local_values["PDUSession_CreateSMContext"]
            else "Not observed; set SKIP_PDU_SESSION=0 and rerun",
        ),
        flow_row(
            "Prepared profile source selection for sm-data",
            topssim_source_values["sm-data"],
            "Measured by SDM-CF source-selection logs"
            if topssim_source_values["sm-data"]
            else "Not observed; requires instrumented rerun",
        ),
        flow_row(
            "SDM-CF local lookup sm-data",
            topssim_values["Session management subscription data"],
            "Measured" if topssim_values["Session management subscription data"]
            else "Not observed; requires instrumented rerun",
        ),
        flow_row(
            "SMPolicyControl_Create",
            topssim_local_values["SMPolicyControl_Create"],
            "Measured by SBI timing logs" if topssim_local_values["SMPolicyControl_Create"]
            else "Not observed; set SKIP_PDU_SESSION=0 and rerun",
        ),
        flow_row(
            "SDM_Subscribe(am-data/smf-select-data/sm-data)",
            [],
            "Moved to post-setup phase",
        ),
        flow_row(
            "Open5GS extra: SDM-CF local lookup ue-context-in-smf-data",
            topssim_values["UE context in SMF data"],
            "Measured by AMF-SDM-CF timing logs; not shown in the theoretical call flow"
            if topssim_values["UE context in SMF data"]
            else "Not observed; requires instrumented rerun",
        ),
        flow_row(
            "Total measured runtime critical path",
            topssim_totals,
            "Measured total from observed runtime SDM-CF lookups",
        ),
    ]

    return (
        [
            "TOPSSIM runtime critical-path component",
            "Status",
            "n",
            "Mean (ms)",
            "Variance",
            "SD",
        ],
        rows,
    )


def flow_row(label: str, values: list[float], status: str = "Measured") -> list[str]:
    return [label, status, *stats_cells(values)]


def runtime_table_detailed(
    standard_logs: list[Path],
    topssim_logs: list[Path],
    year: int,
) -> str:
    standard_values, standard_totals = runtime_values_by_component(standard_logs, year)
    topssim_values, topssim_totals = runtime_values_by_component(
        topssim_logs, year, topssim_logs
    )
    rows = []

    for label, _service, _local_resource, _predicate in RUNTIME_COMPONENTS:
        rows.append(
            [
                label,
                *stats_cells(standard_values[label]),
                *stats_cells(topssim_values[label]),
            ]
        )

    rows.append(
        [
            "Total measured user-profile path",
            *stats_cells(standard_totals),
            *stats_cells(topssim_totals),
        ]
    )

    return markdown_table(
        [
            "Runtime critical-path component",
            "Standard n",
            "Standard mean (ms)",
            "Standard variance",
            "Standard SD",
            "TOPSSIM n",
            "TOPSSIM mean (ms)",
            "TOPSSIM variance",
            "TOPSSIM SD",
        ],
        rows,
    )


def post_setup_table(topssim_logs: list[Path], audit_logs: list[Path], year: int) -> str:
    headers, rows = post_setup_rows(topssim_logs, audit_logs, year)
    return markdown_table(headers, rows)


def post_setup_rows(
    topssim_logs: list[Path], audit_logs: list[Path], year: int
) -> tuple[list[str], list[list[str]]]:
    values = post_setup_values(topssim_logs, audit_logs, year)
    rows = []

    for label in sorted(values):
        rows.append([label, *stats_cells(values[label])])

    if not rows:
        rows.append(["No post-setup events found", "0", "-", "-", "-"])

    return ["Post-setup component", "n", "Mean (ms)", "Variance", "SD"], rows


def html_table(headers: list[str], rows: list[list[str]], title: str | None = None) -> str:
    title_html = f"<h3>{escape(title)}</h3>" if title else ""
    header_html = "".join(f"<th>{escape(header)}</th>" for header in headers)
    body_rows = []
    for row in rows:
        body_rows.append(
            "<tr>" + "".join(f"<td>{escape(cell)}</td>" for cell in row) + "</tr>"
        )
    return (
        f"{title_html}<table><thead><tr>{header_html}</tr></thead>"
        f"<tbody>{''.join(body_rows)}</tbody></table>"
    )


def html_report(
    standard_logs: list[Path],
    topssim_logs: list[Path],
    audit_logs: list[Path],
    year: int,
) -> str:
    prep_headers, prep_rows = preparation_rows(audit_logs, topssim_logs, year)
    comparison, standard, topssim = runtime_tables_data(standard_logs, topssim_logs, year)
    post_headers, post_rows = post_setup_rows(topssim_logs, audit_logs, year)

    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>TOPSSIM User-Profile Campaign Report</title>
  <style>
    :root {{
      color-scheme: light;
      --ink: #172033;
      --muted: #5f6b7a;
      --line: #c9d2df;
      --soft: #f6f8fb;
      --standard: #365f91;
      --topssim: #147568;
    }}
    body {{
      margin: 0;
      padding: 32px;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      color: var(--ink);
      background: white;
    }}
    main {{
      max-width: 1480px;
      margin: 0 auto;
    }}
    h1 {{
      margin: 0 0 8px;
      font-size: 28px;
      font-weight: 700;
      letter-spacing: 0;
    }}
    h2 {{
      margin: 30px 0 12px;
      padding-bottom: 8px;
      border-bottom: 2px solid var(--line);
      font-size: 20px;
    }}
    h3 {{
      margin: 0 0 10px;
      font-size: 16px;
    }}
    .meta {{
      margin: 0 0 22px;
      color: var(--muted);
      font-size: 14px;
    }}
    .runtime-grid {{
      display: grid;
      grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
      gap: 18px;
      align-items: start;
    }}
    .panel {{
      border: 1px solid var(--line);
      padding: 14px;
      background: white;
    }}
    .panel.standard {{
      border-top: 4px solid var(--standard);
    }}
    .panel.topssim {{
      border-top: 4px solid var(--topssim);
    }}
    table {{
      width: 100%;
      border-collapse: collapse;
      table-layout: fixed;
      font-size: 13px;
    }}
    th, td {{
      border: 1px solid var(--line);
      padding: 7px 8px;
      vertical-align: top;
      overflow-wrap: anywhere;
    }}
    th {{
      background: var(--soft);
      text-align: left;
      font-weight: 650;
    }}
    td:nth-child(n+3), th:nth-child(n+3) {{
      text-align: right;
      width: 88px;
    }}
    .comparison td:nth-child(n+2), .comparison th:nth-child(n+2) {{
      text-align: right;
      width: 88px;
    }}
    .comparison td:first-child, .comparison th:first-child,
    td:nth-child(2), th:nth-child(2) {{
      width: auto;
    }}
    .comparison {{
      margin-bottom: 18px;
    }}
    @media (max-width: 900px) {{
      body {{
        padding: 18px;
      }}
      .runtime-grid {{
        grid-template-columns: 1fr;
      }}
    }}
  </style>
</head>
<body>
<main>
  <h1>TOPSSIM User-Profile Campaign Report</h1>
  <p class="meta">Generated from {len(standard_logs)} standard runs and {len(topssim_logs)} TOPSSIM runs.</p>

  <h2>Preparation Phase</h2>
  {html_table(prep_headers, prep_rows)}

  <h2>Runtime Critical Path</h2>
  <div class="comparison">
    {html_table(*comparison, title="Runtime total comparison")}
  </div>
  <div class="runtime-grid">
    <section class="panel standard">
      {html_table(*standard, title="Standard")}
    </section>
    <section class="panel topssim">
      {html_table(*topssim, title="TOPSSIM")}
    </section>
  </div>

  <h2>Post-Setup Phase</h2>
  {html_table(post_headers, post_rows)}
</main>
</body>
</html>
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate a multi-run user-profile report with variance"
    )
    parser.add_argument("--standard-sepp-log", type=Path, action="append")
    parser.add_argument("--standard-sepp-glob", action="append")
    parser.add_argument("--topssim-sepp-log", type=Path, action="append")
    parser.add_argument("--topssim-sepp-glob", action="append")
    parser.add_argument("--topssim-cache-audit", type=Path, action="append")
    parser.add_argument("--topssim-cache-audit-glob", action="append")
    parser.add_argument("--topssim-cache-dir", type=Path, action="append")
    parser.add_argument(
        "--detailed-runtime",
        action="store_true",
        help="Print n, mean, variance, and SD columns for runtime instead of compact mean ± SD",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "html"),
        default="markdown",
        help="Output format",
    )
    parser.add_argument("--year", type=int, default=datetime.now().year)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    standard_logs = expand_paths(args.standard_sepp_log, args.standard_sepp_glob)
    topssim_logs = expand_paths(args.topssim_sepp_log, args.topssim_sepp_glob)
    audit_logs = expand_paths(args.topssim_cache_audit, args.topssim_cache_audit_glob)

    if args.topssim_cache_dir:
        audit_logs.extend(path / "audit.jsonl" for path in args.topssim_cache_dir)

    standard_logs = [path for path in standard_logs if path.exists()]
    topssim_logs = [path for path in topssim_logs if path.exists()]
    audit_logs = [path for path in audit_logs if path.exists()]

    if not standard_logs:
        raise SystemExit("No standard SEPP logs found")
    if not topssim_logs:
        raise SystemExit("No TOPSSIM SEPP logs found")

    if args.format == "html":
        print(html_report(standard_logs, topssim_logs, audit_logs, args.year))
        return 0

    print("## Preparation Phase")
    print(preparation_table(audit_logs, topssim_logs, args.year))
    print()
    print("## Runtime Critical Path")
    if args.detailed_runtime:
        print(runtime_table_detailed(standard_logs, topssim_logs, args.year))
    else:
        print(runtime_table_compact(standard_logs, topssim_logs, args.year))
    print()
    print("## Post-Setup Phase")
    print(post_setup_table(topssim_logs, audit_logs, args.year))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
