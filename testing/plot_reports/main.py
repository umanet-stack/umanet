#!/usr/bin/env python3
"""
Plot comparison graphs from test reports
Compares tap, dpdk, and ovs-dpdk results for iperf, iperf-udp, and sockperf tests
"""
import argparse
import re
from pathlib import Path
from typing import Dict, List, Tuple, Optional
from datetime import datetime, timezone, timedelta
import matplotlib.pyplot as plt
import numpy as np

# Get script directory
SCRIPT_DIR = Path(__file__).parent.resolve()
TESTING_DIR = SCRIPT_DIR.parent

# Consistent color scheme across all plots
# UMANet: red, green
COLOR_UMANET_1 = '#d62728'  # red
COLOR_UMANET_2 = '#2ca02c'  # green
# Linux: blue, orange
COLOR_LINUX_1 = '#1f77b4'   # blue
COLOR_LINUX_2 = '#ff7f0e'   # orange
# OvS-DPDK: purple, brown
COLOR_OVS_DPDK_1 = '#9467bd'  # purple
COLOR_OVS_DPDK_2 = '#8c564b'  # brown


def extract_number_from_path(path: Path) -> Optional[int]:
    """Extract number of VMs from report path like 'report-16vm'"""
    match = re.search(r'report-(\d+)vm', str(path))
    return int(match.group(1)) if match else None


def parse_iperf_report(report_path: Path) -> Optional[Dict]:
    """Parse iperf report markdown file"""
    try:
        with open(report_path, 'r') as f:
            content = f.read()
        
        # Extract Total VMs (for x-axis)
        total_vms_match = re.search(r'\*\*Total VMs:\*\* (\d+)', content)
        if not total_vms_match:
            # Fallback to old format for backward compatibility
            vm_match = re.search(r'\*\*Number of VMs:\*\* (\d+)', content)
            if not vm_match:
                return None
            total_vms = int(vm_match.group(1))
        else:
            total_vms = int(total_vms_match.group(1))
        
        # Extract Total Throughput (Gbps)
        total_match = re.search(r'\*\*Total Throughput\*\* \| ([0-9.]+) Gbps', content)
        if not total_match:
            return None
        total_throughput = float(total_match.group(1))
        
        # Extract Average per VM (Gbps)
        avg_match = re.search(r'\*\*Average per VM\*\* \| ([0-9.]+) Gbps', content)
        if not avg_match:
            return None
        avg_per_vm = float(avg_match.group(1))
        
        return {
            'num_vms': total_vms,  # Use Total VMs for x-axis
            'total_throughput': total_throughput,
            'throughput_per_vm': avg_per_vm
        }
    except Exception as e:
        print(f"Error parsing {report_path}: {e}")
        return None


def parse_iperf_udp_report(report_path: Path) -> Optional[Dict]:
    """Parse iperf-udp report markdown file"""
    try:
        with open(report_path, 'r') as f:
            content = f.read()
        
        # Extract Total VMs (for x-axis)
        total_vms_match = re.search(r'\*\*Total VMs:\*\* (\d+)', content)
        if not total_vms_match:
            # Fallback to old format for backward compatibility
            vm_match = re.search(r'\*\*Number of VMs:\*\* (\d+)', content)
            if not vm_match:
                return None
            total_vms = int(vm_match.group(1))
        else:
            total_vms = int(total_vms_match.group(1))
        
        # Extract Sender PPS (Total)
        sender_pps_match = re.search(r'\*\*Sender PPS \(Total\)\*\* \| ([0-9,]+) packets/sec', content)
        if not sender_pps_match:
            return None
        sender_pps_total = float(sender_pps_match.group(1).replace(',', ''))
        
        # Extract Receiver PPS (Total)
        receiver_pps_match = re.search(r'\*\*Receiver PPS \(Total\)\*\* \| ([0-9,]+) packets/sec', content)
        if not receiver_pps_match:
            return None
        receiver_pps_total = float(receiver_pps_match.group(1).replace(',', ''))
        
        # Calculate lost PPS
        lost_pps_total = sender_pps_total - receiver_pps_total
        
        # Extract Avg Sender PPS per VM
        avg_sender_pps_match = re.search(r'\*\*Avg Sender PPS per VM\*\* \| ([0-9,]+) packets/sec', content)
        if not avg_sender_pps_match:
            return None
        avg_sender_pps_per_vm = float(avg_sender_pps_match.group(1).replace(',', ''))
        
        # Extract Avg Receiver PPS per VM
        avg_receiver_pps_match = re.search(r'\*\*Avg Receiver PPS per VM\*\* \| ([0-9,]+) packets/sec', content)
        if not avg_receiver_pps_match:
            return None
        avg_receiver_pps_per_vm = float(avg_receiver_pps_match.group(1).replace(',', ''))
        
        # Calculate lost PPS per VM
        lost_pps_per_vm = avg_sender_pps_per_vm - avg_receiver_pps_per_vm
        
        return {
            'num_vms': total_vms,  # Use Total VMs for x-axis
            'sender_pps_total': sender_pps_total,
            'receiver_pps_total': receiver_pps_total,
            'lost_pps_total': lost_pps_total,
            'sender_pps_per_vm': avg_sender_pps_per_vm,
            'receiver_pps_per_vm': avg_receiver_pps_per_vm,
            'lost_pps_per_vm': lost_pps_per_vm
        }
    except Exception as e:
        print(f"Error parsing {report_path}: {e}")
        return None


