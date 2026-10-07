"""Small deterministic external command used by integration tests."""
import json
import os
import signal
import sys
import time

mode, *args = sys.argv[1:]
if mode == "args":
    print(json.dumps(args))
elif mode == "streams":
    print("out")
    print("err", file=sys.stderr)
elif mode == "exit":
    sys.exit(int(args[0]))
elif mode == "wait":
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    print("ready", flush=True)
    time.sleep(30)
elif mode == "new-session":
    os.setsid()
    time.sleep(0.05)
else:
    raise SystemExit("unknown probe mode")
