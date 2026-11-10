"""A display in a terminal: connects to the hub like the firmware does and
prints what it is sent. Handy for checking the hub without hardware.

    python tools/fake_display.py --id cyd-test1
    python tools/fake_display.py --id cyd-test1 --act light.kitchen toggle
"""
import argparse
import asyncio
import json

import aiohttp


async def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--url", default="ws://localhost:8390/ws")
    p.add_argument("--id", default="cyd-fake01")
    p.add_argument("--act", nargs="+", metavar=("ENTITY", "ACTION"),
                   help="send one action after the layout arrives: ENTITY ACTION [VALUE]")
    p.add_argument("--seconds", type=float, default=0, help="stop after this long (0: run until ^C)")
    args = p.parse_args()

    async with aiohttp.ClientSession() as http, http.ws_connect(args.url) as ws:
        await ws.send_json({"t": "hello", "id": args.id, "name": args.id, "fw": "fake", "proto": 1, "w": 320, "h": 240})
        loop = asyncio.get_running_loop()
        stop = loop.time() + args.seconds if args.seconds else None
        while True:
            timeout = max(0.1, stop - loop.time()) if stop else None
            try:
                msg = await ws.receive(timeout=timeout)
            except asyncio.TimeoutError:
                return
            if msg.type != aiohttp.WSMsgType.TEXT:
                print("closed")
                return
            m = json.loads(msg.data)
            print(json.dumps(m, ensure_ascii=False))
            if m["t"] == "layout" and args.act:
                entity, action, *value = args.act
                out = {"t": "act", "e": entity, "a": action}
                if value:
                    out["v"] = float(value[0]) if value[0].replace(".", "", 1).isdigit() else value[0]
                await ws.send_json(out)
                args.act = None


if __name__ == "__main__":
    asyncio.run(main())
