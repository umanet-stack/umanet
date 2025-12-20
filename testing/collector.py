#!/usr/bin/env python3
import asyncio
import json
from pathlib import Path
from datetime import datetime

OUTDIR = Path("testing/results")
OUTDIR.mkdir(parents=True, exist_ok=True)

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

        with DEBUG_LOG.open("a") as f:
            f.write(f"[{ts}] received from {addr}, vm={vm}\n")

    except Exception as e:
        with DEBUG_LOG.open("a") as f:
            f.write(f"[{ts}] ERROR from {addr}: {e}\n")

    finally:
        writer.close()
        await writer.wait_closed()


async def main():
    server = await asyncio.start_server(
        handle_vm,
        host="0.0.0.0",
        port=9000,
        limit=1024 * 1024,  # 1MB per connection
    )

    print("🔥 asyncio collector listening on port 9000")

    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
