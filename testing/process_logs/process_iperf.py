#!/usr/bin/env python3
"""
Process iperf3 test results and generate reports
"""
import re
import argparse
from pathlib import Path
from typing import Dict, List
import matplotlib.pyplot as plt
import matplotlib
matplotlib.use('Agg')  # Non-interactive backend
import numpy as np
from datetime import datetime

# Get script directory to make paths relative to it
SCRIPT_DIR = Path(__file__).parent.resolve()


def load_results(logs_dir: Path, process_all_vms: bool = False) -> Dict[str, dict]:
    """Extract results from VM log files
    
    Args:
        logs_dir: Directory containing VM log files
        process_all_vms: If True, process all VMs. If False, process only odd-numbered VMs (clients).
    """
    results = {}
    
    # Find all VM log files
    for log_file in sorted(logs_dir.glob("vm*.log")):
        vm_name = log_file.stem  # e.g., "vm1"
        vm_num = int(vm_name[2:])  # Extract number: "vm1" -> 1
        
        # Only process odd-numbered VMs (clients) if process_all_vms is False
        if not process_all_vms and vm_num % 2 == 0:
            continue
        
        try:
            with open(log_file, 'r') as f:
                log_content = f.read()
            
            # Extract the summary line
            # Format: [timestamp] start-iperf.sh[pid]: [date time] vm:   Throughput: X Gbps | Bytes: X GB | Retransmits: X | CPU (host): X% | CPU (remote): X%
            # Note: Script name can be start-iperf.sh or start-test.sh depending on test setup
            pattern = r'\[.*?\] start-(?:iperf|test)\.sh\[.*?\]: \[.*?\] vm:\s+Throughput:\s+([\d.]+)\s+Gbps\s+\|\s+Bytes:\s+([\d.]+)\s+GB\s+\|\s+Retransmits:\s+(\d+)\s+\|\s+CPU\s+\(host\):\s+([\d.]+)%\s+\|\s+CPU\s+\(remote\):\s+([\d.]+)%'
            match = re.search(pattern, log_content)
            
            if match:
                throughput_gbps = float(match.group(1))
                bytes_gb = float(match.group(2))
                retransmits = int(match.group(3))
                cpu_host = float(match.group(4))
                cpu_remote = float(match.group(5))
                
                # Skip VMs with invalid data (0 throughput, negative values, etc.)
                if throughput_gbps <= 0 or bytes_gb <= 0:
                    print(f"⚠️  Skipping {vm_name}: Invalid data (throughput={throughput_gbps} Gbps, bytes={bytes_gb} GB)")
                    continue
                
                # Extract intervals from log text
                # Format: "[timestamp] start-iperf.sh[pid]:   [0-1.001431s] 10.13 Gbps"
                # Note: Script name can be start-iperf.sh or start-test.sh
                intervals = []
                # Find all interval lines after "iperf3 intervals"
                # Pattern matches lines with kernel timestamp prefix: "[timestamp] start-iperf.sh[pid]:   [0-1.001431s] 10.13 Gbps"
                # Look for lines that contain interval pattern after "iperf3 intervals"
                interval_section = re.search(r'iperf3 intervals.*?\n((?:\[.*?\] start-(?:iperf|test)\.sh\[.*?\]:\s+\[[\d.]+-[\d.]+s\]\s+[\d.]+\s+Gbps\n?)+)', log_content, re.MULTILINE)
                
                if interval_section:
                    interval_block = interval_section.group(1)
                    for line in interval_block.strip().split('\n'):
                        # Parse: "[timestamp] start-iperf.sh[pid]:   [0-1.001431s] 10.13 Gbps"
                        # Extract just the interval part: "[0-1.001431s] 10.13 Gbps"
                        line_match = re.search(r'\[([\d.]+)-([\d.]+)s\]\s+([\d.]+)\s+Gbps', line)
                        if line_match:
                            start = float(line_match.group(1))
                            end = float(line_match.group(2))
                            throughput_gbps_interval = float(line_match.group(3))
                            intervals.append({
                                'sum': {
                                    'start': start,
                                    'end': end,
                                    'bits_per_second': throughput_gbps_interval * 1e9,
                                    'bytes': throughput_gbps_interval * 1e9 * (end - start) / 8  # Approximate
                                }
                            })
                
                results[vm_name] = {
                    'end': {
                        'sum_sent': {
                            'bits_per_second': throughput_gbps * 1e9,
                            'bytes': bytes_gb * 1e9,
                            'retransmits': retransmits,
                            'seconds': 30.0  # Default test duration
                        },
                        'cpu_utilization_percent': {
                            'host_total': cpu_host,
                            'host_user': 0.0,  # Not available from summary line
                            'host_system': 0.0,
                            'remote_total': cpu_remote,
                            'remote_user': 0.0,
                            'remote_system': 0.0
                        }
                    },
                    'intervals': intervals if intervals else []
                }
            else:
                print(f"⚠️  Skipping {vm_name}: Could not find summary line in {log_file}")
        except Exception as e:
            print(f"⚠️  Error processing {log_file}: {e}")
    
    return results


