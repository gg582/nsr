#!/usr/bin/env python3
"""
NSR Python Plugin Template
This template provides a skeleton for creating custom NSR plugins in Python.
"""
import sys
import json

def send(obj):
    """Send a JSON-RPC response or notification to NSR."""
    print(json.dumps(obj), flush=True)

def main():
    # Process JSON-RPC messages line by line from stdin
    for line in sys.stdin:
        try:
            req = json.loads(line.strip())
        except Exception:
            continue
            
        method = req.get("method")
        req_id = req.get("id")
        params = req.get("params", {})
        
        if method == "init":
            # Respond to the initialization request.
            # You can also return "reserved_keys" if your plugin needs custom key bindings.
            send({
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "status": "ok",
                    "description": "Python Plugin Template"
                }
            })
        elif method == "cleanup":
            # NSR is exiting or disabling this plugin.
            # Perform any cleanup if necessary.
            pass
        elif method == "update_telemetry":
            # Notification sent on every telemetry tick (no response needed)
            # params contains: target_ip, interval_ms, and hops list
            hops = params.get("hops", [])
            pass
        elif method == "render_hops":
            # Request to annotate hops in the TUI normal view.
            # Return a list of annotations for specific hop indexes.
            send({
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "annotations": []
                }
            })
        elif method == "render":
            # Request to draw content in the plugin's TUI panel.
            # Return list of text lines with optional coordinates and colors.
            # Available colors: cyan, green, yellow, red, magenta, blue, white, highlight
            send({
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "lines": [
                        {"y": 1, "x": 1, "text": "Python Plugin Template Active", "color": "cyan"},
                        {"y": 2, "x": 1, "text": "Modify this plugin at ~/.nsr/plugins/", "color": "white"}
                    ]
                }
            })
        elif method == "on_key":
            # Key event forwarded by NSR.
            # Return handled: True to consume the key and block default NSR behavior.
            send({
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "handled": False
                }
            })

if __name__ == "__main__":
    main()
