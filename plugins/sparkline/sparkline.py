#!/usr/bin/env python3
import sys
import json
from collections import defaultdict, deque

# Track the last 20 RTTs per hop
MAX_HISTORY = 20
rtt_history = defaultdict(lambda: deque(maxlen=MAX_HISTORY))
loss_history = defaultdict(lambda: deque(maxlen=MAX_HISTORY))

# Unicode sparkline blocks
SPARK_CHARS = " ▂▃▄▅▆▇█"

def make_sparkline(rtts):
    if not rtts:
        return ""
    # Filter out None/timeouts
    valid_rtts = [r for r in rtts if r is not None and r >= 0]
    if not valid_rtts:
        return "X" * len(rtts)
    
    min_val = min(valid_rtts)
    max_val = max(valid_rtts)
    range_val = max_val - min_val
    
    spark = []
    for r in rtts:
        if r is None or r < 0:
            spark.append("X")  # Loss indicator
        elif range_val == 0:
            spark.append(SPARK_CHARS[3])  # Flat line at medium height
        else:
            idx = int(((r - min_val) / range_val) * 7)
            idx = max(0, min(7, idx))
            spark.append(SPARK_CHARS[idx])
    return "".join(spark)

def send(obj):
    print(json.dumps(obj), flush=True)

def main():
    for line in sys.stdin:
        try:
            req = json.loads(line.strip())
        except Exception:
            continue
            
        method = req.get("method")
        req_id = req.get("id")
        params = req.get("params", {})
        
        if method == "init":
            send({
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "status": "ok",
                    "description": "RTT Sparkline & Trend Tracker"
                }
            })
        elif method == "cleanup":
            pass
        elif method == "update_telemetry":
            hops = params.get("hops", [])
            for hop in hops:
                idx = hop.get("hop_idx")
                status = hop.get("status")
                
                if status == "timeout":
                    rtt_history[idx].append(None)
                    loss_history[idx].append(1)
                else:
                    rtt_us = hop.get("rtt_us", 0)
                    rtt_ms = rtt_us / 1000.0
                    rtt_history[idx].append(rtt_ms)
                    loss_history[idx].append(0)
        elif method == "render_hops":
            annotations = []
            hops = params.get("hops", [])
            for hop in hops:
                idx = hop.get("hop_idx")
                history = list(rtt_history[idx])
                if history:
                    spark = make_sparkline(history)
                    latest = [h for h in history if h is not None]
                    latest_val = latest[-1] if latest else 0.0
                    annotations.append({
                        "hop_idx": idx,
                        "text": f"[{spark}] {latest_val:.1f}ms"
                    })
            send({
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "annotations": annotations
                }
            })
        elif method == "render":
            focused_idx = params.get("focused_node_id")
            focused_addr = params.get("focused_addr")
            
            lines = []
            if focused_idx is not None and focused_idx in rtt_history and rtt_history[focused_idx]:
                history = list(rtt_history[focused_idx])
                losses = list(loss_history[focused_idx])
                
                valid_rtts = [r for r in history if r is not None]
                loss_pct = (sum(losses) / len(losses)) * 100 if losses else 0.0
                
                if valid_rtts:
                    min_rtt = min(valid_rtts)
                    max_rtt = max(valid_rtts)
                    avg_rtt = sum(valid_rtts) / len(valid_rtts)
                    spark = make_sparkline(history)
                    
                    lines.append({"y": 1, "x": 1, "text": f"Hop {focused_idx} ({focused_addr}) - RTT Historical Trend", "color": "highlight"})
                    lines.append({"y": 2, "x": 1, "text": f"Trend: [{spark}]", "color": "cyan"})
                    lines.append({"y": 3, "x": 1, "text": f"Min: {min_rtt:.1f}ms | Max: {max_rtt:.1f}ms | Avg: {avg_rtt:.1f}ms", "color": "green"})
                    lines.append({"y": 4, "x": 1, "text": f"Loss Rate: {loss_pct:.1f}% (Last {len(history)} probes)", "color": "yellow" if loss_pct > 0 else "green"})
                else:
                    lines.append({"y": 1, "x": 1, "text": f"Hop {focused_idx} ({focused_addr}) - All Timeout", "color": "red"})
            else:
                lines.append({"y": 1, "x": 1, "text": "RTT Sparkline Plugin Active", "color": "cyan"})
                lines.append({"y": 2, "x": 1, "text": "Focus a hop to view detailed RTT history.", "color": "white"})
                
            send({
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "lines": lines
                }
            })
        elif method == "on_key":
            send({
                "jsonrpc": "2.0",
                "id": req_id,
                "result": {
                    "handled": False
                }
            })

if __name__ == "__main__":
    main()
