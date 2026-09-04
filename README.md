# opta-gateway-mt

The IP2 Opta gateway on two threads: a PLC thread that owns the EtherNet/IP
client and a main thread that owns every Arduino Cloud property, sharing one
mutex-guarded snapshot and one command queue.

Design and batch plan:
`../docs/superpowers/specs/2026-09-04-opta-gateway-mt-design.md`

Build with `tools/build.sh` only. It generates `version.h` from `git describe`;
the Arduino IDE skips that step and produces a firmware with no version, which
OTA cannot tolerate.

Batch 0 (this repository's first milestone): PLC thread reads the 28 sensor
tags into a snapshot; main publishes them. Nothing else is migrated yet.
