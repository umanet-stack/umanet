#!/usr/bin/env python3
"""
Process iperf3 test results and generate reports
"""
import json
import os
from pathlib import Path
from typing import Dict, List
import matplotlib.pyplot as plt
import matplotlib
matplotlib.use('Agg')  # Non-interactive backend
import numpy as np
from datetime import datetime

# Directories
RESULTS_DIR = Path("testing/results")
REPORTS_DIR = Path("testing/reports")


def load_results() -> Dict[str, dict]:
    """Load all JSON results from the results directory"""
    results = {}
    
    for json_file in RESULTS_DIR.glob("*.json"):
        vm_name = json_file.stem  # e.g., "vm1"
        try:
            with open(json_file, 'r') as f:
                results[vm_name] = json.load(f)
        except Exception as e:
            print(f"⚠️  Error loading {json_file}: {e}")
    
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
    max_len = max(len(ts) for ts in timeseries.values())
    avg_throughput = []
    
    for i in range(max_len):
        values = []
        for ts_data in timeseries.values():
            if i < len(ts_data):
                values.append(ts_data[i]['throughput_gbps'])
        if values:
            avg_throughput.append(np.mean(values))
    
    times = list(range(len(avg_throughput)))
    plt.plot(times, avg_throughput, 'k-', linewidth=3, label='Average', alpha=0.9)
    
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


def main():
    """Main processing pipeline"""
    print("🔥 Processing iperf3 results...")
    print()
    
    # Create reports directory
    REPORTS_DIR.mkdir(exist_ok=True, parents=True)
    
    # Load results
    print("📂 Loading results...")
    results = load_results()
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
    plot_throughput_timeseries(timeseries, REPORTS_DIR / "throughput_timeseries.png")
    plot_per_vm_throughput(per_vm_stats, REPORTS_DIR / "throughput_per_vm.png")
    plot_cpu_utilization(per_vm_stats, REPORTS_DIR / "cpu_utilization.png")
    print()
    
    # Generate report
    print("📝 Generating markdown report...")
    generate_markdown_report(per_vm_stats, overall_stats, REPORTS_DIR / "report.md")
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
    print(f"✅ All reports saved to: {REPORTS_DIR}/")
    print()


if __name__ == "__main__":
    main()

