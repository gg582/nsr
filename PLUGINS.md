# NSR Plugin System

NSR loads plugins as **separate processes** and talks to them through **JSON-RPC 2.0 over stdin/stdout**. This lets you write plugins in any language.

## Built-in plugins

Two executable plugins are provided in the `plugins/` directory:

| Name         | Description                                              |
| :----------- | :------------------------------------------------------- |
| `geoip`      | Resolve hop IPs to country and provider.                 |
| `percentile` | Show p99.9 / p99 / p50 / p1 / p0.1 RTT percentiles.      |

They are inactive by default. Enable them from the **Tools** menu (`t`) in the TUI.

## Installing plugins

```bash
make plugins-install
```

This compiles the built-in plugins and copies the executables into `~/.nsr/plugins/`.

## Plugin discovery

At startup NSR scans `~/.nsr/plugins/` for executable files. The filename becomes the plugin name. To install a third-party plugin, just drop an executable there:

```bash
cp myplugin ~/.nsr/plugins/
```

## Tools menu controls

* `t` — Open the **Tools** plugin menu.
* `j` / `k` — Move cursor up/down.
* `h` / `l` — Move cursor by 5 items.
* `Enter` — Toggle the selected plugin on/off.
* `Esc` / `q` — Close the menu.

Enabled state is persisted in `~/.nsrconfig` as `NSR_PLUGIN_<name>=1`.

## JSON-RPC protocol

All messages are single-line JSON objects terminated by `\n`.

### NSR → plugin: `init`

Request:
```json
{"jsonrpc":"2.0","id":1,"method":"init","params":{"config_path":"/home/user/.nsrconfig","plugin_dir":"/home/user/.nsr/plugins"}}
```

Response:
```json
{"jsonrpc":"2.0","id":1,"result":{"status":"ok","description":"My plugin"}}
```

A plugin may also reserve keys so that `on_key` events for those keys are routed only to it:

```json
{"jsonrpc":"2.0","id":1,"result":{"status":"ok","description":"My plugin","reserved_keys":"t,q"}}
```

Keys are caseless. If a reserved key conflicts with a built-in NSR shortcut, NSR moves the built-in action to a free slot from `custom_keys` and updates the footer labels dynamically.

### NSR → plugin: `cleanup`

Notification (no `id`):
```json
{"jsonrpc":"2.0","method":"cleanup","params":{}}
```

### NSR → plugin: `update_telemetry`

Notification sent on every telemetry tick:
```json
{
  "jsonrpc": "2.0",
  "method": "update_telemetry",
  "params": {
    "target_ip": "8.8.8.8",
    "interval_ms": 100,
    "hops": [
      {
        "hop_idx": 1,
        "addr": "192.168.1.1",
        "rtt_us": 1234,
        "sent": 10,
        "recv": 10,
        "loss": 0.0,
        "status": "exceeded"
      }
    ]
  }
}
```

`status` is one of: `reply`, `exceeded`, `unreachable`, `timeout`, `probing`.

### NSR → plugin: `render`

Request for panel drawing:
```json
{"jsonrpc":"2.0","id":2,"method":"render","params":{"mode":"normal","width":40,"height":20,"focused_node_id":0,"focused_addr":""}}
```

Response:
```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "lines": [
      {"y": 1, "x": 1, "text": "p99.9 12.34 ms", "color": "cyan"}
    ]
  }
}
```

`color` is optional and may be `cyan`, `green`, `yellow`, `red`, `magenta`, `blue`, `white`, or `highlight`. `y`/`x` are relative to the panel origin given by NSR.

### NSR → plugin: `render_hops`

Request for hop annotations in Normal view:
```json
{"jsonrpc":"2.0","id":3,"method":"render_hops","params":{"hops":[{"hop_idx":1,"addr":"...",...}]}}
```

Response:
```json
{"jsonrpc":"2.0","id":3,"result":{"annotations":[{"hop_idx":1,"text":"[US ISP]"}]}}
```

### NSR → plugin: `on_key`

Request when a key is pressed in Normal/Grid/Tree modes:
```json
{"jsonrpc":"2.0","id":4,"method":"on_key","params":{"key":109}}
```

Response:
```json
{"jsonrpc":"2.0","id":4,"result":{"handled":true}}
```

Return `handled:false` to let NSR process the key.

## Example Python plugin

Save as `~/.nsr/plugins/myplugin` and `chmod +x` it:

```python
#!/usr/bin/env python3
import sys, json

def send(obj):
    print(json.dumps(obj), flush=True)

for line in sys.stdin:
    req = json.loads(line)
    method = req.get("method")
    req_id = req.get("id")

    if method == "init":
        send({"jsonrpc": "2.0", "id": req_id,
              "result": {"status": "ok", "description": "Python demo plugin"}})
    elif method == "update_telemetry":
        pass
    elif method == "render":
        send({"jsonrpc": "2.0", "id": req_id,
              "result": {"lines": [{"y": 1, "x": 1, "text": "Hello from Python", "color": "cyan"}]}})
    elif method == "render_hops":
        send({"jsonrpc": "2.0", "id": req_id,
              "result": {"annotations": []}})
    elif method == "on_key":
        send({"jsonrpc": "2.0", "id": req_id,
              "result": {"handled": False}})
```

## Notes

* Plugins must read from `stdin` and write to `stdout`. All writes must be flushed immediately.
* `init` is still a synchronous call with a 2000 ms timeout.
* `render` / `render_hops` are handled asynchronously: NSR draws the previous frame's cached response immediately, polls for the current response with a 20 ms budget, and then issues a new request for the next frame. Slow plugins no longer block the TUI.
* `on_key` is sent with a tight 10 ms response budget; if the plugin does not answer in time NSR treats the key as unhandled and processes it normally.
* `update_telemetry` is a notification; NSR does not wait for a response.
* If a plugin process exits, NSR marks it dead and will not restart it until re-enabled.
