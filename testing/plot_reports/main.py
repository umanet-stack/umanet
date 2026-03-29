import argparse
import json
import re
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import matplotlib.pyplot as plt

plt.rcParams["pdf.fonttype"] = 42
plt.rcParams["ps.fonttype"] = 42

SCRIPT_DIR = Path(__file__).parent.resolve()
TESTING_DIR = SCRIPT_DIR.parent

COLOR_UMANET_1 = "#2ca02c"
COLOR_UMANET_2 = "#d62728"
COLOR_LINUX_1 = "#1f77b4"
COLOR_LINUX_2 = "#ff7f0e"
COLOR_OVS_DPDK_1 = "#9467bd"
COLOR_OVS_DPDK_2 = "#8c564b"


def parse_iperf_report(report_path: Path) -> Optional[Dict]:
    try:
        with open(report_path, "r") as f:
            data = json.load(f)

        num_vms = data["total_vms"]
        total_throughput = data["total_throughput_gbps"]
        avg_per_vm = data["avg_per_vm_gbps"]

        return {
            "num_vms": num_vms,
            "total_throughput": total_throughput,
            "throughput_per_vm": avg_per_vm,
        }
    except Exception as e:
        print(f"Error parsing {report_path}: {e}")
        return None


def parse_iperf_udp_report(report_path: Path) -> Optional[Dict]:
    try:
        with open(report_path, "r") as f:
            data = json.load(f)

        total_vms = data["total_vms"]
        sender_pps_total = data["total_sender_pps"]
        receiver_pps_total = data["total_receiver_pps"]
        lost_pps_total = sender_pps_total - receiver_pps_total

        return {
            "num_vms": total_vms,
            "sender_pps_total": sender_pps_total,
            "receiver_pps_total": receiver_pps_total,
            "lost_pps_total": lost_pps_total,
        }
    except Exception as e:
        print(f"Error parsing {report_path}: {e}")
        return None


def parse_sockperf_report(report_path: Path) -> Optional[Dict]:
    try:
        with open(report_path, "r") as f:
            data = json.load(f)

        total_vms = data["total_vms"]
        p99_latency = data["avg_p99_usec"]
        total_received_messages = data["total_received_messages"]

        return {
            "num_vms": total_vms,
            "p99_latency": p99_latency,
            "total_received": total_received_messages,
        }
    except Exception as e:
        print(f"Error parsing {report_path}: {e}")
        return None


def collect_reports(test_type: str) -> Tuple[List[Dict], List[Dict], List[Dict]]:
    tap_reports = []
    dpdk_reports = []
    ovs_dpdk_reports = []

    tap_base = TESTING_DIR / "tap" / test_type / "vm-client"
    dpdk_base = TESTING_DIR / "dpdk" / test_type / "vm-client"
    ovs_dpdk_base = TESTING_DIR / "ovs-dpdk" / test_type / "vm-client"

    if test_type == "iperf":
        parse_func = parse_iperf_report
    elif test_type == "iperf-udp":
        parse_func = parse_iperf_udp_report
    elif test_type == "sockperf":
        parse_func = parse_sockperf_report
    else:
        raise ValueError(f"Unknown test type: {test_type}")

    if tap_base.exists():
        for report_dir in tap_base.glob("report-*vm"):
            report_path = report_dir / "report.json"
            if report_path.exists():
                data = parse_func(report_path)
                if data:
                    tap_reports.append(data)
            else:
                print(f"⚠️ warning: tap report not found: {report_path}")

    if dpdk_base.exists():
        for report_dir in dpdk_base.glob("report-*vm"):
            report_path = report_dir / "report.json"
            if report_path.exists():
                data = parse_func(report_path)
                if data:
                    dpdk_reports.append(data)
            else:
                print(f"⚠️ warning: dpdk report not found: {report_path}")

    if ovs_dpdk_base.exists():
        for report_dir in ovs_dpdk_base.glob("report-*vm"):
            report_path = report_dir / "report.json"
            if report_path.exists():
                data = parse_func(report_path)
                if data:
                    ovs_dpdk_reports.append(data)
            else:
                print(f"⚠️ warning: ovs-dpdk report not found: {report_path}")

    tap_reports.sort(key=lambda x: x["num_vms"])
    dpdk_reports.sort(key=lambda x: x["num_vms"])
    ovs_dpdk_reports.sort(key=lambda x: x["num_vms"])

    return tap_reports, dpdk_reports, ovs_dpdk_reports


