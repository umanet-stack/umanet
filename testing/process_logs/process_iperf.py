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


def is_udp_mode(log_content: str) -> bool:
    return "Lost/Total Datagrams" in log_content or "Lost/Total" in log_content


def parse_udp_results(log_content: str) -> dict:
    """
    parses lines like:
    [  5]   0.00-1.00   sec  4.28 MBytes  35.9 Mbits/sec  0.001 ms  10351/80540 (13%)  sender
    [SUM]   0.00-10.01  sec   176 MBytes   148 Mbits/sec  0.001 ms  627759/3518290 (18%)  receiver

    """
    result = {"end": {}, "intervals": []}

    # \w = alphanumeric character (no spaces/symbols)
    # \s = whitespace, \S = non-whitespace
    # format:  [   31.756220] start-test.sh[1343]: [ID]   start-end   sec  Transfer     Bitrate         Jitter    Lost/Total (percent) [optional receiver/sender]
    # e.g.   : [   31.756345] start-test.sh[1343]: [  9]  0.00-30.00  sec  5.56 GBytes  1.59 Gbits/sec  0.000 ms  0/4058628  (0%)       sender
    interval_pattern = r"(?:\[.*?\]\s+\S+\[.*?\]:\s+)?\[\s*(\w+)\]\s+([\d.]+)-([\d.]+)\s+sec\s+([\d.]+)\s+(\w+)\s+([\d.]+)\s+(\w+)(?:/sec|/s)\s+([\d.]+)\s+ms\s+(\d+)/(\d+)\s+\(([\d.]+)%\)(?:\s+(receiver|sender))?"
    # (?:\[.*?\]\s+\S+\[.*?\]:\s+)? = optional prefix: [timestamp] start-test.sh[pid]:
    #     (?: ) = group these characters so i can make them optional, but do NOT save the text
    # \[\s*(\w+)\] = id (group 1)
    # \s+([\d.]+)-([\d.]+)\s+sec = start-end sec (group 2 and 3)
    # \s+([\d.]+)\s+(\w+) = transfer (group 4 and 5)
    # \s+([\d.]+)\s+(\w+)(?:/sec|/s) = bitrate (group 6 and 7)
    # \s+([\d.]+)\s+ms = jitter (group 8)
    # \s+(\d+)/(\d+) = lost/total (group 9 and 10)
    # \s+\(([\d.]+)%\) = percent (group 11)
    # (?:\s+(receiver|sender))? = optional receiver/sender (group 12)

    def convert_units(transfer_value, transfer_unit, bitrate_value, bitrate_unit):
        if transfer_unit == "MBytes":
            bytes_transferred = transfer_value * 1e6
        elif transfer_unit == "GBytes":
            bytes_transferred = transfer_value * 1e9
        elif transfer_unit == "KBytes":
            bytes_transferred = transfer_value * 1e3
        elif transfer_unit == "Bytes":
            bytes_transferred = transfer_value
        else:
            bytes_transferred = transfer_value

        # bitrate to bits per second
        if bitrate_unit in ["Gbits", "Gbits/sec", "Gbits/s"]:
            bits_per_second = bitrate_value * 1e9
        elif bitrate_unit in ["Mbits", "Mbits/sec", "Mbits/s"]:
            bits_per_second = bitrate_value * 1e6
        elif bitrate_unit in ["Kbits", "Kbits/sec", "Kbits/s"]:
            bits_per_second = bitrate_value * 1e3
        elif bitrate_unit in ["bits", "bits/sec", "bits/s"]:
            bits_per_second = bitrate_value
        else:
            # assume bits per second
            bits_per_second = bitrate_value

        return bytes_transferred, bits_per_second

    all_matches = list(re.finditer(interval_pattern, log_content))

    if not all_matches:
        return result

    # find summary line: prioritize receiver over sender, prefer [SUM] over individual streams
    summary_match = None
    receiver_sum_match = None
    sender_sum_match = None

    for match in all_matches:
        stream_id = match.group(1)
        # optional receiver/sender suffix (returns None if not matched)
        role = match.group(12)

        if stream_id == "SUM":
            if role == "receiver":
                receiver_sum_match = match
            elif role == "sender":
                sender_sum_match = match
            elif summary_match is None:  # SUM without role designation
                summary_match = match

    if receiver_sum_match:
        summary_match = receiver_sum_match
    elif sender_sum_match:
        summary_match = sender_sum_match
    elif not summary_match and all_matches:
        # find match with longest duration as summary if no SUM found
        summary_match = max(
            all_matches, key=lambda m: float(m.group(3)) - float(m.group(2))
        )

    # parse summary
    if summary_match:
        interval_start = float(summary_match.group(2))
        interval_end = float(summary_match.group(3))
        duration = interval_end - interval_start

        transfer_value = float(summary_match.group(4))
        transfer_unit = summary_match.group(5)
        bitrate_value = float(summary_match.group(6))
        bitrate_unit = summary_match.group(7)
        jitter_ms = float(summary_match.group(8))
        lost_datagrams = int(summary_match.group(9))
        total_datagrams = int(summary_match.group(10))
        loss_percent = float(summary_match.group(11))

        bytes_transferred, _ = convert_units(
            transfer_value,
            transfer_unit,
            0,
            "",  # We'll calculate throughput from bytes
        )

        # calculate actual throughput from bytes received and duration
        # This is more accurate than using the bitrate from log, which may be estimated/sending rate
        bits_per_second = (bytes_transferred * 8) / duration if duration > 0 else 0

        # calculate PPS (packets per second)
        # Sender PPS = total datagrams / duration (what was sent)
        # Receiver PPS = (total - lost) / duration (what was actually received)
        received_datagrams = total_datagrams - lost_datagrams
        sender_pps = total_datagrams / duration if duration > 0 else 0
        receiver_pps = received_datagrams / duration if duration > 0 else 0

        result["end"] = {
            "sum_received": {
                "bits_per_second": bits_per_second,
                "bytes": bytes_transferred,
                "seconds": duration,
                "jitter_ms": jitter_ms,
                "lost_datagrams": lost_datagrams,
                "total_datagrams": total_datagrams,
                "received_datagrams": received_datagrams,
                "loss_percent": loss_percent,
                "sender_pps": sender_pps,
                "receiver_pps": receiver_pps,
            },
        }

    # extract all interval data (including SUM intervals for time series)
    seen_intervals = set()
    for match in all_matches:
        # create unique key for interval
        interval_key = (float(match.group(2)), float(match.group(3)))
        if interval_key in seen_intervals:
            continue
        seen_intervals.add(interval_key)

        interval_start = float(match.group(2))
        interval_end = float(match.group(3))
        transfer_value = float(match.group(4))
        transfer_unit = match.group(5)
        jitter_ms = float(match.group(8))
        lost_datagrams = int(match.group(9))
        total_datagrams = int(match.group(10))

        bytes_transferred, _ = convert_units(
            transfer_value, # 5.58 
            transfer_unit, # GBytes
            0, # 1.60 
            "",  # Gbits/sec, calculate throughput from bytes
        )

        interval_duration = interval_end - interval_start
        # calculate actual throughput from bytes transferred and duration
        # ensures consistency between bytes and throughput
        bits_per_second = (
            (bytes_transferred * 8) / interval_duration if interval_duration > 0 else 0
        )
        received_datagrams = total_datagrams - lost_datagrams
        sender_pps = total_datagrams / interval_duration if interval_duration > 0 else 0
        receiver_pps = (
            received_datagrams / interval_duration if interval_duration > 0 else 0
        )

        result["intervals"].append(
            {
                "sum": {
                    "start": interval_start,
                    "end": interval_end,
                    "bits_per_second": bits_per_second,
                    "bytes": bytes_transferred,
                    "jitter_ms": jitter_ms,
                    "lost_datagrams": lost_datagrams,
                    "total_datagrams": total_datagrams,
                    "received_datagrams": received_datagrams,
                    "sender_pps": sender_pps,
                    "receiver_pps": receiver_pps,
                }
            }
        )

    # sort intervals by start time
    result["intervals"].sort(key=lambda x: x["sum"]["start"])

    return result