def parse_sockperf_report(report_path: Path) -> Optional[Dict]:
    """Parse sockperf report markdown file"""
    try:
        with open(report_path, 'r') as f:
            content = f.read()
        
        # Extract Total VMs (for x-axis)
        total_vms_match = re.search(r'\*\*Total VMs:\*\* (\d+)', content)
        if not total_vms_match:
            # Fallback to old format for backward compatibility
            vm_match = re.search(r'\*\*Number of VMs:\*\* (\d+)', content)
            if not vm_match:
                return None
            total_vms = int(vm_match.group(1))
        else:
            total_vms = int(total_vms_match.group(1))
        
        # Extract Average p99
        p99_match = re.search(r'\*\*Average p99\*\* \| ([0-9.]+) μs', content)
        if not p99_match:
            return None
        p99_latency = float(p99_match.group(1))
        
        # Extract Total Messages Sent
        sent_match = re.search(r'\*\*Total Messages Sent\*\* \| ([0-9,]+)', content)
        if not sent_match:
            return None
        total_sent = int(sent_match.group(1).replace(',', ''))
        
        return {
            'num_vms': total_vms,  # Use Total VMs for x-axis
            'p99_latency': p99_latency,
            'total_sent': total_sent
        }
    except Exception as e:
        print(f"Error parsing {report_path}: {e}")
        return None


def collect_reports(test_type: str) -> Tuple[List[Dict], List[Dict], List[Dict]]:
    """Collect reports from tap, dpdk, and ovs-dpdk folders"""
    tap_reports = []
    dpdk_reports = []
    ovs_dpdk_reports = []
    
    tap_base = TESTING_DIR / 'tap' / test_type / 'vm-client'
    dpdk_base = TESTING_DIR / 'dpdk' / test_type / 'vm-client'
    ovs_dpdk_base = TESTING_DIR / 'ovs-dpdk' / test_type / 'vm-client'
    
    # Parse function based on test type
    if test_type == 'iperf':
        parse_func = parse_iperf_report
    elif test_type == 'iperf-udp':
        parse_func = parse_iperf_udp_report
    elif test_type == 'sockperf':
        parse_func = parse_sockperf_report
    else:
        raise ValueError(f"Unknown test type: {test_type}")
    
    # Collect tap reports
    if tap_base.exists():
        for report_dir in tap_base.glob('report-*vm'):
            report_path = report_dir / 'report.md'
            if report_path.exists():
                data = parse_func(report_path)
                if data:
                    tap_reports.append(data)
    
    # Collect dpdk reports
    if dpdk_base.exists():
        for report_dir in dpdk_base.glob('report-*vm'):
            report_path = report_dir / 'report.md'
            if report_path.exists():
                data = parse_func(report_path)
                if data:
                    dpdk_reports.append(data)
    
    # Collect ovs-dpdk reports
    if ovs_dpdk_base.exists():
        for report_dir in ovs_dpdk_base.glob('report-*vm'):
            report_path = report_dir / 'report.md'
            if report_path.exists():
                data = parse_func(report_path)
                if data:
                    ovs_dpdk_reports.append(data)
    
    # Sort by number of VMs
    tap_reports.sort(key=lambda x: x['num_vms'])
    dpdk_reports.sort(key=lambda x: x['num_vms'])
    ovs_dpdk_reports.sort(key=lambda x: x['num_vms'])
    
    return tap_reports, dpdk_reports, ovs_dpdk_reports


