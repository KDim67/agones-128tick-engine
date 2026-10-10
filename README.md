# agones-128tick-engine

Authoritative 128 Hz dedicated server for a small-arena shooter, written in C++20 on top of ENet. It runs as an Agones GameServer on Kubernetes, exports Prometheus metrics, and ships with a headless load generator that plays real matches against it through netem-impaired loopback and fails the run when the server misses its budget.

There is no client. The bots in `tools/loadgen` implement the client side of the protocol (prediction, reconciliation, interpolation, lag-compensated shots) so the server can be measured end to end.

## Measured

GitHub-hosted runner (2 vCPU), 2 servers x 10 bots, from CI:

| Profile | Tick p99 | Start lateness p99 | Overruns | Hit confirmation | Hit error p99 |
|---|---|---|---|---|---|
| baseline, 20 s | < 250 us | < 50 us | 0 / 2560 | 100% | 0.000 mm |
| degraded, 30 s | < 250 us | < 50 us | 0 / 3840 | 100% | 0.000 mm |

Tick figures are Prometheus histogram bucket bounds; the budget is 7812.5 us.

Desktop, Docker, 6 servers x 10 bots, 60 s:

| Profile | Snapshots lost / reordered | Bandwidth per client | Hit confirmation | Hit error p99 |
|---|---|---|---|---|
| baseline | 0% / 0% | 110 kbps, 100% delta | 100% | 0.000 mm |
| degraded | 6.0% / 46.6% | 118 kbps, 100% delta | 100% | 0.000 mm |

Full-state snapshots cost 500.7 kbps per client before delta encoding. With input redundancy off (`--redundancy 1`) on the degraded profile, input loss goes to 5.28%, hit confirmation drops to 91.9% and hit error p99 to 240 mm, and the run fails.

## How the tick works

`TickLoop` runs a fixed 7.8125 ms step. It sleeps until 1.5 ms before the deadline and spins the rest with a pause instruction, catches up at most 4 ticks after a stall, and asks for `SCHED_FIFO` (or `THREAD_PRIORITY_TIME_CRITICAL` on Windows) where the process is allowed to have it.

Each tick of `AuthoritativeServer` does, in order: drain ENet events, check session timeouts once a second, send RTT probes at 16 Hz, consume each client's inputs and resolve their shots, run respawns, record the frame into history, and send every client its snapshot.

### Inputs

Every input packet carries the newest four inputs, so a single lost datagram costs nothing. Inputs land in a 64-slot reorder buffer indexed by sequence and are simulated strictly in order. A hole waits 8 ticks for a late or redundant copy before the gap is skipped and charged as loss. A token bucket caps a client at 4 inputs per tick, which bounds catch-up bursts after a held uplink.

### Lag compensation

The server keeps one second of history (128 frames). A shot names the two ticks and the sub-tick fraction the shooter was rendering, and the server replays that exact interpolation before tracing the ray against head, torso and leg boxes, clipped by arena geometry. A rewind may reach back RTT + 24 interpolation ticks + 16 ticks of slack and no further, and never across a respawn or reconnect. Every shot gets a `ShotResult` back with its verdict and the set of valid targets at the rewound tick; the victim gets a `HitNotification`.

### Snapshots

State is quantized to 1/512 m for positions, 1/256 m/s for velocity and 2^16 steps per turn for angles. Each client gets a delta against the last frame it acknowledged (acks ride in the input stream), out of a 64-frame ring; a client with no usable baseline gets a full frame. The client's own entity is always sent at full precision for prediction. A snapshot for 10 players is at most 212 bytes. History records the dequantized positions, so the server rewinds against exactly what clients drew and hit error is zero, not merely small.

### Memory

ENet's allocator is replaced through `enet_initialize_with_callbacks` with a fixed-block pool (64 to 2048 byte classes, 1088 KiB) that is sealed before the loop starts. A request the pool cannot serve fails and ENet drops that packet; nothing reaches `malloc` from the tick. `test_allocation` replaces global `operator new`, drives ten real ENet clients through kills, respawns, a rejoin, a fire spammer and a peer that stops acking, and fails on any allocation inside a live tick.

### Misbehaving clients

Weapon cooldown of 13 ticks counted in the client's own input sequence, so late bursts are not mistaken for rapid fire. At most 64 unacknowledged reliable packets per peer before the peer is cut off. View angles are validated and clamped, malformed packets are counted and dropped.

### Agones

All SDK traffic runs on its own threads. `Ready()` waits for the first completed tick, and `Health()` is withheld if the tick counter stops moving, so a wedged loop gets the pod replaced. A Ready server that players connect to directly allocates itself so a fleet scale-down cannot take it. An allocated server left empty past `--empty-grace` seconds shuts down.

## Layout

```
include/engine/     headers
src/                server; AgonesBridge.cpp with the SDK, AgonesBridgeNull.cpp without
tests/              simulation, protocol and codec, telemetry, orchestration, allocation audit
tools/loadgen/      bot client and benchmark verdicts
tools/bench/        run_benchmark.sh: N servers + netem + load generator, exit code is the verdict
tools/netem/        impair.sh: UDP-only netem on one interface, TCP scrapes stay clean
deploy/agones/      GameServer, Fleet, FleetAutoscaler, GameServerAllocation
deploy/monitoring/  PodMonitor for prometheus-operator
dashboards/         Grafana dashboard
```