def load_results(
    logs_dir: Path, process_all_vms: bool = False
) -> Tuple[Dict[str, dict], int]:
    """
    extract results from VM log files
    """
    results = {}
    total_vms = 0

    for log_file in sorted(logs_dir.glob("vm*.log")):
        vm_name = log_file.stem  # "vm1"
        vm_num = int(vm_name[2:])  # "vm1" -> 1

        # process_all_vms = False -> only count odd ones
        if process_all_vms or vm_num % 2 == 1:
            total_vms += 1

        # process_all_vms = False -> skip even ones
        if not process_all_vms and vm_num % 2 == 0:
            continue

        try:
            with open(log_file, "r") as f:
                log_content = f.read()

            if is_udp_mode(log_content):
                udp_result = parse_udp_results(log_content)
                if udp_result.get("end") and udp_result["end"].get("sum_received"):
                    # validate UDP data
                    sum_recv = udp_result["end"]["sum_received"]
                    if sum_recv.get("bits_per_second", 0) > 0:
                        results[vm_name] = udp_result
                    else:
                        print(f"⚠️  skipping {vm_name}: invalid UDP data")
                else:
                    print(f"⚠️  skipping {vm_name}: could not parse UDP results")
                continue

            # format: [timestamp] start-iperf.sh[pid]: [date time] vm:   Throughput: X Gbps | Bytes: X GB | Retransmits: X | CPU (host): X% | CPU (remote): X%
            # script name can be start-iperf.sh or start-test.sh
            # \[.*?\] -> stops at the first closing bracket it finds (.*? is non-greedy)
            # ([\d.]+) -> parentheses capture digits + dots
            # \s+ -> one or more spaces
            pattern = r"\[.*?\] start-(?:iperf|test)\.sh\[.*?\]: \[.*?\] vm:\s+Throughput:\s+([\d.]+)\s+Gbps\s+\|\s+Bytes:\s+([\d.]+)\s+GB\s+\|\s+Retransmits:\s+(\d+)\s+\|\s+CPU\s+\(host\):\s+([\d.]+)%\s+\|\s+CPU\s+\(remote\):\s+([\d.]+)%"
            match = re.search(pattern, log_content)

            if match:
                throughput_gbps = float(match.group(1))
                bytes_gb = float(match.group(2))
                retransmits = int(match.group(3))
                cpu_host = float(match.group(4))
                cpu_remote = float(match.group(5))

                # skip VMs with invalid data 
                if throughput_gbps <= 0 or bytes_gb <= 0:
                    print(
                        f"⚠️  skipping {vm_name}: invalid data (throughput={throughput_gbps} Gbps, bytes={bytes_gb} GB)"
                    )
                    continue

                results[vm_name] = {
                    "end": {
                        "sum_sent": {
                            "bits_per_second": throughput_gbps * 1e9,
                            "bytes": bytes_gb * 1e9,
                            "retransmits": retransmits,
                            "seconds": 30.0,  # default test duration
                        },
                        "cpu_utilization_percent": {
                            "host_total": cpu_host,
                            "remote_total": cpu_remote,
                        },
                    },
                }
            else:
                print(f"⚠️  skipping {vm_name}: can't find summary line in {log_file}")
        except Exception as e:
            print(f"⚠️  error processing {log_file}: {e}")

    return results, total_vms


