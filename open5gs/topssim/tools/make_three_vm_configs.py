#!/usr/bin/env python3
"""Generate TOPSSIM three-VM roaming configs from Open5GS example configs."""

from __future__ import annotations

import argparse
from pathlib import Path


EXAMPLES = Path("build/configs/examples")

HPLMN_ACCESS_CONTROL = """  access_control:
    - plmn_id:
        mcc: 001
        mnc: 01
    - plmn_id:
        mcc: 001
        mnc: 02
    - plmn_id:
        mcc: 315
        mnc: 010"""

VPLMN_ACCESS_CONTROL = HPLMN_ACCESS_CONTROL


def replace_all(text: str, replacements: dict[str, str]) -> str:
    for old, new in sorted(replacements.items(), key=lambda item: len(item[0]), reverse=True):
        text = text.replace(old, new)
    return text


def replace_access_control(text: str, replacement: str) -> str:
    start = text.find("  access_control:")
    if start < 0:
        return text

    end = text.find("  guami:", start)
    if end < 0:
        return text

    return text[:start] + replacement + "\n" + text[end:]


def remove_sepp3_peer(text: str) -> str:
    lines = text.splitlines()
    kept = []
    skip = False

    for line in lines:
        if line.startswith("        - receiver: sepp3.localdomain"):
            skip = True
            continue
        if skip and line.startswith("        - receiver: "):
            skip = False
        if skip and line and not line.startswith(" "):
            skip = False

        if not skip:
            kept.append(line)

    return "\n".join(kept) + "\n"


def write_config(source: Path, target: Path, replacements: dict[str, str],
                 access_control: str | None = None) -> None:
    text = source.read_text(encoding="utf-8")
    text = replace_all(text, replacements)
    if access_control:
        text = replace_access_control(text, access_control)
    text = remove_sepp3_peer(text)
    target.write_text(text, encoding="utf-8")
    print(f"Wrote {target}")


def set_n32f_port(text: str, address: str, port: int) -> str:
    return text.replace(
        f"n32f:\n          address: {address}\n          port: 7777",
        f"n32f:\n          address: {address}\n          port: {port}",
    )


def set_peer_n32f_port(text: str, address: str, port: int) -> str:
    return text.replace(
        f"n32f:\n            uri: http://{address}:7777",
        f"n32f:\n            uri: http://{address}:{port}",
    )


def replace_after_marker(text: str, marker: str, old: str, new: str) -> str:
    start = text.find(marker)
    if start < 0:
        raise ValueError(f"Cannot find marker: {marker!r}")
    pos = text.find(old, start)
    if pos < 0:
        raise ValueError(f"Cannot find text after marker {marker!r}: {old!r}")
    return text[:pos] + new + text[pos + len(old):]


def expose_cloud_runtime_endpoints(
    text: str,
    public_ip: str,
    amf_loopback: str,
    upf_loopback: str,
) -> str:
    text = replace_after_marker(
        text,
        "\namf:\n",
        f"  ngap:\n    server:\n      - address: {amf_loopback}",
        f"  ngap:\n    server:\n      - address: {public_ip}",
    )
    text = replace_after_marker(
        text,
        "\nupf:\n",
        f"  gtpu:\n    server:\n      - address: {upf_loopback}",
        f"  gtpu:\n    server:\n      - address: {public_ip}",
    )
    return text


def write_cloud_config(
    source: Path,
    target: Path,
    replacements: dict[str, str],
    local_public_ip: str,
    peer_public_ip: str,
    n32f_port: int,
    amf_loopback: str,
    upf_loopback: str,
    open5gs_path: str,
    access_control: str | None = None,
) -> None:
    text = source.read_text(encoding="utf-8")
    text = text.replace("/home/alexis/TestBed5G/open5gs", open5gs_path)
    text = replace_all(text, replacements)
    if access_control:
        text = replace_access_control(text, access_control)
    text = remove_sepp3_peer(text)
    text = expose_cloud_runtime_endpoints(
        text, local_public_ip, amf_loopback, upf_loopback)
    text = set_n32f_port(text, local_public_ip, n32f_port)
    text = set_peer_n32f_port(text, peer_public_ip, n32f_port)
    target.write_text(text, encoding="utf-8")
    print(f"Wrote {target}")