def plot_iperf(
    tap_reports: List[Dict],
    dpdk_reports: List[Dict],
    ovs_dpdk_reports: List[Dict],
    output_dir: Path,
):
    fig, ax = plt.subplots(1, 1, figsize=(10, 6))

    if tap_reports:
        tap_vms = [r["num_vms"] for r in tap_reports]
        tap_total = [r["total_throughput"] for r in tap_reports]
        tap_per_vm = [r["throughput_per_vm"] for r in tap_reports]
        ax.plot(
            tap_vms,
            tap_total,
            "o-",
            label="Linux (Total)",
            linewidth=2,
            markersize=5,
            color=COLOR_LINUX_1,
        )
        ax.plot(
            tap_vms,
            tap_per_vm,
            "o--",
            label="Linux (Per VM)",
            linewidth=2,
            markersize=5,
            color=COLOR_LINUX_2,
        )

    if dpdk_reports:
        dpdk_vms = [r["num_vms"] for r in dpdk_reports]
        dpdk_total = [r["total_throughput"] for r in dpdk_reports]
        dpdk_per_vm = [r["throughput_per_vm"] for r in dpdk_reports]
        ax.plot(
            dpdk_vms,
            dpdk_total,
            "s-",
            label="UMANet (Total)",
            linewidth=2,
            markersize=5,
            color=COLOR_UMANET_1,
        )
        ax.plot(
            dpdk_vms,
            dpdk_per_vm,
            "s--",
            label="UMANet (Per VM)",
            linewidth=2,
            markersize=5,
            color=COLOR_UMANET_2,
        )

    if ovs_dpdk_reports:
        ovs_dpdk_vms = [r["num_vms"] for r in ovs_dpdk_reports]
        ovs_dpdk_total = [r["total_throughput"] for r in ovs_dpdk_reports]
        ovs_dpdk_per_vm = [r["throughput_per_vm"] for r in ovs_dpdk_reports]
        ax.plot(
            ovs_dpdk_vms,
            ovs_dpdk_total,
            "^-",
            label="OVS-DPDK (Total)",
            linewidth=2,
            markersize=5,
            color=COLOR_OVS_DPDK_1,
        )
        ax.plot(
            ovs_dpdk_vms,
            ovs_dpdk_per_vm,
            "^--",
            label="OVS-DPDK (Per VM)",
            linewidth=2,
            markersize=5,
            color=COLOR_OVS_DPDK_2,
        )

    ax.set_xlabel("VM Count", fontsize=18)
    ax.set_ylabel("Throughput (Gbps)", fontsize=18)
    ax.set_yscale("log")
    ax.tick_params(axis="both", which="major", labelsize=16)
    ax.legend(fontsize=15, loc="best")
    ax.grid(True, alpha=0.3, which="both")
    for vm_count in [32, 48, 64]:
        ax.axvline(x=vm_count, color="red", linestyle="--", linewidth=1.5, alpha=0.7)

    plt.tight_layout()
    output_path_png = output_dir / "iperf_comparison.png"
    output_path_pdf = output_dir / "iperf_comparison.pdf"
    plt.savefig(output_path_png, dpi=300, bbox_inches="tight")
    plt.savefig(output_path_pdf, bbox_inches="tight")
    print(f"Saved: {output_path_png}")
    print(f"Saved: {output_path_pdf}")
    plt.close()


