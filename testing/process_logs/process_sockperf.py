import argparse
import json
import re
from pathlib import Path
from typing import Dict, List, Tuple

import matplotlib
import matplotlib.pyplot as plt

matplotlib.use("Agg")
from datetime import datetime

import numpy as np

SCRIPT_DIR = Path(__file__).parent.resolve()


def load_results(
    logs_dir: Path, process_all_vms: bool = False
) -> Tuple[Dict[str, dict], int]:
    results = {}
    total_vms = 0

    for log_file in sorted(logs_dir.glob("vm*.log")):
        vm_name = log_file.stem
        vm_num = int(vm_name[2:])

        if process_all_vms or vm_num % 2 == 1:
            total_vms += 1
        if not process_all_vms and vm_num % 2 == 0:
            continue

        try:
            with open(log_file, "r") as f:
                log_content = f.read()

            # format: sockperf: ====> avg-latency=32.472 (std-dev=24.530)
            # .*? = non-greedy wildcard (match as few characters as possible)
            avg_latency_pattern = (
                r"sockperf:.*?avg-latency=([\d.]+)\s+\(std-dev=([\d.]+)\)"
            )
            avg_match = re.search(avg_latency_pattern, log_content)
            if not avg_match:
                print(f"⚠️  skipping {vm_name}: could not find avg-latency in {log_file}")
                continue

            avg_latency = float(avg_match.group(1))
            std_dev = float(avg_match.group(2))

            # format: sockperf: ---> percentile 50.000 =   29.526
            percentiles = {}
            percentile_pattern = r"sockperf:.*?percentile\s+([\d.]+)\s+=\s+([\d.]+)"
            for match in re.finditer(percentile_pattern, log_content):
                p = float(match.group(1))
                value = float(match.group(2))
                percentiles[p] = value

            # format: sockperf: ---> <MIN> observation =   18.061
            min_pattern = r"sockperf:.*?<MIN>\s+observation\s+=\s+([\d.]+)"
            max_pattern = r"sockperf:.*?<MAX>\s+observation\s+=\s+([\d.]+)"
            min_match = re.search(min_pattern, log_content)
            max_match = re.search(max_pattern, log_content)

            min_latency = float(min_match.group(1)) if min_match else 0
            max_latency = float(max_match.group(1)) if max_match else 0

            # format: sockperf: [Total Run] RunTime=29.978 sec; Warm up time=400 msec; SentMessages=459153; ReceivedMessages=459152
            total_run_pattern = r"sockperf:.*?\[Total Run\].*?SentMessages=(\d+);\s+ReceivedMessages=(\d+)"
            total_match = re.search(total_run_pattern, log_content)

            sent_messages = int(total_match.group(1)) if total_match else 0
            received_messages = int(total_match.group(2)) if total_match else 0

            # extract runtime
            runtime_pattern = r"sockperf:.*?\[Total Run\]\s+RunTime=([\d.]+)\s+sec"
            runtime_match = re.search(runtime_pattern, log_content)
            runtime = float(runtime_match.group(1)) if runtime_match else 0

            # skip VMs with invalid data
            if avg_latency <= 0 or sent_messages == 0:
                print( f"⚠️  skipping {vm_name}: invalid data (avg_latency={avg_latency}, sent={sent_messages})")
                continue

            results[vm_name] = {
                "avg_latency_usec": avg_latency,
                "std_dev_usec": std_dev,
                "min_latency_usec": min_latency,
                "max_latency_usec": max_latency,
                "percentiles": percentiles,
                "sent_messages": sent_messages,
                "received_messages": received_messages,
                "runtime_sec": runtime,
                "dropped_messages": sent_messages - received_messages,
            }

        except Exception as e:
            print(f"⚠️  error processing {log_file}: {e}")

    return results, total_vms


def extract_per_vm_stats(results: Dict[str, dict]) -> Dict[str, dict]:
    stats = {}

    for vm_name, data in results.items():
        percentiles = data.get("percentiles", {})

        stats[vm_name] = {
            "avg_latency_usec": data.get("avg_latency_usec", 0),
            "std_dev_usec": data.get("std_dev_usec", 0),
            "min_latency_usec": data.get("min_latency_usec", 0),
            "max_latency_usec": data.get("max_latency_usec", 0),
            "p25_usec": percentiles.get(25.0, 0),
            "p50_usec": percentiles.get(50.0, 0),
            "p75_usec": percentiles.get(75.0, 0),
            "p90_usec": percentiles.get(90.0, 0),
            "p99_usec": percentiles.get(99.0, 0),
            "p99_9_usec": percentiles.get(99.9, 0),
            "p99_99_usec": percentiles.get(99.99, 0),
            "p99_999_usec": percentiles.get(99.999, 0),
            "sent_messages": data.get("sent_messages", 0),
            "received_messages": data.get("received_messages", 0),
            "dropped_messages": data.get("dropped_messages", 0),
            "runtime_sec": data.get("runtime_sec", 0),
        }

    return stats