def force_plmn_support(text: str, mnc: str) -> str:
    text = text.replace("mcc: 999\n      mnc: 70", f"mcc: 001\n      mnc: {mnc}")
    text = text.replace("mcc: 001\n      mnc: 01", f"mcc: 001\n      mnc: {mnc}")
    return text


def write_ue_config(source: Path, target: Path, replacements: dict[str, str]) -> None:
    text = source.read_text(encoding="utf-8")
    text = replace_all(text, replacements)
    text = force_plmn_support(text, "01")
    target.write_text(text, encoding="utf-8")
    print(f"Wrote {target}")


def make_hplmn() -> None:
    write_config(
        EXAMPLES / "5gc-sepp1-999-70.yaml",
        EXAMPLES / "topssim-hplmn.yaml",
        {
            "mnc070.mcc999": "mnc001.mcc001",
            "mcc: 999": "mcc: 001",
            "mnc: 70": "mnc: 01",
            "127.0.1.4": "172.16.85.125",
            "127.0.1.5": "172.16.85.128",
            "127.0.1.7": "172.16.85.100",
            "127.0.1.10": "172.16.85.120",
            "127.0.1.11": "172.16.85.121",
            "127.0.1.12": "172.16.85.122",
            "127.0.1.13": "172.16.85.123",
            "127.0.1.14": "172.16.85.124",
            "127.0.1.15": "172.16.85.126",
            "127.0.1.20": "172.16.85.127",
            "127.0.1.200": "172.16.85.129",
            "127.0.1.250": "172.16.85.101",
            "127.0.1.251": "172.16.85.102",
            "127.0.1.252": "172.16.85.103",
            "127.0.2.251": "172.16.85.112",
            "127.0.2.252": "172.16.85.113",
            "127.0.3.251": "172.16.85.112",
            "127.0.3.252": "172.16.85.113",
        },
        HPLMN_ACCESS_CONTROL,
    )


def make_vplmn() -> None:
    write_config(
        EXAMPLES / "5gc-sepp2-001-01.yaml",
        EXAMPLES / "topssim-vplmn.yaml",
        {
            "mnc001.mcc001": "mnc002.mcc001",
            "mcc: 999": "mcc: 001",
            "mnc: 70": "mnc: 01",
            "mnc: 01": "mnc: 02",
            "127.0.2.4": "172.16.85.145",
            "127.0.2.5": "172.16.85.148",
            "127.0.2.7": "172.16.85.110",
            "127.0.2.10": "172.16.85.140",
            "127.0.2.11": "172.16.85.141",
            "127.0.2.12": "172.16.85.142",
            "127.0.2.13": "172.16.85.143",
            "127.0.2.14": "172.16.85.144",
            "127.0.2.15": "172.16.85.146",
            "127.0.2.20": "172.16.85.147",
            "127.0.2.200": "172.16.85.149",
            "127.0.2.250": "172.16.85.111",
            "127.0.2.251": "172.16.85.112",
            "127.0.2.252": "172.16.85.113",
            "127.0.1.251": "172.16.85.102",
            "127.0.1.252": "172.16.85.103",
            "127.0.3.251": "172.16.85.102",
            "127.0.3.252": "172.16.85.103",
        },
        VPLMN_ACCESS_CONTROL,
    )


def make_ue() -> None:
    candidates = [
        EXAMPLES / "gnb-001-01-ue-999-70.yaml",
        EXAMPLES / "gnb-001-01-ue-999-70-net10.yaml",
    ]
    source = next((path for path in candidates if path.exists()), candidates[0])
    write_ue_config(
        source,
        EXAMPLES / "topssim-ue.yaml",
        {
            "db_uri: mongodb://localhost/open5gs": "db_uri: mongodb://172.16.85.100/open5gs",
            "mnc070.mcc999": "mnc001.mcc001",
            "mnc001.mcc001": "mnc002.mcc001",
            "mcc: 999": "mcc: 001",
            "mnc: 70": "mnc: 01",
            "127.0.2.5": "172.16.85.148",
            "10.146.0.5": "172.16.85.148",
            "127.0.1.5": "172.16.85.128",
            "10.145.0.5": "172.16.85.128",
        },
    )