def plot_sockperf(
    tap_reports: List[Dict],
    dpdk_reports: List[Dict],
    ovs_dpdk_reports: List[Dict],
    output_dir: Path,
):
    fig1, ax1 = plt.subplots(1, 1, figsize=(8, 5))

    if tap_reports:
        tap_vms = [r["num_vms"] for r in tap_reports]
        tap_p99 = [r["p99_latency"] for r in tap_reports]
        ax1.plot(
            tap_vms,
            tap_p99,
            "o-",
            label="Linux",
            linewidth=2,
            markersize=5,
            color=COLOR_LINUX_1,
        )

    if dpdk_reports:
        dpdk_vms = [r["num_vms"] for r in dpdk_reports]
        dpdk_p99 = [r["p99_latency"] for r in dpdk_reports]
        ax1.plot(
            dpdk_vms,
            dpdk_p99,
            "s-",
            label="UMANet",
            linewidth=2,
            markersize=5,
            color=COLOR_UMANET_1,
        )

    if ovs_dpdk_reports:
        ovs_dpdk_vms = [r["num_vms"] for r in ovs_dpdk_reports]
        ovs_dpdk_p99 = [r["p99_latency"] for r in ovs_dpdk_reports]
        ax1.plot(
            ovs_dpdk_vms,
            ovs_dpdk_p99,
            "^-",
            label="OVS-DPDK",
            linewidth=2,
            markersize=5,
            color=COLOR_OVS_DPDK_1,
        )

    for vm_count in [32, 48, 64]:
        ax1.axvline(x=vm_count, color="red", linestyle="--", linewidth=1.5, alpha=0.7)

    ax1.set_xlabel("VM Count", fontsize=18)
    ax1.set_ylabel("p99 Latency (μs)", fontsize=18)
    ax1.tick_params(axis="both", which="major", labelsize=16)
    ax1.legend(fontsize=15)
    ax1.grid(True, alpha=0.3)
    ax1.set_ylim(top=600)

    plt.tight_layout()
    output_path_png = output_dir / "sockperf_p99_latency.png"
    output_path_pdf = output_dir / "sockperf_p99_latency.pdf"
    plt.savefig(output_path_png, dpi=300, bbox_inches="tight")
    plt.savefig(output_path_pdf, bbox_inches="tight")
    print(f"Saved: {output_path_png}")
    print(f"Saved: {output_path_pdf}")
    plt.close()

    fig2, ax2 = plt.subplots(1, 1, figsize=(8, 5))

    if tap_reports:
        tap_vms = [r["num_vms"] for r in tap_reports]
        tap_received = [r["total_received"] for r in tap_reports]
        ax2.plot(
            tap_vms,
            tap_received,
            "o-",
            label="Linux",
            linewidth=2,
            markersize=5,
            color=COLOR_LINUX_1,
        )

    if dpdk_reports:
        dpdk_vms = [r["num_vms"] for r in dpdk_reports]
        dpdk_received = [r["total_received"] for r in dpdk_reports]
        ax2.plot(
            dpdk_vms,
            dpdk_received,
            "s-",
            label="UMANet",
            linewidth=2,
            markersize=5,
            color=COLOR_UMANET_1,
        )

    if ovs_dpdk_reports:
        ovs_dpdk_vms = [r["num_vms"] for r in ovs_dpdk_reports]
        ovs_dpdk_received = [r["total_received"] for r in ovs_dpdk_reports]
        ax2.plot(
            ovs_dpdk_vms,
            ovs_dpdk_received,
            "^-",
            label="OVS-DPDK",
            linewidth=2,
            markersize=5,
            color=COLOR_OVS_DPDK_1,
        )

    for vm_count in [32, 48, 64]:
        ax2.axvline(x=vm_count, color="red", linestyle="--", linewidth=1.5, alpha=0.7)

    ax2.set_xlabel("VM Count", fontsize=24)
    ax2.set_ylabel("Messages Received", fontsize=24)
    ax2.tick_params(axis="both", which="major", labelsize=22)
    ax2.legend(fontsize=21)
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    output_path_png = output_dir / "sockperf_messages_received.png"
    output_path_pdf = output_dir / "sockperf_messages_received.pdf"
    plt.savefig(output_path_png, dpi=300, bbox_inches="tight")
    plt.savefig(output_path_pdf, bbox_inches="tight")
    print(f"Saved: {output_path_png}")
    print(f"Saved: {output_path_pdf}")
    plt.close()