def calculate_overall_stats(per_vm_stats: Dict[str, dict], total_vms: int) -> dict:
    """calculate overall statistics across all VMs"""
    if not per_vm_stats:
        return {}

    completed_vms = len(per_vm_stats)
    avg_latency = np.mean([s["avg_latency_usec"] for s in per_vm_stats.values()])
    avg_p50 = np.mean([s["p50_usec"] for s in per_vm_stats.values()])
    avg_p90 = np.mean([s["p90_usec"] for s in per_vm_stats.values()])
    avg_p99 = np.mean([s["p99_usec"] for s in per_vm_stats.values()])
    avg_p99_9 = np.mean([s["p99_9_usec"] for s in per_vm_stats.values()])
    total_sent = sum(s["sent_messages"] for s in per_vm_stats.values())
    total_received = sum(s["received_messages"] for s in per_vm_stats.values())
    total_dropped = sum(s["dropped_messages"] for s in per_vm_stats.values())

    return {
        "total_vms": total_vms,
        "num_vms": completed_vms,
        "avg_latency_usec": avg_latency,
        "avg_p50_usec": avg_p50,
        "avg_p90_usec": avg_p90,
        "avg_p99_usec": avg_p99,
        "avg_p99_9_usec": avg_p99_9,
        "total_sent_messages": total_sent,
        "total_received_messages": total_received,
        "total_dropped_messages": total_dropped,
        "drop_rate_percent": (total_dropped / total_sent * 100)
        if total_sent > 0
        else 0,
    }


def plot_latency_percentiles(per_vm_stats: Dict[str, dict], output_path: Path):
    """plot p50, p90, p99 latency for all VMs as dot plot"""
    fig, ax = plt.subplots(figsize=(14, 8))

    vms = sorted(per_vm_stats.keys(), key=lambda x: int(x[2:]))  # Sort by VM number
    vm_indices = np.arange(len(vms))

    # extract percentile data
    p50_values = [per_vm_stats[vm]["p50_usec"] for vm in vms]
    p90_values = [per_vm_stats[vm]["p90_usec"] for vm in vms]
    p99_values = [per_vm_stats[vm]["p99_usec"] for vm in vms]
    p99_9_values = [per_vm_stats[vm]["p99_9_usec"] for vm in vms]

    # plot as scatter with different markers
    ax.scatter(
        vm_indices,
        p50_values,
        marker="o",
        s=100,
        alpha=0.7,
        label="p50",
        color="green",
        zorder=3,
    )
    ax.scatter(
        vm_indices,
        p90_values,
        marker="s",
        s=100,
        alpha=0.7,
        label="p90",
        color="blue",
        zorder=3,
    )
    ax.scatter(
        vm_indices,
        p99_values,
        marker="^",
        s=100,
        alpha=0.7,
        label="p99",
        color="orange",
        zorder=3,
    )
    ax.scatter(
        vm_indices,
        p99_9_values,
        marker="D",
        s=80,
        alpha=0.7,
        label="p99.9",
        color="red",
        zorder=3,
    )

    ax.axhline(
        np.mean(p50_values), color="green", linestyle="--", alpha=0.3, linewidth=1
    )
    ax.axhline(
        np.mean(p90_values), color="blue", linestyle="--", alpha=0.3, linewidth=1
    )
    ax.axhline(
        np.mean(p99_values), color="orange", linestyle="--", alpha=0.3, linewidth=1
    )
    ax.axhline(
        np.mean(p99_9_values), color="red", linestyle="--", alpha=0.3, linewidth=1
    )

    ax.set_xlabel("VM", fontsize=12, fontweight="bold")
    ax.set_ylabel("Latency (μs)", fontsize=12, fontweight="bold")
    ax.set_title("Latency Percentiles Per VM", fontsize=14, fontweight="bold")
    ax.set_xticks(vm_indices)
    ax.set_xticklabels(vms, rotation=45, ha="right")
    ax.legend(loc="best", fontsize=10)
    ax.grid(True, alpha=0.3, axis="y")

    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"saved plot: {output_path}")