def extract_per_vm_stats(results: Dict[str, dict]) -> Dict[str, dict]:
    """
    extract throughput and CPU stats for each VM
    """
    stats = {}

    for vm_name, data in results.items():
        try:
            end_data = data.get("end", {})
            cpu = end_data.get("cpu_utilization_percent", {})

            # check if UDP mode (has sum_received with UDP-specific fields)
            sum_received = end_data.get("sum_received", {})
            if sum_received and "jitter_ms" in sum_received:
                # udp mode
                stats[vm_name] = {
                    "mode": "udp",
                    "throughput_gbps": sum_received.get("bits_per_second", 0) / 1e9,
                    "bytes_received": sum_received.get("bytes", 0),
                    "duration_sec": sum_received.get("seconds", 0),
                    "jitter_ms": sum_received.get("jitter_ms", 0),
                    "lost_datagrams": sum_received.get("lost_datagrams", 0),
                    "total_datagrams": sum_received.get("total_datagrams", 0),
                    "received_datagrams": sum_received.get(
                        "received_datagrams",
                        sum_received.get("total_datagrams", 0)
                        - sum_received.get("lost_datagrams", 0),
                    ),
                    "loss_percent": sum_received.get("loss_percent", 0),
                    "sender_pps": sum_received.get("sender_pps", 0),
                    "receiver_pps": sum_received.get("receiver_pps", 0),
                    "cpu_host_total": cpu.get("host_total", 0),
                    "cpu_remote_total": cpu.get("remote_total", 0),
                }
            else:
                # tcp mode
                sum_sent = end_data.get("sum_sent", {})
                stats[vm_name] = {
                    "mode": "tcp",
                    "throughput_gbps": sum_sent.get("bits_per_second", 0) / 1e9,
                    "bytes_sent": sum_sent.get("bytes", 0),
                    "duration_sec": sum_sent.get("seconds", 0),
                    "retransmits": sum_sent.get("retransmits", 0),
                    "cpu_host_total": cpu.get("host_total", 0),
                    "cpu_remote_total": cpu.get("remote_total", 0),
                }
        except Exception as e:
            print(f"⚠️  Error processing {vm_name}: {e}")

    return stats