def plot_iperf(tap_reports: List[Dict], dpdk_reports: List[Dict], ovs_dpdk_reports: List[Dict], output_dir: Path):
    """Plot iperf comparison graphs"""
    fig, ax = plt.subplots(1, 1, figsize=(10, 6))
    
    # Plot all lines on the same y-axis with log scale
    if tap_reports:
        tap_vms = [r['num_vms'] for r in tap_reports]
        tap_total = [r['total_throughput'] for r in tap_reports]
        tap_per_vm = [r['throughput_per_vm'] for r in tap_reports]
        ax.plot(tap_vms, tap_total, 'o-', label='Linux (Total)', linewidth=2, markersize=5, color=COLOR_LINUX_1)
        ax.plot(tap_vms, tap_per_vm, 'o--', label='Linux (Per VM)', linewidth=2, markersize=5, color=COLOR_LINUX_2)
    
    if dpdk_reports:
        dpdk_vms = [r['num_vms'] for r in dpdk_reports]
        dpdk_total = [r['total_throughput'] for r in dpdk_reports]
        dpdk_per_vm = [r['throughput_per_vm'] for r in dpdk_reports]
        ax.plot(dpdk_vms, dpdk_total, 's-', label='UMANet (Total)', linewidth=2, markersize=5, color=COLOR_UMANET_1)
        ax.plot(dpdk_vms, dpdk_per_vm, 's--', label='UMANet (Per VM)', linewidth=2, markersize=5, color=COLOR_UMANET_2)
    
    if ovs_dpdk_reports:
        ovs_dpdk_vms = [r['num_vms'] for r in ovs_dpdk_reports]
        ovs_dpdk_total = [r['total_throughput'] for r in ovs_dpdk_reports]
        ovs_dpdk_per_vm = [r['throughput_per_vm'] for r in ovs_dpdk_reports]
        ax.plot(ovs_dpdk_vms, ovs_dpdk_total, '^-', label='OvS-DPDK (Total)', linewidth=2, markersize=5, color=COLOR_OVS_DPDK_1)
        ax.plot(ovs_dpdk_vms, ovs_dpdk_per_vm, '^--', label='OvS-DPDK (Per VM)', linewidth=2, markersize=5, color=COLOR_OVS_DPDK_2)
    
    ax.set_xlabel('VM Count', fontsize=16)
    ax.set_ylabel('Throughput (Gbps)', fontsize=16)
    ax.set_yscale('log')
    ax.tick_params(axis='both', which='major', labelsize=14)
    ax.legend(fontsize=13, loc='best')
    ax.grid(True, alpha=0.3, which='both')
    
    plt.tight_layout()
    output_path_png = output_dir / 'iperf_comparison.png'
    output_path_pdf = output_dir / 'iperf_comparison.pdf'
    plt.savefig(output_path_png, dpi=300, bbox_inches='tight')
    plt.savefig(output_path_pdf, bbox_inches='tight')
    print(f"Saved: {output_path_png}")
    print(f"Saved: {output_path_pdf}")
    plt.close()


def plot_sockperf(tap_reports: List[Dict], dpdk_reports: List[Dict], ovs_dpdk_reports: List[Dict], output_dir: Path):
    """Plot sockperf comparison graphs"""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))
    
    # Plot 1: p99 Latency
    if tap_reports:
        tap_vms = [r['num_vms'] for r in tap_reports]
        tap_p99 = [r['p99_latency'] for r in tap_reports]
        ax1.plot(tap_vms, tap_p99, 'o-', label='Linux', linewidth=2, markersize=5, color=COLOR_LINUX_1)
    
    if dpdk_reports:
        dpdk_vms = [r['num_vms'] for r in dpdk_reports]
        dpdk_p99 = [r['p99_latency'] for r in dpdk_reports]
        ax1.plot(dpdk_vms, dpdk_p99, 's-', label='UMANet', linewidth=2, markersize=5, color=COLOR_UMANET_1)
    
    if ovs_dpdk_reports:
        ovs_dpdk_vms = [r['num_vms'] for r in ovs_dpdk_reports]
        ovs_dpdk_p99 = [r['p99_latency'] for r in ovs_dpdk_reports]
        ax1.plot(ovs_dpdk_vms, ovs_dpdk_p99, '^-', label='OvS-DPDK', linewidth=2, markersize=5, color=COLOR_OVS_DPDK_1)
    
    ax1.set_xlabel('VM Count', fontsize=16)
    ax1.set_ylabel('p99 Latency (μs)', fontsize=16)
    ax1.tick_params(axis='both', which='major', labelsize=14)
    ax1.legend(fontsize=13)
    ax1.grid(True, alpha=0.3)
    
    # Plot 2: Messages Sent
    if tap_reports:
        tap_vms = [r['num_vms'] for r in tap_reports]
        tap_sent = [r['total_sent'] for r in tap_reports]
        ax2.plot(tap_vms, tap_sent, 'o-', label='Linux', linewidth=2, markersize=5, color=COLOR_LINUX_1)
    
    if dpdk_reports:
        dpdk_vms = [r['num_vms'] for r in dpdk_reports]
        dpdk_sent = [r['total_sent'] for r in dpdk_reports]
        ax2.plot(dpdk_vms, dpdk_sent, 's-', label='UMANet', linewidth=2, markersize=5, color=COLOR_UMANET_1)
    
    if ovs_dpdk_reports:
        ovs_dpdk_vms = [r['num_vms'] for r in ovs_dpdk_reports]
        ovs_dpdk_sent = [r['total_sent'] for r in ovs_dpdk_reports]
        ax2.plot(ovs_dpdk_vms, ovs_dpdk_sent, '^-', label='OvS-DPDK', linewidth=2, markersize=5, color=COLOR_OVS_DPDK_1)
    
    ax2.set_xlabel('VM Count', fontsize=14)
    ax2.set_ylabel('Messages Sent', fontsize=14)
    ax2.tick_params(axis='both', which='major', labelsize=12)
    ax2.legend(fontsize=12)
    ax2.grid(True, alpha=0.3)
    
    plt.tight_layout()
    output_path_png = output_dir / 'sockperf_comparison.png'
    output_path_pdf = output_dir / 'sockperf_comparison.pdf'
    plt.savefig(output_path_png, dpi=300, bbox_inches='tight')
    plt.savefig(output_path_pdf, bbox_inches='tight')
    print(f"Saved: {output_path_png}")
    print(f"Saved: {output_path_pdf}")
    plt.close()


