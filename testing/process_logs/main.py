import argparse
import os
import subprocess
import sys
from pathlib import Path
from typing import Dict

SCRIPT_DIR = Path(__file__).parent.resolve()


def load_environment():
    repo_root = SCRIPT_DIR.parent.parent
    env_sh = repo_root / "env.sh"

    if not env_sh.exists():
        print(f"Warning: env.sh not found at {env_sh}")
        print(f"   You may need to manually set TEST environment variable")
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


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Process test results and generate reports",
        epilog="Set TEST environment variable to 'iperf', 'iperf-udp', or 'sockperf' to specify test type",
    )
    parser.add_argument(
        "folder",
        choices=["dpdk", "tap", "dpdk-tap", "ovs-dpdk"],
        help="Folder name: 'dpdk', 'tap', 'dpdk-tap', or 'ovs-dpdk'",
    )
    parser.add_argument(
        "mode",
        choices=["vm-vm-internal", "vm-client"],
        help="Processing mode: 'vm-vm-internal' (process only odd VMs) or 'vm-client' (process all VMs)",
    )
    return parser.parse_args()


def setup_directories(folder: str, mode: str, test_type: str) -> Dict[str, Path]:
    base_dir = SCRIPT_DIR.parent / folder
    logs_dir = base_dir / "logs"
    test_env = os.environ.get("TEST", "").lower()
    if test_type == "iperf" and test_env == "iperf-udp":
        report_folder = "iperf-udp"
    else:
        report_folder = test_type
    reports_base_dir = base_dir / report_folder / mode

    reports_base_dir.mkdir(exist_ok=True, parents=True)

    if not logs_dir.exists():
        print(f"Logs directory not found: {logs_dir}")
        print(f"   Please ensure log files are in: {logs_dir}/")
        sys.exit(1)

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

    if test_env in ["iperf", "iperf3", "iperf-udp"]:
        return "iperf"
    elif test_env in ["sockperf", "latency"]:
        return "sockperf"
    else:
        print(
            f"ERROR: TEST environment variable must be set to 'iperf', 'iperf-udp', or 'sockperf'"
        )
        print(f"   Current value: TEST='{os.environ.get('TEST', '(not set)')}'")
        print()
        print("Usage:")
        print("  export TEST=iperf       # For TCP iperf tests")
        print(
            "  export TEST=iperf-udp   # For UDP iperf tests (auto-detected from logs)"
        )
        print("  python testing/process_logs/main.py dpdk vm-client")
        print()
        print("  export TEST=sockperf")
        print("  python testing/process_logs/main.py tap vm-vm-internal")
        sys.exit(1)


def main():
    load_environment()

    test_type = detect_test_type()

    args = parse_arguments()

    dirs = setup_directories(args.folder, args.mode, test_type)

    print(f"Processing {test_type} results...")
    print(f"- Base folder: {dirs['base_dir']}")
    print(f"- Logs folder: {dirs['logs_dir']}")
    print(f"- Reports folder: {dirs['reports_dir']}")
    print()

    if test_type == "iperf":
        from process_iperf import process_iperf_results

        process_iperf_results(dirs["logs_dir"], dirs["reports_dir"], args.mode)
    elif test_type == "sockperf":
        from process_sockperf import process_sockperf_results

        process_sockperf_results(dirs["logs_dir"], dirs["reports_dir"], args.mode)


if __name__ == "__main__":
    main()
