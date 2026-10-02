# Results: WS suite Windows TX Inhibit pin time

Measured 2026-10-02 on `DESKTOP-E34PGI3` (Windows, Intel i5-8500T, 6 threads) against `C:\WSJT\ws-suite\build\ws.exe`. The source of that binary is commit `834eb8c` on `main`.

* Rig: Icom IC-9700. CAT on COM7 (CP210x). PTT method RTS on a separate port, COM6 (FTDI).
* Mode: FT8, dial 144.174 MHz. The probe did not answer a live decode. Tune held the radio in transmit. The Tune watchdog is 90 s, so Tune was clicked off and back on 29 times during the long run. Halt Tx at the end.
* UDP: `127.0.0.1:2237`, Id `WS`, Accept UDP requests on.
* Probe: `tools/probe_tx_inhibit_latency.py --ttl-ms 100 --interval-ms 137`. The script used on this PC is the wsjtx-inhibit probe, with a Windows `QueryPerformanceCounter` clock in place of `time.monotonic_ns()`.
* Clock: `QueryPerformanceCounter` converted to nanoseconds the same way as `TxInhibitDrop::monotonic_ns()`. Comparable to the type 17 stamps on this machine. Not comparable to Linux `CLOCK_MONOTONIC`.

The inhibit thread clears RTS with `EscapeCommFunction` on the handle it holds for COM6. `socket read → RTS` is that thread's stamp at the start of the clear until the write returns. `send → RTS` is the probe's stamp immediately before `sendto` until the same write return. Type 17 arrival is not pin time. The inhibit thread is an ordinary scheduler thread. The process priority class was Normal. The audio thread is time-critical. Nothing in the inhibit path raises its priority.

The Linux numbers this is compared with are in [wsjtx-inhibit `docs/RESULTS-ws-3.2.1-260926-inhibit-latency.md`](https://github.com/wa1hco/wsjtx-inhibit/blob/main/docs/RESULTS-ws-3.2.1-260926-inhibit-latency.md). That run was an i7-6700, IC-7300, RTS on the CAT port, and `CLOCK_MONOTONIC`. Its quiet transmit send → RTS was p50 0.33 ms, p99 0.74 ms, max 5.3 ms. Under an 8-core rebuild the transmit max was 41 ms.

CSVs:

* [win-latency/inhibit-latency-30min.csv](win-latency/inhibit-latency-30min.csv)
* [win-latency/inhibit-latency-load.csv](win-latency/inhibit-latency-load.csv)

## Tune, desktop otherwise idle, 30 minutes

Span 1785 s. 12,178 holds. 177 holds have no pin stamp. Those are the gaps while Tune was clicked off and back on. `udp-dispatch` stayed at normal priority.

| | n | p50 | p99 | max | ≥ 5 ms | ≥ 30 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Transmit, socket read → RTS | 11862 | 0.23 ms | 0.47 ms | 1.31 ms | 0 | 0 |
| Transmit, send → RTS | 11862 | 0.49 ms | 1.13 ms | 2.35 ms | 0 | 0 |
| Idle, send → RTS | 139 | 0.48 ms | 1.42 ms | 1.91 ms | 0 | 0 |

Socket read → RTS is the COM write after the inhibit thread has the packet. Send → RTS adds the localhost handoff and the thread wake onto that clear. No hold reached 5 ms. On transmit, 218 of 11,862 holds took 1 ms or more from send to the pin. Seven of those spent 1 ms or more inside the clear itself. The maximum clear was 1.31 ms.

Type 17 arrival during transmit was p50 1.7 ms, p99 63 ms, max 358 ms. That is the status datagram, not the pin.

The median is a little slower than the quiet Linux transmit run (0.49 ms against 0.33 ms). The tail is shorter (2.35 ms against 5.3 ms, and against the 9.8 ms idle maximum on that machine).

## Six cores busy, 15 seconds

Six tight loops, one per core, so the i5-8500T sat at 100 percent. This is not a `cmake -j` rebuild. Span 12 s. 81 holds. Tune was on and every hold dropped a high pin. The probe filed the rows as idle because `Status.Transmitting` stayed false for this short sweep.

| | n | p50 | p99 | max | ≥ 5 ms | ≥ 30 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Send → RTS | 81 | 0.34 ms | 0.76 ms | 0.78 ms | 0 | 0 |
| Socket read → RTS | 81 | 0.16 ms | 0.51 ms | 0.62 ms | 0 | 0 |

Nothing reached 1 ms. A 15 second sample cannot show a stall that happens a few times in half an hour. It also does not reproduce the Linux rebuild tail.
