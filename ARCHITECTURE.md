# NSR Architectural Deep-Dive: From C to High Resilience

This document outlines the architectural principles used to transform a standard C rewrite of the Rust-based Trippy tracer into a high-performance, fault-tolerant network analyzer built on **LibTTAK**.

Rather than pursuing raw throughput alone, NSR explores whether aggressive process isolation, deterministic execution, and extremely small deployment footprints can coexist with competitive real-world tracer performance.

---

# 1. Evolution of Isolation

NSR evolved through four architectural tiers.

Each tier increases fault isolation while attempting to preserve practical tracer throughput.

---

## Tier 1: Base NSR (Foundation)

### Concept
- Direct C23 rewrite of Trippy core tracer behavior
- LibTTAK event-driven architecture

### Properties
- Arena-based memory handling
- Lightweight packet processing
- Reduced runtime overhead

### Goal
- Achieve protocol parity with lower runtime footprint

---

## Tier 2: NSR Ultra (State Resilience)

### Concept
- Supervisor-worker model
- Generational shared memory

### Properties
- State survives worker crashes
- Automatic worker restart
- Capability-based state access

### Goal
- Prevent total tracer failure during worker faults

---

## Tier 3: NSR Singular (Privilege Separation)

### Concept

Multi-process tracing pipeline:

```text
Sender
Receiver
Broker
```

### Responsibilities

#### Sender
- outbound packet transmission only

#### Receiver
- inbound packet processing only

#### Broker
- event coordination
- state ownership

### Goal
- reduce fault blast radius

---

## Tier 4: NSR Omni (Intent Proxying)

### Concept

Separate low-privilege logic from high-privilege network execution.

```text
Logic Engine
    ↓
Intent Validation
    ↓
Gatekeeper
    ↓
Network Execution
```

### Logic Engine
- TTL scheduling
- sequence generation
- route logic
- state transitions

### Gatekeeper
- raw socket ownership
- packet transmission
- packet reception
- network boundary control

---

# 2. Fault Isolation Model

NSR intentionally avoids concentrating all responsibilities inside one process.

---

## Process Isolation

Network failures remain isolated from core logic.

---

## Capability Boundaries

Opaque handles reduce direct state exposure.

---

## Deterministic Scheduling

Execution cycles remain predictable under heavy tracing loads.

---

## State Recovery

Supervisor-driven restart allows partial recovery after failure.

---

## Reduced Residual State

Transient execution state is cleared after execution cycles.

---

# 3. Benchmark Results

Measured on Linux x86_64.

Dual-stack ICMPv4/v6 enabled.

---

## Full End-to-End Benchmark

Measured using:

```bash
sudo -E /usr/bin/time -v ./nsr_bench_e2e 127.0.0.1 10s
```

Measured output:

```text
Probes Sent: 1229760
Observations Recv: 549312
Elapsed Time: 9.98 s
Throughput: 123268.21 probes/sec
```

Later optimized run:

```text
Probes Sent: 1026848
Observations Recv: 833610
Elapsed Time: 9.99 s
Throughput: 102766.84 probes/sec
Maximum resident set size: 17936 KB
```

Peak run:

```text
~191k probes/sec
```

---

## NSR vs Trippy

| Metric | Trippy | NSR |
| :--- | :--- | :--- |
| Binary Size | ~9.2 MB | **23 KB** |
| RSS | ~28.5 MB | **~17.5 MB** |
| Throughput | ~150k probes/sec | **~191k probes/sec (peak)** |
| Protocol Support | IPv4 / IPv6 | IPv4 / IPv6 |
| Fault Isolation | Limited | Strong |

---

## Internal Primitive Benchmark

This benchmark does **not** represent full tracer throughput.

It measures isolated internal execution performance only.

```bash
make nsr_bench
./nsr_bench
```

Result:

```text
Integrity speed: ~5ns/op
```

This benchmark exists only to measure computational overhead of internal primitives.

It should not be interpreted as end-to-end tracer throughput.

---

# 4. Why NSR Is Small

---

## Native Compilation

- LTO
- aggressive optimization flags
- section garbage collection
- stripped binaries

---

## Minimal Runtime Overhead

No heavyweight async runtime layer.

---

## Explicit Resource Ownership

Memory behavior remains predictable and transparent.

---

# 5. Tradeoffs

NSR intentionally accepts additional architectural complexity in exchange for stronger isolation.

Compared to Trippy:

### NSR gains
- smaller binaries
- stronger fault boundaries
- process isolation
- recovery experimentation

### NSR loses
- simpler architecture
- easier maintenance
- ecosystem maturity

---

# 6. Engineering Conclusion

NSR demonstrates that C can still serve as a viable foundation for modern network tooling when paired with explicit architectural boundaries.

It remains:

- extremely small
- operationally fast
- fault-isolated
- experimentally ambitious

while maintaining competitive throughput against significantly heavier tracer implementations.