## Building

Needs CMake 3.20 and a C++20 compiler (GCC 14 and MSVC 2022 are what CI uses). ENet is fetched by CMake.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

| Option | Default | |
|---|---|---|
| `ENGINE_WITH_AGONES` | OFF | link the Agones C++ SDK; needs gRPC and the SDK installed, easiest through Docker |
| `ENGINE_BUILD_TOOLS` | ON | build `load_generator` |
| `ENABLE_SANITIZERS` | OFF | ASan + UBSan |

Running it:

```bash
./build/game_server --port 7777 --metrics-port 9090
```

`--port` UDP game port, `--metrics-port` HTTP port for `/metrics`, `--agones` talk to the SDK sidecar, `--empty-grace S` seconds an allocated server may sit empty, `--benchmark S` exit after S seconds.

### Images

```bash
docker build --target runtime -t agones-128tick-engine .
docker build --target bench -t agones-128tick-engine-bench .
```

`runtime` is distroless, non-root, about 39 MiB, and is the one Agones runs. It builds gRPC from source, which takes about an hour on a 2-core machine the first time; after that the layer is cached. `bench` holds the server, the load generator and the scripts, built without Agones, so it skips gRPC entirely.

## Benchmarking

```bash
docker run --rm --cap-add NET_ADMIN -v "$PWD/reports:/reports" \
  agones-128tick-engine-bench --profile degraded --servers 6 --duration 60
```

`NET_ADMIN` is for netem on the container's loopback. Profiles, applied per direction to UDP only:

| Profile | Delay | Jitter | Loss | RTT |
|---|---|---|---|---|
| `baseline` | 0 | 0 | 0 | ~0 |
| `spec` | 25 ms | 5 ms, normal | 5% | ~50 ms |
| `degraded` | 60 ms | 15 ms, normal | 5% | ~120 ms |

`--delay`, `--jitter` and `--loss` override a profile; `--redundancy 1` turns off input redundancy as a control. Normal-distributed jitter reorders packets the way a real path does, which is what exercises the reorder buffer.

The load generator scrapes every server before and after the measured window and fails the run unless:

- every bot connected and none dropped
- per server: tick p99 and start lateness p99 within the budget, overruns at most 0.1% of ticks, at least 127.5 TPS
- input loss after redundancy at most 0.1%
- at least 98% of hits confirmed by the server, hit position error p99 at most 1 mm
- no malformed or undecodable packets, no pool refusals, no reliable backlog cutoffs
- no throttled shots (bots and server agree on the cooldown)
- snapshot bandwidth at most 160 kbps per client

Reports land in `/reports` as JSON plus each server's log. The harness also times its own wake-ups; if it was late at the same moments the servers were, the host was busy and the run says nothing about the engine.

## Deploying

Requires a cluster with Agones installed, and prometheus-operator for the PodMonitor.

```bash
kubectl apply -f deploy/agones/fleet.yaml
kubectl apply -f deploy/agones/fleetautoscaler.yaml
kubectl apply -f deploy/monitoring/podmonitor.yaml
kubectl create -f deploy/agones/allocation.yaml -o yaml
```

The fleet uses dynamic UDP ports and packed scheduling, and the autoscaler keeps two Ready servers ahead of demand (2 to 20 replicas). Pods get 1 CPU and 128 MiB with requests equal to limits, a read-only root filesystem and all capabilities dropped. `gameserver.yaml` is a single GameServer for smoke tests.

The manifests pull `ghcr.io/kdim67/agones-128tick-engine:0.1.0`. CI pushes that image when a `v*` tag is pushed, after every other job passes, with provenance and an SBOM.

Import `dashboards/game_server_dashboard.json` into Grafana for tick budget, lateness, rewinds, input pipeline, transport health, snapshot bandwidth and fleet state.

## Metrics

Served as Prometheus text on `--metrics-port`.

| Series | |
|---|---|
| `game_tick_duration_us`, `game_tick_lateness_us`, `game_tick_jitter_us` | histograms |
| `game_tick_overruns`, `game_total_ticks`, `game_uptime_seconds` | |
| `game_active_players`, `game_agones_state` | |
| `game_client_inputs`, `game_input_backlog_peak`, `game_malformed_packets` | input pipeline |
| `game_ballistic_shots`, `game_confirmed_hits`, `game_occluded_shots`, `game_rejected_rewinds`, `game_throttled_shots` | shots |
| `game_snapshot_bytes`, `game_snapshots{encoding="delta\|full"}` | replication |
| `game_net_pool_peak_bytes`, `game_net_pool_failures`, `game_reliable_backlog_disconnects` | transport |

## CI

`.github/workflows/ci.yml` runs on pushes to `main`, pull requests and release tags: MSVC `/W4 /WX`, GCC 14 with conversion and shadowing warnings as errors, and GCC 14 under ASan + UBSan followed by a sanitized match against real bots. The benchmark job builds the bench image, loads the netem modules on the runner, and runs a baseline and a degraded match through the gates above. On `main` the runtime image is built and held under 80 MiB.