def extract_timeseries(results: Dict[str, dict]) -> Dict[str, List[dict]]:
    """
    extract time-series data from intervals (handles both TCP and UDP modes)
    """
    timeseries = {}

    for vm_name, data in results.items():
        intervals = data.get("intervals", [])
        ts_data = []

        for interval in intervals:
            sum_data = interval.get("sum", {})
            entry = {
                "start": sum_data.get("start", 0),
                "end": sum_data.get("end", 0),
                "throughput_gbps": sum_data.get("bits_per_second", 0) / 1e9,
                "bytes": sum_data.get("bytes", 0),
            }

            # add UDP-specific fields if present
            if "jitter_ms" in sum_data:
                entry["jitter_ms"] = sum_data.get("jitter_ms", 0)
                entry["lost_datagrams"] = sum_data.get("lost_datagrams", 0)
                entry["total_datagrams"] = sum_data.get("total_datagrams", 0)
                entry["received_datagrams"] = sum_data.get("received_datagrams", 0)
                entry["sender_pps"] = sum_data.get("sender_pps", 0)
                entry["receiver_pps"] = sum_data.get("receiver_pps", 0)

            ts_data.append(entry)

        timeseries[vm_name] = ts_data

    return timeseries


def calculate_overall_stats(per_vm_stats: Dict[str, dict], total_vms: int) -> dict:
    if not per_vm_stats:
        return {}

    # detect mode from first VM
    first_vm = list(per_vm_stats.values())[0]
    is_udp = first_vm.get("mode") == "udp"

    completed_vms = len(per_vm_stats)
    total_throughput_gbps = sum(s["throughput_gbps"] for s in per_vm_stats.values())
    avg_cpu_host = np.mean([s["cpu_host_total"] for s in per_vm_stats.values()])
    avg_cpu_remote = np.mean([s["cpu_remote_total"] for s in per_vm_stats.values()])

    stats = {
        "mode": "udp" if is_udp else "tcp",
        "total_throughput_gbps": total_throughput_gbps,
        "total_vms": total_vms,
        "num_vms": completed_vms,
        "avg_per_vm_gbps": total_throughput_gbps / completed_vms
        if completed_vms > 0
        else 0,
        "avg_cpu_host_percent": avg_cpu_host,
        "avg_cpu_remote_percent": avg_cpu_remote,
    }

    if is_udp:
        total_bytes = sum(s.get("bytes_received", 0) for s in per_vm_stats.values())
        total_lost = sum(s.get("lost_datagrams", 0) for s in per_vm_stats.values())
        total_datagrams = sum(
            s.get("total_datagrams", 0) for s in per_vm_stats.values()
        )
        total_received_datagrams = sum(
            s.get("received_datagrams", 0) for s in per_vm_stats.values()
        )
        total_sender_pps = sum(s.get("sender_pps", 0) for s in per_vm_stats.values())
        total_receiver_pps = sum(
            s.get("receiver_pps", 0) for s in per_vm_stats.values()
        )
        avg_jitter = np.mean([s.get("jitter_ms", 0) for s in per_vm_stats.values()])

        stats.update(
            {
                "total_bytes_received_gb": total_bytes / 1e9,
                "total_lost_datagrams": total_lost,
                "total_datagrams": total_datagrams,
                "total_received_datagrams": total_received_datagrams,
                "overall_loss_percent": (total_lost / total_datagrams * 100)
                if total_datagrams > 0
                else 0,
                "total_sender_pps": total_sender_pps,
                "avg_sender_pps_per_vm": total_sender_pps / completed_vms
                if completed_vms > 0
                else 0,
                "total_receiver_pps": total_receiver_pps,
                "avg_receiver_pps_per_vm": total_receiver_pps / completed_vms
                if completed_vms > 0
                else 0,
                "avg_jitter_ms": avg_jitter,
            }
        )
    else:
        # tcp-specific stats
        total_bytes = sum(s.get("bytes_sent", 0) for s in per_vm_stats.values())
        total_retransmits = sum(s.get("retransmits", 0) for s in per_vm_stats.values())

        stats.update(
            {
                "total_bytes_sent_gb": total_bytes / 1e9,
                "total_retransmits": total_retransmits,
            }
        )

    return stats