def make_hplmn_cloud(args: argparse.Namespace) -> None:
    write_cloud_config(
        EXAMPLES / "5gc-sepp1-999-70.yaml",
        EXAMPLES / "topssim-hplmn-cloud.yaml",
        {
            "mnc070.mcc999": "mnc001.mcc001",
            "mcc: 999": "mcc: 001",
            "mnc: 70": "mnc: 01",
            # Public SEPP endpoints consumed outside the HPLMN VM.
            "127.0.1.251": args.hplmn_ip,
            "127.0.1.252": args.hplmn_ip,
            "127.0.2.251": args.vplmn_ip,
            "127.0.2.252": args.vplmn_ip,
            "127.0.3.251": args.vplmn_ip,
            "127.0.3.252": args.vplmn_ip,
        },
        args.hplmn_ip,
        args.vplmn_ip,
        args.n32f_port,
        "127.0.1.5",
        "127.0.1.7",
        args.core_open5gs,
        HPLMN_ACCESS_CONTROL,
    )


def make_vplmn_cloud(args: argparse.Namespace) -> None:
    write_cloud_config(
        EXAMPLES / "5gc-sepp2-001-01.yaml",
        EXAMPLES / "topssim-vplmn-cloud.yaml",
        {
            "mnc001.mcc001": "mnc002.mcc001",
            "mcc: 999": "mcc: 001",
            "mnc: 70": "mnc: 01",
            "mnc: 01": "mnc: 02",
            # Public SEPP endpoints consumed outside the VPLMN VM.
            "127.0.2.251": args.vplmn_ip,
            "127.0.2.252": args.vplmn_ip,
            "127.0.1.251": args.hplmn_ip,
            "127.0.1.252": args.hplmn_ip,
            "127.0.3.251": args.hplmn_ip,
            "127.0.3.252": args.hplmn_ip,
        },
        args.vplmn_ip,
        args.hplmn_ip,
        args.n32f_port,
        "127.0.2.5",
        "127.0.2.7",
        args.core_open5gs,
        VPLMN_ACCESS_CONTROL,
    )


def make_ue_cloud(args: argparse.Namespace) -> None:
    candidates = [
        EXAMPLES / "gnb-001-01-ue-999-70.yaml",
        EXAMPLES / "gnb-001-01-ue-999-70-net10.yaml",
    ]
    source = next((path for path in candidates if path.exists()), candidates[0])
    write_ue_config(
        source,
        EXAMPLES / "topssim-ue-cloud.yaml",
        {
            "db_uri: mongodb://localhost/open5gs": "db_uri: mongodb://127.0.0.1:27018/open5gs",
            "/home/alexis/TestBed5G/open5gs": args.ue_open5gs,
            "mnc070.mcc999": "mnc001.mcc001",
            "mnc001.mcc001": "mnc002.mcc001",
            "mcc: 999": "mcc: 001",
            "mnc: 70": "mnc: 01",
            "127.0.2.5": args.vplmn_ip,
            "10.146.0.5": args.vplmn_ip,
            "127.0.1.5": args.hplmn_ip,
            "10.145.0.5": args.hplmn_ip,
        },
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "role",
        choices=["hplmn", "vplmn", "ue", "all"],
        help="Config set to generate",
    )
    parser.add_argument(
        "--cloud-single-ip",
        action="store_true",
        help="Generate *-cloud.yaml configs for VMs with one routable IP each",
    )
    parser.add_argument("--hplmn-ip", default="107.191.48.152")
    parser.add_argument("--vplmn-ip", default="216.128.180.130")
    parser.add_argument("--ue-ip", default="104.238.133.155")
    parser.add_argument("--core-open5gs", default="/home/alexis/TOPSSIM/open5gs")
    parser.add_argument("--ue-open5gs", default="/home/alexis/TestBed5G/open5gs")
    parser.add_argument(
        "--n32f-port",
        type=int,
        default=7778,
        help="N32 forwarding port when N32C and N32F share one public IP",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.cloud_single_ip:
        if args.role in ("hplmn", "all"):
            make_hplmn_cloud(args)
        if args.role in ("vplmn", "all"):
            make_vplmn_cloud(args)
        if args.role in ("ue", "all"):
            make_ue_cloud(args)
    else:
        if args.role in ("hplmn", "all"):
            make_hplmn()
        if args.role in ("vplmn", "all"):
            make_vplmn()
        if args.role in ("ue", "all"):
            make_ue()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