def plot_iperf_udp(
    tap_reports: List[Dict],
    dpdk_reports: List[Dict],
    ovs_dpdk_reports: List[Dict],
    output_dir: Path,
):
    fig1, ax1 = plt.subplots(1, 1, figsize=(8, 8))

    if tap_reports:
        tap_vms = [r["num_vms"] for r in tap_reports]
        tap_received = [r["receiver_pps_total"] for r in tap_reports]
        tap_sent = [r["sender_pps_total"] for r in tap_reports]
        ax1.plot(
            tap_vms,
            tap_sent,
            "o--",
            label="Sent (Linux)",
            linewidth=2,
            markersize=5,
            color=COLOR_LINUX_2,
        )
        ax1.plot(
            tap_vms,
            tap_received,
            "o-",
            label="Received (Linux)",
            linewidth=2,
            markersize=5,
            color=COLOR_LINUX_1,
        )

    if dpdk_reports:
        dpdk_vms = [r["num_vms"] for r in dpdk_reports]
        dpdk_received = [r["receiver_pps_total"] for r in dpdk_reports]
        dpdk_sent = [r["sender_pps_total"] for r in dpdk_reports]
        ax1.plot(
            dpdk_vms,
            dpdk_sent,
            "s--",
            label="Sent (UMANet)",
            linewidth=2,
            markersize=5,
            color=COLOR_UMANET_2,
        )
        ax1.plot(
            dpdk_vms,
            dpdk_received,
            "s-",
            label="Received (UMANet)",
            linewidth=2,
            markersize=5,
            color=COLOR_UMANET_1,
        )

    if ovs_dpdk_reports:
        ovs_dpdk_vms = [r["num_vms"] for r in ovs_dpdk_reports]
        ovs_dpdk_received = [r["receiver_pps_total"] for r in ovs_dpdk_reports]
        ovs_dpdk_sent = [r["sender_pps_total"] for r in ovs_dpdk_reports]
        ax1.plot(
            ovs_dpdk_vms,
            ovs_dpdk_sent,
            "^--",
            label="Sent (OVS-DPDK)",
            linewidth=2,
            markersize=5,
            color=COLOR_OVS_DPDK_2,
        )
        ax1.plot(
            ovs_dpdk_vms,
            ovs_dpdk_received,
            "^-",
            label="Received (OVS-DPDK)",
            linewidth=2,
            markersize=5,
            color=COLOR_OVS_DPDK_1,
        )

    for vm_count in [32, 48, 64]:
        ax1.axvline(x=vm_count, color="red", linestyle="--", linewidth=1.5, alpha=0.7)

    ax1.set_xlabel("VM Count", fontsize=18)
    ax1.set_ylabel("PPS", fontsize=18)
    ax1.tick_params(axis="both", which="major", labelsize=16)
    ax1.legend(fontsize=15, ncol=2, loc="upper center", bbox_to_anchor=(0.5, 1.3))
    ax1.grid(True, alpha=0.3)

    plt.tight_layout(rect=[0, 0, 1, 0.88])
    output_path_png = output_dir / "iperf_udp_pps.png"
    output_path_pdf = output_dir / "iperf_udp_pps.pdf"
    plt.savefig(output_path_png, dpi=300, bbox_inches="tight")
    plt.savefig(output_path_pdf, bbox_inches="tight")
    print(f"Saved: {output_path_png}")
    print(f"Saved: {output_path_pdf}")
    plt.close()

    fig2, ax2 = plt.subplots(1, 1, figsize=(8, 5))

    if tap_reports:
        tap_vms = [r["num_vms"] for r in tap_reports]
        tap_loss_pct = [
            (r["lost_pps_total"] / r["sender_pps_total"]) * 100 for r in tap_reports
        ]
        ax2.plot(
            tap_vms,
            tap_loss_pct,
            "o-",
            label="Linux",
            linewidth=2,
            markersize=5,
            color=COLOR_LINUX_2,
        )

    if ovs_dpdk_reports:
        ovs_dpdk_vms = [r["num_vms"] for r in ovs_dpdk_reports]
        ovs_dpdk_loss_pct = [
            (r["lost_pps_total"] / r["sender_pps_total"]) * 100
            for r in ovs_dpdk_reports
        ]
        ax2.plot(
            ovs_dpdk_vms,
            ovs_dpdk_loss_pct,
            "^-",
            label="OVS-DPDK",
            linewidth=2,
            markersize=5,
            color=COLOR_OVS_DPDK_2,
        )

    if dpdk_reports:
        dpdk_vms = [r["num_vms"] for r in dpdk_reports]
        dpdk_loss_pct = [
            (r["lost_pps_total"] / r["sender_pps_total"]) * 100 for r in dpdk_reports
        ]
        ax2.plot(
            dpdk_vms,
            dpdk_loss_pct,
            "s-",
            label="UMANet",
            linewidth=2,
            markersize=5,
            color=COLOR_UMANET_2,
        )

    for vm_count in [32, 48, 64]:
        ax2.axvline(x=vm_count, color="red", linestyle="--", linewidth=1.5, alpha=0.7)

    ax2.set_xlabel("VM Count", fontsize=18)
    ax2.set_ylabel("Packet Loss (%)", fontsize=18)
    ax2.tick_params(axis="both", which="major", labelsize=16)
    ax2.grid(True, alpha=0.3)
    ax2.legend(fontsize=15)

    plt.tight_layout()
    output_path_png = output_dir / "iperf_udp_packet_loss.png"
    output_path_pdf = output_dir / "iperf_udp_packet_loss.pdf"
    plt.savefig(output_path_png, dpi=300, bbox_inches="tight")
    plt.savefig(output_path_pdf, bbox_inches="tight")
    print(f"Saved: {output_path_png}")
    print(f"Saved: {output_path_pdf}")
    plt.close()


