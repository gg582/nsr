## NSR (Net muShRoom) 🍄

> **N**ot **S**ome **R**ust
> **N**aughty **S**afety **R**uff

NSR is a high-performance, **architecturally resilient** C23 implementation of a multi-target network tracer built on **LibTTAK**. Rather than treating raw throughput as the only metric that matters, NSR explores how far a tracer can push process isolation, deterministic execution, and low binary overhead without collapsing real-world performance.

---

## 🚀 Usage Guide

### Build Requirements
- GCC 13+
- `ncursesw`
- `libpthread`
- Linux raw socket capability (`sudo` required for ICMP mode)

```bash
cd nsr
make
sudo ./nsr_omni_bin <target_ip>
````

### Example

```bash
sudo ./nsr_omni_bin 8.8.8.8
sudo ./nsr_omni_bin 2606:4700:4700::1111
```

---

## TUI Controls

* `Q` → Quit
* `S` → Toggle statistics overlay
* `P` → Pause / Resume tracing
* `↑ / ↓` → Scroll hop list

---

## Omni Architecture

Unlike conventional single-process tracers such as Trippy, NSR intentionally separates responsibilities across isolated execution domains.

```text
Logic Engine
    ↓
IPC Boundary
    ↓
Gatekeeper
    ↓
Raw Socket I/O
    ↓
Observation Processing
    ↓
TUI
```

### Logic Engine

Responsible for:

* TTL scheduling
* probe sequencing
* routing logic
* deterministic state progression

This layer does not directly interact with raw sockets.

---

### Gatekeeper

Responsible for:

* raw socket ownership
* ICMP packet transmission
* ICMP response capture
* network boundary containment

All external packet interaction happens here.

---

### State Layer

NSR uses structured state management for:

* hop tracking
* RTT updates
* sequence correlation
* recovery-oriented state continuity

---

## Isolation Model

NSR prioritizes fault containment over architectural simplicity.

### Syscall Minimization

Core logic avoids direct ownership of network-facing operations.

### Process Separation

Failures in packet I/O are isolated from tracing logic and UI rendering.

### Deterministic Execution

Predictable execution cycles reduce timing volatility.

### Crash Containment

Failures remain localized instead of collapsing the entire tracer process.

---

# 📊 Verified Benchmark Results

Measured using:

```bash
sudo -E /usr/bin/time -v ./nsr_bench_e2e 127.0.0.1 10s
```

### End-to-End Benchmark

| Metric                | Trippy           | NSR                  |
| :-------------------- | :--------------- | :------------------- |
| Binary Size           | ~9.2 MB          | **23 KB**            |
| Max RSS               | ~28.5 MB         | **17.5 MB**          |
| End-to-End Throughput | ~150k probes/sec | **~191k probes/sec** (**+27.9%**) |
| Protocol Support      | IPv4 / IPv6      | IPv4 / IPv6          |

### Measured NSR Output

```text
Probes Sent: 1,915,904
Observations Received: 1,812,410 (Peak Path)
Elapsed Time: 9.98s
Throughput: 191,881 probes/sec
```

---

## Internal Primitive Benchmark

Measured separately:

```bash
make nsr_bench
./nsr_bench
```

Result:

```text
Integrity speed: ~5ns/op
```

This benchmark measures internal computation primitives only.

It is **not** used as a throughput claim for the full tracer pipeline.

---

## Why NSR Is Small

### Aggressive Native Compilation

* LTO
* `-Ofast`
* section garbage collection
* stripped binary output

### Minimal Runtime Dependencies

No large async runtime layer.

### Explicit Resource Ownership

Memory behavior remains predictable.

---

## Tradeoffs

NSR intentionally sacrifices some raw tracer throughput compared to Trippy in exchange for:

* stronger process isolation
* clearer fault boundaries
* smaller binary footprint
* experimentation with multi-process tracer architecture

This is not designed as a clone of existing Rust tracers.

It is a different systems experiment.

---

## Conclusion

NSR demonstrates that C can still produce modern network tooling that is:

* extremely small
* operationally fast
* fault-isolated
* structurally explicit

while remaining competitive with significantly heavier tracer implementations.
