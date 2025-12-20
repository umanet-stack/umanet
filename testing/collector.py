#!/usr/bin/env python3
import asyncio
import json
import subprocess
from pathlib import Path
from datetime import datetime
import sys

OUTDIR = Path("testing/results")
OUTDIR.mkdir(parents=True, exist_ok=True)

if OUTDIR.exists():
    for p in OUTDIR.iterdir():
        if p.is_file():
            p.unlink()
else:
    OUTDIR.mkdir(parents=True)

RAW_LOG = OUTDIR / "raw.jsonl"
DEBUG_LOG = OUTDIR / "debug.log"


async def handle_vm(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    addr = writer.get_extra_info("peername")
    ts = datetime.now().strftime("%H:%M:%S")

    try:
        data = await reader.read()  # read until EOF
        if not data:
            return

        line = data.decode(errors="replace").strip()
        obj = json.loads(line)

        vm = obj.pop("vm")
        out_file = OUTDIR / f"{vm}.json"

        # atomic write
        out_file.write_text(json.dumps(obj))

        # append raw log
        with RAW_LOG.open("a") as f:
            f.write(line + "\n")

    except Exception as e:
        with DEBUG_LOG.open("a") as f:
            f.write(f"[{ts}] ERROR from {addr}: {e}\n")

    finally:
        writer.close()
        await writer.wait_closed()


async def monitor_loop():
    """Mimics the shell monitor output."""
    while True:
        try:
            # pgrep -c cloud-hyp
            proc = subprocess.run(
                ["pgrep", "-c", "cloud-hyp"],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
            )
            vm_count = int(proc.stdout.strip() or 0)
        except Exception:
            vm_count = 0

        results = len(list(OUTDIR.glob("*.json")))
        ts = datetime.now().strftime("%H:%M:%S")

        sys.stderr.write(
            f"\r[{ts}] Running VMs: {vm_count} | Results collected: {results}   "
        )
        sys.stderr.flush()

        await asyncio.sleep(2)


async def main():
    server = await asyncio.start_server(
        handle_vm,
        host="0.0.0.0",
        port=9000,
        limit=2 * 1024 * 1024,  # 2MB per VM
    )

    print("\n🔥 asyncio collector listening on port 9000\n", file=sys.stderr)

    async with server:
        await asyncio.gather(
            server.serve_forever(),
            monitor_loop(),
        )


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n👋 collector stopped", file=sys.stderr)