def plot_throughput_timeseries(timeseries: Dict[str, List[dict]], output_path: Path):
    """
    plot throughput time series for all VMs
    """
    plt.figure(figsize=(14, 8))

    # plot individual VMs
    for vm_name, ts_data in sorted(timeseries.items()):
        times = [d["end"] for d in ts_data]
        throughputs = [d["throughput_gbps"] for d in ts_data]
        plt.plot(times, throughputs, label=vm_name, alpha=0.7, linewidth=1)

    # calculate and plot average
    if timeseries:
        # use the first VM's time values as reference
        first_vm_ts = list(timeseries.values())[0]
        reference_times = [d["end"] for d in first_vm_ts]

        max_len = max(len(ts) for ts in timeseries.values())
        avg_throughput = []
        avg_times = []

        for i in range(max_len):
            values = []
            for ts_data in timeseries.values():
                if i < len(ts_data):
                    values.append(ts_data[i]["throughput_gbps"])
            if values:
                avg_throughput.append(np.mean(values))
                # use time from first VM (or average if multiple VMs have different times)
                if i < len(reference_times):
                    avg_times.append(reference_times[i])
                else:
                    avg_times.append(i)  # fallback to index

        if avg_times and avg_throughput:
            plt.plot(
                avg_times, avg_throughput, "k-", linewidth=3, label="Average", alpha=0.9
            )

    plt.xlabel("Time (seconds)", fontsize=12)
    plt.ylabel("Throughput (Gbps)", fontsize=12)
    plt.title("Throughput Over Time - All VMs", fontsize=14, fontweight="bold")
    plt.legend(loc="best", fontsize=8, ncol=2)
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"saved plot: {output_path}")