def plot_iperf_udp(tap_reports: List[Dict], dpdk_reports: List[Dict], ovs_dpdk_reports: List[Dict], output_dir: Path):
    """Plot iperf-udp comparison graphs (line plots)"""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))
    
    # Plot 1: Total PPS (Received and Lost)
    if tap_reports:
        tap_vms = [r['num_vms'] for r in tap_reports]
        tap_received = [r['receiver_pps_total'] for r in tap_reports]
        tap_lost = [r['lost_pps_total'] for r in tap_reports]
        ax1.plot(tap_vms, tap_received, 'o-', label='Received (Linux)', linewidth=2, markersize=5, color=COLOR_LINUX_1)
        ax1.plot(tap_vms, tap_lost, 'o--', label='Lost (Linux)', linewidth=2, markersize=5, color=COLOR_LINUX_2)
    
    if dpdk_reports:
        dpdk_vms = [r['num_vms'] for r in dpdk_reports]
        dpdk_received = [r['receiver_pps_total'] for r in dpdk_reports]
        dpdk_lost = [r['lost_pps_total'] for r in dpdk_reports]
        ax1.plot(dpdk_vms, dpdk_received, 's-', label='Received (UMANet)', linewidth=2, markersize=5, color=COLOR_UMANET_1)
        ax1.plot(dpdk_vms, dpdk_lost, 's--', label='Lost (UMANet)', linewidth=2, markersize=5, color=COLOR_UMANET_2)
    
    if ovs_dpdk_reports:
        ovs_dpdk_vms = [r['num_vms'] for r in ovs_dpdk_reports]
        ovs_dpdk_received = [r['receiver_pps_total'] for r in ovs_dpdk_reports]
        ovs_dpdk_lost = [r['lost_pps_total'] for r in ovs_dpdk_reports]
        ax1.plot(ovs_dpdk_vms, ovs_dpdk_received, '^-', label='Received (OvS-DPDK)', linewidth=2, markersize=5, color=COLOR_OVS_DPDK_1)
        ax1.plot(ovs_dpdk_vms, ovs_dpdk_lost, '^--', label='Lost (OvS-DPDK)', linewidth=2, markersize=5, color=COLOR_OVS_DPDK_2)
    
    ax1.set_xlabel('VM Count', fontsize=16)
    ax1.set_ylabel('PPS', fontsize=16)
    ax1.tick_params(axis='both', which='major', labelsize=14)
    ax1.grid(True, alpha=0.3)
    
    # Plot 2: PPS per VM (Received and Lost)
    if tap_reports:
        tap_vms = [r['num_vms'] for r in tap_reports]
        tap_received = [r['receiver_pps_per_vm'] for r in tap_reports]
        tap_lost = [r['lost_pps_per_vm'] for r in tap_reports]
        ax2.plot(tap_vms, tap_received, 'o-', label='Received (Linux)', linewidth=2, markersize=5, color=COLOR_LINUX_1)
        ax2.plot(tap_vms, tap_lost, 'o--', label='Lost (Linux)', linewidth=2, markersize=5, color=COLOR_LINUX_2)
    
    if dpdk_reports:
        dpdk_vms = [r['num_vms'] for r in dpdk_reports]
        dpdk_received = [r['receiver_pps_per_vm'] for r in dpdk_reports]
        dpdk_lost = [r['lost_pps_per_vm'] for r in dpdk_reports]
        ax2.plot(dpdk_vms, dpdk_received, 's-', label='Received (UMANet)', linewidth=2, markersize=5, color=COLOR_UMANET_1)
        ax2.plot(dpdk_vms, dpdk_lost, 's--', label='Lost (UMANet)', linewidth=2, markersize=5, color=COLOR_UMANET_2)
    
    if ovs_dpdk_reports:
        ovs_dpdk_vms = [r['num_vms'] for r in ovs_dpdk_reports]
        ovs_dpdk_received = [r['receiver_pps_per_vm'] for r in ovs_dpdk_reports]
        ovs_dpdk_lost = [r['lost_pps_per_vm'] for r in ovs_dpdk_reports]
        ax2.plot(ovs_dpdk_vms, ovs_dpdk_received, '^-', label='Received (OvS-DPDK)', linewidth=2, markersize=5, color=COLOR_OVS_DPDK_1)
        ax2.plot(ovs_dpdk_vms, ovs_dpdk_lost, '^--', label='Lost (OvS-DPDK)', linewidth=2, markersize=5, color=COLOR_OVS_DPDK_2)
    
    ax2.set_xlabel('VM Count', fontsize=16)
    ax2.set_ylabel('PPS per VM', fontsize=16)
    ax2.tick_params(axis='both', which='major', labelsize=14)
    ax2.grid(True, alpha=0.3)
    
    # Create single shared legend outside the plots
    handles, labels = ax1.get_legend_handles_labels()
    fig.legend(handles, labels, loc='upper center', bbox_to_anchor=(0.5, 1.05), ncol=3, fontsize=13, frameon=True)
    
    plt.tight_layout(rect=[0, 0, 1, 0.88])
    output_path_png = output_dir / 'iperf_udp_comparison.png'
    output_path_pdf = output_dir / 'iperf_udp_comparison.pdf'
    plt.savefig(output_path_png, dpi=300, bbox_inches='tight')
    plt.savefig(output_path_pdf, bbox_inches='tight')
    print(f"Saved: {output_path_png}")
    print(f"Saved: {output_path_pdf}")
    plt.close()


