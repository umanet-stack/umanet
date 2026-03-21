#!/usr/bin/env python3
"""
Plot FaaS latency data from aggregated.csv
Creates two plots: mean and median of p50 and p90 latency
"""

import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt

# Get script directory
SCRIPT_DIR = Path(__file__).parent.resolve()
CSV_FILE = SCRIPT_DIR / "aggregated.csv"

# Color scheme
# UMANet: p50 green, p90 red
COLOR_UMANET = "#2ca02c"  # green
# Linux: p50 blue, p90 orange
COLOR_LINUX = "#1f77b4"  # blue
# OVS-DPDK: p50 purple, p90 brown
COLOR_OVS_DPDK = "#9467bd"  # purple

# System name mapping
NAME_MAPPING = {"dpdk32vm": "UMANet", "nginx32vm": "Linux", "ovsdpdk32vm": "OVS-DPDK"}

# Color mapping by system and percentile
COLOR_MAP = {
    "UMANet": {"0.5": COLOR_UMANET, "0.9": COLOR_UMANET},
    "Linux": {"0.5": COLOR_LINUX, "0.9": COLOR_LINUX},
    "OVS-DPDK": {"0.5": COLOR_OVS_DPDK, "0.9": COLOR_OVS_DPDK},
}


def load_data():
    """Load and prepare data from CSV"""
    data = []
    with open(CSV_FILE, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
            # Map system name
            system = NAME_MAPPING.get(row["name"], row["name"])

            data.append(
                {
                    "system": system,
                    "R": float(row["R"]),
                    "p": row["p"],
                    "type": row["type"],
                    "value": float(row["value"]),
                }
            )

    return data


def plot_latency(plot_type="mean"):
    """
    Plot latency vs requests/s

    Args:
        plot_type: 'mean' or 'median'
    """
    data = load_data()

    # Filter by plot type
    filtered_data = [d for d in data if d["type"] == plot_type]

    # Create figure with 2 subplots stacked vertically
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 5), sharex=True)

    # Systems to plot
    systems = ["UMANet", "Linux", "OVS-DPDK"]

    # Organize data by system and percentile
    plot_data = defaultdict(lambda: defaultdict(list))

    for d in filtered_data:
        if d["system"] in systems and d["p"] in ["0.5", "0.9"]:
            plot_data[d["system"]][d["p"]].append((d["R"], d["value"]))

    # Plot p50 on top subplot
    for system in systems:
        if "0.5" not in plot_data[system]:
            continue

        points = sorted(plot_data[system]["0.5"], key=lambda x: x[0])
        if len(points) == 0:
            continue

        R_values = [p[0] for p in points]
        values = [p[1] for p in points]
        color = COLOR_MAP[system]["0.5"]
        label = system

        ax1.plot(
            R_values,
            values,
            "o-",
            label=label,
            linewidth=2,
            markersize=5,
            color=color,
        )

    # Plot p90 on bottom subplot
    for system in systems:
        if "0.9" not in plot_data[system]:
            continue

        points = sorted(plot_data[system]["0.9"], key=lambda x: x[0])
        if len(points) == 0:
            continue

        R_values = [p[0] for p in points]
        values = [p[1] for p in points]
        color = COLOR_MAP[system]["0.9"]
        label = system

        ax2.plot(
            R_values,
            values,
            "o-",
            label=label,
            linewidth=2,
            markersize=5,
            color=color,
        )

    # Configure top subplot (p50)
    ax1.set_ylabel("p50 Latency (ms)", fontsize=16)
    ax1.tick_params(axis="both", which="major", labelsize=16)
    ax1.grid(True, alpha=0.3)
    ax1.set_ylim(0, 10)

    # Configure bottom subplot (p90)
    ax2.set_xlabel("Requests/s", fontsize=16)
    ax2.set_ylabel("p90 Latency (ms)", fontsize=16)
    ax2.tick_params(axis="both", which="major", labelsize=16)
    ax2.grid(True, alpha=0.3)
    ax2.set_ylim(0, 10)

    # Share legend - place it on the top subplot
    ax1.legend(fontsize=15, loc="best", ncol=3)

    plt.tight_layout()

    # Save plots
    output_base = SCRIPT_DIR / f"faas_latency_{plot_type}"
    output_png = output_base.with_suffix(".png")
    output_pdf = output_base.with_suffix(".pdf")

    plt.savefig(output_png, dpi=300, bbox_inches="tight")
    plt.savefig(output_pdf, bbox_inches="tight")
    print(f"Saved: {output_png}")
    print(f"Saved: {output_pdf}")

    plt.close()


def main():
    """Main function"""
    print("📊 Plotting FaaS latency data...")
    print(f"📁 CSV file: {CSV_FILE}")
    print()

    # Plot 1: Mean latency
    print("Creating plot 1: Mean latency...")
    plot_latency("mean")
    print()

    # Plot 2: Median latency
    print("Creating plot 2: Median latency...")
    plot_latency("median")
    print()

    print("✅ Done!")


if __name__ == "__main__":
    main()