def extract_per_vm_stats(results: Dict[str, dict]) -> Dict[str, dict]:
    """Extract throughput and CPU stats for each VM"""
    stats = {}
    
    for vm_name, data in results.items():
        try:
            end_data = data.get('end', {})
            sum_sent = end_data.get('sum_sent', {})
            cpu = end_data.get('cpu_utilization_percent', {})
            
            stats[vm_name] = {
                'throughput_bps': sum_sent.get('bits_per_second', 0),
                'throughput_gbps': sum_sent.get('bits_per_second', 0) / 1e9,
                'throughput_mbps': sum_sent.get('bits_per_second', 0) / 1e6,
                'bytes_sent': sum_sent.get('bytes', 0),
                'duration_sec': sum_sent.get('seconds', 0),
                'retransmits': sum_sent.get('retransmits', 0),
                'cpu_host_total': cpu.get('host_total', 0),
                'cpu_host_user': cpu.get('host_user', 0),
                'cpu_host_system': cpu.get('host_system', 0),
                'cpu_remote_total': cpu.get('remote_total', 0),
                'cpu_remote_user': cpu.get('remote_user', 0),
                'cpu_remote_system': cpu.get('remote_system', 0),
            }
        except Exception as e:
            print(f"⚠️  Error processing {vm_name}: {e}")
    
    return stats


def extract_timeseries(results: Dict[str, dict]) -> Dict[str, List[dict]]:
    """Extract time-series data from intervals"""
    timeseries = {}
    
    for vm_name, data in results.items():
        intervals = data.get('intervals', [])
        ts_data = []
        
        for interval in intervals:
            sum_data = interval.get('sum', {})
            ts_data.append({
                'start': sum_data.get('start', 0),
                'end': sum_data.get('end', 0),
                'throughput_bps': sum_data.get('bits_per_second', 0),
                'throughput_gbps': sum_data.get('bits_per_second', 0) / 1e9,
                'bytes': sum_data.get('bytes', 0),
            })
        
        timeseries[vm_name] = ts_data
    
    return timeseries


def calculate_overall_stats(per_vm_stats: Dict[str, dict]) -> dict:
    """Calculate overall statistics across all VMs"""
    if not per_vm_stats:
        return {}
    
    total_throughput_bps = sum(s['throughput_bps'] for s in per_vm_stats.values())
    total_bytes = sum(s['bytes_sent'] for s in per_vm_stats.values())
    avg_cpu_host = np.mean([s['cpu_host_total'] for s in per_vm_stats.values()])
    avg_cpu_remote = np.mean([s['cpu_remote_total'] for s in per_vm_stats.values()])
    total_retransmits = sum(s['retransmits'] for s in per_vm_stats.values())
    
    return {
        'total_throughput_gbps': total_throughput_bps / 1e9,
        'total_throughput_mbps': total_throughput_bps / 1e6,
        'total_bytes_sent_gb': total_bytes / 1e9,
        'num_vms': len(per_vm_stats),
        'avg_per_vm_gbps': (total_throughput_bps / 1e9) / len(per_vm_stats),
        'avg_cpu_host_percent': avg_cpu_host,
        'avg_cpu_remote_percent': avg_cpu_remote,
        'total_retransmits': total_retransmits,
    }


