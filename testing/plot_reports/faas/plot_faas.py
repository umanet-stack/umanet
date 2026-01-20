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
COLOR_UMANET_P50 = "#2ca02c"  # green
COLOR_UMANET_P90 = "#d62728"  # red
# Linux: p50 blue, p90 orange
COLOR_LINUX_P50 = "#1f77b4"  # blue
COLOR_LINUX_P90 = "#ff7f0e"  # orange
# OVS-DPDK: p50 purple, p90 brown
COLOR_OVS_DPDK_P50 = "#9467bd"  # purple
COLOR_OVS_DPDK_P90 = "#8c564b"  # brown

# System name mapping
NAME_MAPPING = {"dpdk32vm": "UMANet", "nginx32vm": "Linux", "ovsdpdk32vm": "OVS-DPDK"}

# Color mapping by system and percentile
COLOR_MAP = {
    "UMANet": {"0.5": COLOR_UMANET_P50, "0.9": COLOR_UMANET_P90},
    "Linux": {"0.5": COLOR_LINUX_P50, "0.9": COLOR_LINUX_P90},
    "OVS-DPDK": {"0.5": COLOR_OVS_DPDK_P50, "0.9": COLOR_OVS_DPDK_P90},
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

    # Create figure
    fig, ax = plt.subplots(1, 1, figsize=(10, 6))

    # Systems to plot
    systems = ["UMANet", "Linux", "OVS-DPDK"]
    percentiles = ["0.5", "0.9"]

    # Organize data by system and percentile
    plot_data = defaultdict(lambda: defaultdict(list))

    for d in filtered_data:
        if d["system"] in systems and d["p"] in percentiles:
            plot_data[d["system"]][d["p"]].append((d["R"], d["value"]))

    # Plot each system and percentile combination
    for system in systems:
        for p in percentiles:
            if p not in plot_data[system]:
                continue

            # Get data points and sort by R
            points = sorted(plot_data[system][p], key=lambda x: x[0])

            if len(points) == 0:
                continue

            R_values = [p[0] for p in points]
            values = [p[1] for p in points]

            # Get color
            color = COLOR_MAP[system][p]

            # Create label
            p_label = "p50" if p == "0.5" else "p90"
            label = f"{system} ({p_label})"

            # Plot line
            ax.plot(
                R_values,
                values,
                "o-",
                label=label,
                linewidth=2,
                markersize=5,
                color=color,
            )

    # Set labels and title
    ax.set_xlabel("Requests/s", fontsize=18)
    ax.set_ylabel("Latency (ms)", fontsize=18)
    title = f"{plot_type.capitalize()} Latency: p50 and p90"
    ax.set_title(title, fontsize=20, fontweight="bold")

    # Formatting
    ax.tick_params(axis="both", which="major", labelsize=16)
    ax.legend(fontsize=12, loc="best", ncol=2)
    ax.grid(True, alpha=0.3)

    # Use log scale for y-axis if needed (check if values span large range)
    all_values = [d["value"] for d in filtered_data]
    if all_values and max(all_values) / min(all_values) > 100:
        ax.set_yscale("log")

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