def plot_per_vm_throughput(per_vm_stats: Dict[str, dict], output_path: Path):
    """
    plot throughput per VM as bar chart
    """
    plt.figure(figsize=(12, 6))

    vms = sorted(per_vm_stats.keys(), key=lambda x: int(x[2:]))  # sort by VM number
    throughputs = [per_vm_stats[vm]["throughput_gbps"] for vm in vms]

    bars = plt.bar(vms, throughputs, color="steelblue", alpha=0.8)

    # add value labels on bars
    for bar in bars:
        height = bar.get_height()
        plt.text(
            bar.get_x() + bar.get_width() / 2.0,
            height,
            f"{height:.2f}",
            ha="center",
            va="bottom",
            fontsize=9,
        )

    plt.xlabel("VM", fontsize=12)
    plt.ylabel("Throughput (Gbps)", fontsize=12)
    plt.title("Throughput Per VM", fontsize=14, fontweight="bold")
    plt.xticks(rotation=45)
    plt.grid(True, alpha=0.3, axis="y")
    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"saved plot: {output_path}")


def plot_cpu_utilization(per_vm_stats: Dict[str, dict], output_path: Path):
    """
    plot CPU utilization per VM
    """
    plt.figure(figsize=(12, 6))

    vms = sorted(per_vm_stats.keys(), key=lambda x: int(x[2:]))
    cpu_host = [per_vm_stats[vm]["cpu_host_total"] for vm in vms]
    cpu_remote = [per_vm_stats[vm]["cpu_remote_total"] for vm in vms]

    x = np.arange(len(vms))
    width = 0.35

    plt.bar(
        x - width / 2, cpu_host, width, label="Client (Host)", color="coral", alpha=0.8
    )
    plt.bar(
        x + width / 2,
        cpu_remote,
        width,
        label="Server (Remote)",
        color="steelblue",
        alpha=0.8,
    )

    plt.xlabel("VM", fontsize=12)
    plt.ylabel("CPU Utilization (%)", fontsize=12)
    plt.title("CPU Utilization Per VM", fontsize=14, fontweight="bold")
    plt.xticks(x, vms, rotation=45)
    plt.legend()
    plt.grid(True, alpha=0.3, axis="y")
    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close()
    print(f"saved plot: {output_path}")

def generate_json_report(overall_stats: dict, output_path: Path):
    with open(output_path, "w") as f:
        json.dump(overall_stats, f, indent=4)
    print(f"saved report: {output_path}")


def process_iperf_results(logs_dir: Path, reports_dir: Path, mode: str):
    """
    process iperf3 results and generate reports
    """
    process_all_vms = mode == "vm-client"

    print("loading results...")
    print(
        f"   Mode: {mode} ({'processing all VMs' if process_all_vms else 'processing odd VMs only'})"
    )
    results, total_vms = load_results(logs_dir, process_all_vms=process_all_vms)
    print(f"   Found {total_vms} total VM log files")
    print(f"   Successfully processed {len(results)} VM results\n")

    if not results:
        print("❌ No results found!")
        return

    print("extracting statistics...")
    per_vm_stats = extract_per_vm_stats(results)
    overall_stats = calculate_overall_stats(per_vm_stats, total_vms)
    timeseries = extract_timeseries(results)

    print("generating plots...")
    plot_throughput_timeseries(timeseries, reports_dir / "throughput_timeseries.png")
    plot_per_vm_throughput(per_vm_stats, reports_dir / "throughput_per_vm.png")
    plot_cpu_utilization(per_vm_stats, reports_dir / "cpu_utilization.png")

    print("generating json report...")
    generate_json_report(overall_stats, reports_dir / "report.json")

    print(f"✅ All reports saved to: {reports_dir}/")