def plot_throughput_timeseries(timeseries: Dict[str, List[dict]], output_path: Path):
    """Plot throughput time series for all VMs"""
    plt.figure(figsize=(14, 8))
    
    # Plot individual VMs
    for vm_name, ts_data in sorted(timeseries.items()):
        times = [d['end'] for d in ts_data]
        throughputs = [d['throughput_gbps'] for d in ts_data]
        plt.plot(times, throughputs, label=vm_name, alpha=0.7, linewidth=1)
    
    # Calculate and plot average
    if timeseries:
        # Use the first VM's time values as reference
        first_vm_ts = list(timeseries.values())[0]
        reference_times = [d['end'] for d in first_vm_ts]
        
        max_len = max(len(ts) for ts in timeseries.values())
        avg_throughput = []
        avg_times = []
        
        for i in range(max_len):
            values = []
            for ts_data in timeseries.values():
                if i < len(ts_data):
                    values.append(ts_data[i]['throughput_gbps'])
            if values:
                avg_throughput.append(np.mean(values))
                # Use time from first VM (or average if multiple VMs have different times)
                if i < len(reference_times):
                    avg_times.append(reference_times[i])
                else:
                    avg_times.append(i)  # Fallback to index
        
        if avg_times and avg_throughput:
            plt.plot(avg_times, avg_throughput, 'k-', linewidth=3, label='Average', alpha=0.9)
    
    plt.xlabel('Time (seconds)', fontsize=12)
    plt.ylabel('Throughput (Gbps)', fontsize=12)
    plt.title('Throughput Over Time - All VMs', fontsize=14, fontweight='bold')
    plt.legend(loc='best', fontsize=8, ncol=2)
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"✅ Saved plot: {output_path}")


def plot_per_vm_throughput(per_vm_stats: Dict[str, dict], output_path: Path):
    """Plot throughput per VM as bar chart"""
    plt.figure(figsize=(12, 6))
    
    vms = sorted(per_vm_stats.keys(), key=lambda x: int(x[2:]))  # Sort by VM number
    throughputs = [per_vm_stats[vm]['throughput_gbps'] for vm in vms]
    
    bars = plt.bar(vms, throughputs, color='steelblue', alpha=0.8)
    
    # Add value labels on bars
    for bar in bars:
        height = bar.get_height()
        plt.text(bar.get_x() + bar.get_width()/2., height,
                f'{height:.2f}',
                ha='center', va='bottom', fontsize=9)
    
    plt.xlabel('VM', fontsize=12)
    plt.ylabel('Throughput (Gbps)', fontsize=12)
    plt.title('Throughput Per VM', fontsize=14, fontweight='bold')
    plt.xticks(rotation=45)
    plt.grid(True, alpha=0.3, axis='y')
    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"✅ Saved plot: {output_path}")


def plot_cpu_utilization(per_vm_stats: Dict[str, dict], output_path: Path):
    """Plot CPU utilization per VM"""
    plt.figure(figsize=(12, 6))
    
    vms = sorted(per_vm_stats.keys(), key=lambda x: int(x[2:]))
    cpu_host = [per_vm_stats[vm]['cpu_host_total'] for vm in vms]
    cpu_remote = [per_vm_stats[vm]['cpu_remote_total'] for vm in vms]
    
    x = np.arange(len(vms))
    width = 0.35
    
    plt.bar(x - width/2, cpu_host, width, label='Client (Host)', color='coral', alpha=0.8)
    plt.bar(x + width/2, cpu_remote, width, label='Server (Remote)', color='steelblue', alpha=0.8)
    
    plt.xlabel('VM', fontsize=12)
    plt.ylabel('CPU Utilization (%)', fontsize=12)
    plt.title('CPU Utilization Per VM', fontsize=14, fontweight='bold')
    plt.xticks(x, vms, rotation=45)
    plt.legend()
    plt.grid(True, alpha=0.3, axis='y')
    plt.tight_layout()
    plt.savefig(output_path, dpi=150, bbox_inches='tight')
    plt.close()
    print(f"✅ Saved plot: {output_path}")