def main():
    """Main function"""
    parser = argparse.ArgumentParser(
        description='Plot comparison graphs from test reports (tap vs dpdk vs ovs-dpdk)',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  ./main.py iperf
  ./main.py iperf-udp
  ./main.py sockperf
        """
    )
    parser.add_argument(
        'test_type',
        choices=['iperf', 'iperf-udp', 'sockperf'],
        help='Test type to plot'
    )
    
    args = parser.parse_args()
    
    print(f"📊 Plotting {args.test_type} comparison graphs...")
    print(f"📁 Testing directory: {TESTING_DIR}")
    print()
    
    # Collect reports
    tap_reports, dpdk_reports, ovs_dpdk_reports = collect_reports(args.test_type)
    
    print(f"Found {len(tap_reports)} tap reports")
    print(f"Found {len(dpdk_reports)} dpdk reports")
    print(f"Found {len(ovs_dpdk_reports)} ovs-dpdk reports")
    print()
    
    if not tap_reports and not dpdk_reports and not ovs_dpdk_reports:
        print("❌ No reports found!")
        return
    
    # Create output directory with timestamp (in plot_reports folder, GMT+7 timezone)
    gmt7 = timezone(timedelta(hours=7))
    now = datetime.now(gmt7)
    timestamp = now.strftime("%Y%m%d_%H")
    output_dir = SCRIPT_DIR / f"plots_{timestamp}"
    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"📁 Output directory: {output_dir}")
    print()
    
    # Plot based on test type
    if args.test_type == 'iperf':
        plot_iperf(tap_reports, dpdk_reports, ovs_dpdk_reports, output_dir)
    elif args.test_type == 'iperf-udp':
        plot_iperf_udp(tap_reports, dpdk_reports, ovs_dpdk_reports, output_dir)
    elif args.test_type == 'sockperf':
        plot_sockperf(tap_reports, dpdk_reports, ovs_dpdk_reports, output_dir)
    
    print()
    print("✅ Done!")


if __name__ == '__main__':
    main()