def plot_latency_bar_chart(per_vm_stats: Dict[str, dict], output_path: Path):
    """plot average latency per VM as bar chart"""
    plt.figure(figsize=(14, 6))

    vms = sorted(per_vm_stats.keys(), key=lambda x: int(x[2:]))
    avg_latencies = [per_vm_stats[vm]["avg_latency_usec"] for vm in vms]

    bars = plt.bar(vms, avg_latencies, color="steelblue", alpha=0.8)

    # add value labels on bars
    for bar in bars:
        height = bar.get_height()
        plt.text(
            bar.get_x() + bar.get_width() / 2.0,
            height,
            f"{height:.1f}",
            ha="center",
            va="bottom",
            fontsize=8,
        )

    plt.xlabel("VM", fontsize=12, fontweight="bold")
    plt.ylabel("Average Latency (μs)", fontsize=12, fontweight="bold")
    plt.title("Average Latency Per VM", fontsize=14, fontweight="bold")
    plt.xticks(rotation=45, ha="right")
    plt.grid(True, alpha=0.3, axis="y")
    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"saved plot: {output_path}")


def plot_message_stats(per_vm_stats: Dict[str, dict], output_path: Path):
    """plot message statistics per VM"""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(16, 6))

    vms = sorted(per_vm_stats.keys(), key=lambda x: int(x[2:]))
    sent = [per_vm_stats[vm]["sent_messages"] for vm in vms]
    received = [per_vm_stats[vm]["received_messages"] for vm in vms]
    dropped = [per_vm_stats[vm]["dropped_messages"] for vm in vms]

    x = np.arange(len(vms))
    width = 0.35

    # plot sent vs received
    ax1.bar(x - width / 2, sent, width, label="Sent", color="steelblue", alpha=0.8)
    ax1.bar(x + width / 2, received, width, label="Received", color="green", alpha=0.8)
    ax1.set_xlabel("VM", fontsize=12, fontweight="bold")
    ax1.set_ylabel("Messages", fontsize=12, fontweight="bold")
    ax1.set_title("Messages Sent vs Received", fontsize=13, fontweight="bold")
    ax1.set_xticks(x)
    ax1.set_xticklabels(vms, rotation=45, ha="right")
    ax1.legend()
    ax1.grid(True, alpha=0.3, axis="y")

    # plot dropped messages
    bars = ax2.bar(vms, dropped, color="red", alpha=0.7)
    for bar in bars:
        height = bar.get_height()
        if height > 0:
            ax2.text(
                bar.get_x() + bar.get_width() / 2.0,
                height,
                f"{int(height)}",
                ha="center",
                va="bottom",
                fontsize=8,
            )

    ax2.set_xlabel("VM", fontsize=12, fontweight="bold")
    ax2.set_ylabel("Dropped Messages", fontsize=12, fontweight="bold")
    ax2.set_title("Dropped Messages Per VM", fontsize=13, fontweight="bold")
    ax2.set_xticks(range(len(vms)))
    ax2.set_xticklabels(vms, rotation=45, ha="right")
    ax2.grid(True, alpha=0.3, axis="y")

    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"saved plot: {output_path}")

def generate_json_report(overall_stats: dict, output_path: Path):
    with open(output_path, "w") as f:
        json.dump(overall_stats, f, indent=4)
    print(f"saved report: {output_path}")

def process_sockperf_results(logs_dir: Path, reports_dir: Path, mode: str):
    """process sockperf results and generate reports"""
    process_all_vms = mode == "vm-client"

    print("loading results...")
    print(
        f"   Mode: {mode} ({'processing all VMs' if process_all_vms else 'processing odd VMs only'})"
    )
    results, total_vms = load_results(logs_dir, process_all_vms=process_all_vms)
    print(f"   Found {total_vms} total VM log files")
    print(f"   Successfully processed {len(results)} VM results")

    if not results:
        print("❌ No results found!")
        return

    print("extracting statistics...")
    per_vm_stats = extract_per_vm_stats(results)
    overall_stats = calculate_overall_stats(per_vm_stats, total_vms)

    print("generating plots...")
    plot_latency_percentiles(per_vm_stats, reports_dir / "latency_percentiles.png")
    plot_latency_bar_chart(per_vm_stats, reports_dir / "latency_avg_per_vm.png")
    plot_message_stats(per_vm_stats, reports_dir / "message_stats.png")

    print("generating json report...")
    generate_json_report(overall_stats, reports_dir / "report.json")

    print(f"✅ All reports saved to: {reports_dir}/")