def main():
    parser = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
  ./main.py iperf
  ./main.py iperf-udp
  ./main.py sockperf
        """,
    )
    parser.add_argument(
        "test_type",
        choices=["iperf", "iperf-udp", "sockperf"],
    )

    args = parser.parse_args()

    print(f"plotting {args.test_type} comparison graphs...")
    print(f"testing directory: {TESTING_DIR}\n")

    tap_reports, dpdk_reports, ovs_dpdk_reports = collect_reports(args.test_type)
    print(f"Found {len(tap_reports)} tap reports")
    print(f"Found {len(dpdk_reports)} dpdk reports")
    print(f"Found {len(ovs_dpdk_reports)} ovs-dpdk reports\n")

    if not tap_reports and not dpdk_reports and not ovs_dpdk_reports:
        print("no reports found!")
        return

    gmt7 = timezone(timedelta(hours=7))
    now = datetime.now(gmt7)
    timestamp = now.strftime("%Y%m%d_%H")
    output_dir = SCRIPT_DIR / f"plots_{timestamp}"
    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"output directory: {output_dir}")
    print()

    if args.test_type == "iperf":
        plot_iperf(tap_reports, dpdk_reports, ovs_dpdk_reports, output_dir)
    elif args.test_type == "iperf-udp":
        plot_iperf_udp(tap_reports, dpdk_reports, ovs_dpdk_reports, output_dir)
    elif args.test_type == "sockperf":
        plot_sockperf(tap_reports, dpdk_reports, ovs_dpdk_reports, output_dir)

    print("done!")


if __name__ == "__main__":
    main()
