import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import Dict
from process_iperf import process_iperf_results
from process_sockperf import process_sockperf_results

SCRIPT_DIR = Path(__file__).parent.resolve()


def load_env():
    repo_root = SCRIPT_DIR.parent.parent
    env_sh = repo_root / "env.sh"

    if not env_sh.exists():
        print(f"Warning: env.sh not found at {env_sh}")
        return

    try:
        cmd = f"source {env_sh} && env"
        result = subprocess.run(
            cmd,
            shell=True,
            executable="/bin/bash",
            capture_output=True,
            text=True,
            cwd=repo_root,
        )

        if result.returncode != 0:
            print(f"Warning: Failed to source env.sh: {result.stderr}")
            return

        env_vars = {}
        for line in result.stdout.splitlines():
            if "=" in line:
                key, _, value = line.partition("=")
                env_vars[key] = value

        os.environ.update(env_vars)
        print(f"✅ Loaded environment from {env_sh}")
        print(f"   TEST={os.environ.get('TEST', '(not set)')}")
        print()

    except Exception as e:
        print(f"Warning: Failed to load env.sh: {e}")
        print(f"   You may need to manually set TEST environment variable")
        print()


def parse_args():
    parser = argparse.ArgumentParser(
        description="Process test results and generate reports",
        epilog="Set TEST env to 'iperf' or 'iperf-udp' or 'sockperf' to specify test type",
    )
    parser.add_argument(
        "folder",
        choices=["dpdk", "tap", "ovs-dpdk"],
        help="Folder name: 'dpdk', 'tap', or 'ovs-dpdk'",
    )
    parser.add_argument(
        "mode",
        choices=["vm-vm-internal", "vm-client"],
        help="Processing mode: 'vm-vm-internal' (process only odd VMs) or 'vm-client' (process all VMs)",
    )
    parser.add_argument(
        "num_vms",
        type=int,
        help="Number of VMs to process",
    )
    return parser.parse_args()


def setup_directories(folder: str, mode: str, test_type: str, num_vms: int) -> Dict[str, Path]:
    base_dir = SCRIPT_DIR.parent / folder
    logs_dir = base_dir / test_type / mode / "logs" / f"logs-{num_vms}"
    reports_base_dir = base_dir / test_type / mode # e.g. dpdk/iperf/vm-client
    reports_base_dir.mkdir(exist_ok=True, parents=True)

    if not logs_dir.exists():
        print(f"Error: logs directory not found: {logs_dir}")
        return

    process_all_vms = mode == "vm-client"
    num_vms = 0
    for log_file in sorted(logs_dir.glob("vm*.log")):
        vm_name = log_file.stem
        vm_num = int(vm_name[2:])

        if process_all_vms or vm_num % 2 == 1:
            num_vms += 1

    reports_dir = reports_base_dir / f"report-{num_vms}vm"
    reports_dir.mkdir(exist_ok=True, parents=True)

    return {
        "base_dir": base_dir,
        "logs_dir": logs_dir,
        "reports_dir": reports_dir,
    }


def detect_test_type() -> str:
    test_env = os.environ.get("TEST", "").lower()
    if test_env not in ["iperf", "iperf-udp", "sockperf"]:
        print(f"ERROR: TEST env must be set to 'iperf', 'iperf-udp', or 'sockperf'")
        print(f"   Current value: TEST='{os.environ.get('TEST', '(not set)')}'")
        sys.exit(1)

    return test_env


def main():
    load_env()
    test_type = detect_test_type()
    args = parse_args()
    dirs = setup_directories(args.folder, args.mode, test_type, args.num_vms)

    print(f"Processing {test_type} results...")
    print(f"- Base folder: {dirs['base_dir']}")
    print(f"- Logs folder: {dirs['logs_dir']}")
    print(f"- Reports folder: {dirs['reports_dir']}")
    print()

    if test_type == "iperf" or test_type == "iperf-udp":
        process_iperf_results(dirs["logs_dir"], dirs["reports_dir"], args.mode)

    elif test_type == "sockperf":
        process_sockperf_results(dirs["logs_dir"], dirs["reports_dir"], args.mode)


if __name__ == "__main__":
    main()