def generate_markdown_report(per_vm_stats: Dict[str, dict], overall: dict, output_path: Path):
    """Generate a comprehensive markdown report"""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    
    report = f"""# iperf3 Performance Test Report

**Generated:** {timestamp}  
**Test Duration:** ~30 seconds per VM  
**Number of VMs:** {overall['num_vms']}  
**Parallel Streams:** 4 per VM

---

## Overall Summary

| Metric | Value |
|--------|-------|
| **Total Throughput** | {overall['total_throughput_gbps']:.2f} Gbps ({overall['total_throughput_mbps']:.2f} Mbps) |
| **Average per VM** | {overall['avg_per_vm_gbps']:.2f} Gbps |
| **Total Data Sent** | {overall['total_bytes_sent_gb']:.2f} GB |
| **Avg Client CPU** | {overall['avg_cpu_host_percent']:.2f}% |
| **Avg Server CPU** | {overall['avg_cpu_remote_percent']:.2f}% |
| **Total Retransmits** | {overall['total_retransmits']} |

---

## Per-VM Results

### Throughput

| VM | Throughput (Gbps) | Throughput (Mbps) | Data Sent (GB) | Retransmits |
|----|-------------------|-------------------|----------------|-------------|
"""
    
    for vm in sorted(per_vm_stats.keys(), key=lambda x: int(x[2:])):
        stats = per_vm_stats[vm]
        report += f"| {vm} | {stats['throughput_gbps']:.3f} | {stats['throughput_mbps']:.2f} | {stats['bytes_sent']/1e9:.3f} | {stats['retransmits']} |\n"
    
    report += """
### CPU Utilization

| VM | Client Total (%) | Client User (%) | Client System (%) | Server Total (%) | Server User (%) | Server System (%) |
|----|------------------|-----------------|-------------------|------------------|-----------------|-------------------|
"""
    
    for vm in sorted(per_vm_stats.keys(), key=lambda x: int(x[2:])):
        stats = per_vm_stats[vm]
        report += f"| {vm} "
        report += f"| {stats['cpu_host_total']:.2f} "
        report += f"| {stats['cpu_host_user']:.2f} "
        report += f"| {stats['cpu_host_system']:.2f} "
        report += f"| {stats['cpu_remote_total']:.2f} "
        report += f"| {stats['cpu_remote_user']:.2f} "
        report += f"| {stats['cpu_remote_system']:.2f} |\n"
    
    report += """
---

## Visualizations

See the following plots for detailed analysis:

- `throughput_timeseries.png` - Throughput over time for all VMs
- `throughput_per_vm.png` - Bar chart of throughput per VM
- `cpu_utilization.png` - CPU utilization comparison

---

## Notes

- **Client** refers to the VM sending data (odd VM numbers: vm1, vm3, vm5, etc.)
- **Server** refers to the VM receiving data (even VM numbers: vm0, vm2, vm4, etc.)
- Each client connects to the corresponding server (vm1 → vm0, vm3 → vm2, etc.)
"""
    
    with open(output_path, 'w') as f:
        f.write(report)
    
    print(f"✅ Saved report: {output_path}")


def get_next_report_number(base_dir: Path) -> int:
    """Find the next report number by counting existing report-* directories"""
    if not base_dir.exists():
        return 0
    
    existing_reports = []
    for item in base_dir.iterdir():
        if item.is_dir() and item.name.startswith("report-"):
            try:
                num = int(item.name.split("-")[1])
                existing_reports.append(num)
            except (ValueError, IndexError):
                continue
    
    if not existing_reports:
        return 0
    
    return max(existing_reports) + 1


