import csv
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt

plt.rcParams["pdf.fonttype"] = 42
plt.rcParams["ps.fonttype"] = 42

SCRIPT_DIR = Path(__file__).parent.resolve()
CSV_FILE = SCRIPT_DIR / "aggregated.csv"

COLOR_UMANET = "#2ca02c"
COLOR_LINUX = "#1f77b4"
COLOR_OVS_DPDK = "#9467bd"

NAME_MAPPING = {"dpdk32vm": "UMANet", "nginx32vm": "Linux", "ovsdpdk32vm": "OVS-DPDK"}

COLOR_MAP = {
    "UMANet": {"0.5": COLOR_UMANET, "0.9": COLOR_UMANET},
    "Linux": {"0.5": COLOR_LINUX, "0.9": COLOR_LINUX},
    "OVS-DPDK": {"0.5": COLOR_OVS_DPDK, "0.9": COLOR_OVS_DPDK},
}


def load_data():
    data = []
    with open(CSV_FILE, "r") as f:
        reader = csv.DictReader(f)
        for row in reader:
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
    data = load_data()
    filtered_data = [d for d in data if d["type"] == plot_type]

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 5), sharex=True)
    systems = ["Linux", "UMANet", "OVS-DPDK"]

    plot_data = defaultdict(lambda: defaultdict(list))

    for d in filtered_data:
        if d["system"] in systems and d["p"] in ["0.5", "0.9"]:
            plot_data[d["system"]][d["p"]].append((d["R"], d["value"]))

    # p50 on top subplot
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

    # p90 on bottom subplot
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

    ax1.set_ylabel("p50 Latency (ms)", fontsize=16)
    ax1.tick_params(axis="both", which="major", labelsize=16)
    ax1.grid(True, alpha=0.3)
    ax1.set_yscale("log")
    ax1.set_ylim(0, 200)

    ax2.set_xlabel("Requests/s", fontsize=16)
    ax2.set_ylabel("p90 Latency (ms)", fontsize=16)
    ax2.tick_params(axis="both", which="major", labelsize=16)
    ax2.grid(True, alpha=0.3)
    ax2.set_yscale("log")
    ax2.set_ylim(0, 200)

    handles, labels = ax1.get_legend_handles_labels()

    fig.legend(
        handles,
        labels,
        fontsize=15,
        loc="upper center",
        ncol=3,
        bbox_to_anchor=(0.5, 1.1),
    )

    plt.tight_layout(rect=[0, 0, 1, 1])  # make room for legend at top

    output_base = SCRIPT_DIR / f"faas_latency_{plot_type}"
    output_png = output_base.with_suffix(".png")
    output_pdf = output_base.with_suffix(".pdf")

    plt.savefig(output_png, dpi=300, bbox_inches="tight")
    plt.savefig(output_pdf, bbox_inches="tight")
    print(f"Saved: {output_png}")
    print(f"Saved: {output_pdf}")

    plt.close()


def main():
    print("Plotting mean latency...")
    plot_latency("mean")
    print()

    print("Plotting median latency...")
    plot_latency("median")
    print()

    print("done!")


if __name__ == "__main__":
    main()