def process_iperf_results(logs_dir: Path, reports_dir: Path, mode: str):
    """Process iperf3 results and generate reports
    
    Args:
        logs_dir: Directory containing VM log files
        reports_dir: Directory where reports will be saved
        mode: Processing mode ('vm-vm-internal' or 'vm-client')
    """
    process_all_vms = (mode == "vm-client")
    
    # Load results
    print("📂 Loading results...")
    print(f"   Mode: {mode} ({'processing all VMs' if process_all_vms else 'processing odd VMs only'})")
    results = load_results(logs_dir, process_all_vms=process_all_vms)
    print(f"   Found {len(results)} VM results")
    print()
    
    if not results:
        print("❌ No results found!")
        return
    
    # Extract statistics
    print("📊 Extracting statistics...")
    per_vm_stats = extract_per_vm_stats(results)
    overall_stats = calculate_overall_stats(per_vm_stats)
    timeseries = extract_timeseries(results)
    print()
    
    # Generate plots
    print("📈 Generating plots...")
    plot_throughput_timeseries(timeseries, reports_dir / "throughput_timeseries.png")
    plot_per_vm_throughput(per_vm_stats, reports_dir / "throughput_per_vm.png")
    plot_cpu_utilization(per_vm_stats, reports_dir / "cpu_utilization.png")
    print()
    
    # Generate report
    print("📝 Generating markdown report...")
    generate_markdown_report(per_vm_stats, overall_stats, reports_dir / "report.md")
    print()
    
    # Print summary
    print("=" * 60)
    print("SUMMARY")
    print("=" * 60)
    print(f"Total Throughput:     {overall_stats['total_throughput_gbps']:.2f} Gbps")
    print(f"Average per VM:       {overall_stats['avg_per_vm_gbps']:.2f} Gbps")
    print(f"Total Data Sent:      {overall_stats['total_bytes_sent_gb']:.2f} GB")
    print(f"Avg Client CPU:       {overall_stats['avg_cpu_host_percent']:.2f}%")
    print(f"Avg Server CPU:       {overall_stats['avg_cpu_remote_percent']:.2f}%")
    print("=" * 60)
    print()
    print(f"✅ All reports saved to: {reports_dir}/")
    print()


def main():
    """Main processing pipeline - for standalone execution"""
    parser = argparse.ArgumentParser(description="Process iperf3 test results and generate reports")
    parser.add_argument(
        "folder",
        choices=["dpdk", "tap", "dpdk-tap"],
        help="Folder name: 'dpdk', 'tap', or 'dpdk-tap'"
    )
    parser.add_argument(
        "mode",
        choices=["vm-vm-internal", "vm-client"],
        help="Processing mode: 'vm-vm-internal' (process only odd VMs) or 'vm-client' (process all VMs)"
    )
    args = parser.parse_args()
    
    # Determine if we should process all VMs
    process_all_vms = (args.mode == "vm-client")
    
    # Set up directories relative to script
    base_dir = SCRIPT_DIR.parent / args.folder
    logs_dir = base_dir / "logs"
    reports_base_dir = base_dir / "iperf" / args.mode
    
    # Create base directory if it doesn't exist
    reports_base_dir.mkdir(exist_ok=True, parents=True)
    
    # Check if logs directory exists
    if not logs_dir.exists():
        print(f"❌ Logs directory not found: {logs_dir}")
        print(f"   Please ensure log files are in: {logs_dir}/")
        return
    
    # Count VMs to determine report directory name
    num_vms = 0
    for log_file in sorted(logs_dir.glob("vm*.log")):
        vm_name = log_file.stem  # e.g., "vm1"
        vm_num = int(vm_name[2:])  # Extract number: "vm1" -> 1
        
        # Count based on mode
        if process_all_vms or vm_num % 2 == 1:  # All VMs for vm-client, odd VMs for vm-vm-internal
            num_vms += 1
    
    # Create report directory: iperf/{mode}/report-{n}vm
    reports_dir = reports_base_dir / f"report-{num_vms}vm"
    reports_dir.mkdir(exist_ok=True, parents=True)
    
    print("🔥 Processing iperf3 results...")
    print(f"📁 Base folder: {base_dir}")
    print(f"📁 Logs folder: {logs_dir}")
    print(f"📁 Reports folder: {reports_dir}")
    print()
    
    # Call the main processing function
    process_iperf_results(logs_dir, reports_dir, args.mode)


if __name__ == "__main__":
    main()

