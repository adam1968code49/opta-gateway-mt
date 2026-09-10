# 批 10 终审记录 — DNS 证据 + 离线事件日志（飞行记录仪）

范围：b90317f..（本批最终提交）。spec：父仓库 `docs/superpowers/specs/2026-09-10-opta-gateway-mt-batch10-episode-log-design.md`。

## 起因

2026-09-10 夜里三次阶梯复位（01:04 / 04:02 / 04:43），标记全是 `offline 20m p=8/0 fo=0 wr=2 gw=ok`：上游通、路由器通、重关联无效、复位后 30 s 内连上——板子自己的网络栈卡住。20 分钟只跑了 8 次探针（每趟 `update()` 约 2 min），夜里探针重解析域名卡过 4.4 s / 9.8 s，mbed `connectSSL(host)` 解析失败时静默连 0.0.0.0。**DNS 头号嫌疑，无直接证据**；复位又把 RAM 清光，夜里没人守串口。USB 日志不做（USB-C 切 host 后串口与 DFU 全失，2026-09-01 实测）。

## 本批做了什么

- **DNS 测试**（`cloud_ladder.h`）：云断且 WiFi 通、离线满 2 min 起，每 60 s `nsapi_dns_reset()` 后真实解析 iot.arduino.cc 并计时。前 2 分钟不清缓存（库和探针还在用那条 A 记录，终审 I-1）。
- **事件日志**（`episode_log.h`）：云断期间每 60 s 一行 `20m w1 p1 d0/15021 u118000 r-27 h15912`（离线分钟、WiFi、探针、DNS 结果/ms、上趟 update() ms、RSSI、堆余量）进 40 行 RAM 环。落盘两处、两个键：复位前 → `epi_reset`（静默，在标记之后）；离线 ≥ 5 min 自愈 → `epi_heal`。每块带头行 `# reset n=61 off=20m rows=20`。开机读回串口；RJ45 `http://192.168.102.107/episode` 两块都显示。
- **标记格式**：`off 20m p=8/0 fo=0 wr=2 gw=1 dns=0`（gw/dns：1 通、0 失败、`-` 未测）。
- `[HB]` 加 `dns=<-|0|1>/<ms>`；`[CLOUD] back after` 行加 `dnsFails=/upmax=`。
- **KVStore 互斥**（`boot_reason.h` `g_kvMutex`，2 s 有界等待）：复位标记、事件日志都拿锁；主线程的 WiFi 覆盖写入仍无锁（只在人工保存/清除时发生、随后即重启）。

## 操作员须知

1. 早上看 `/episode`：`d0/15xxx` 连成一片 = DNS 在板子这边失效（库连不上的原因）；`d1/…` 而 `u` 很大 = 解析正常、TLS/MQTT 那一层卡；`w0` = WiFi 掉了。
2. 标记第三次改格式：`off 20m p=8/0 fo=0 wr=2 gw=1 dns=0`。ops 没有脚本解析这段文字（`watch_restart.py` 只取 `n=`）。
3. `epi_heal` 只在自愈的离线 ≥ 5 min 时写，3 min 的抖动不留痕，避免频繁写 flash。
4. `[HB] dns=-/0` 在云通时是常态（不测）。

## 终审（opus，2026-09-10）

**裁决：通过（带条件）→ 已修 5 条，第 6 条部分处理。** Critical 0，Important 6，Minor 11。

- **I-1** 掉线 20 ms 就清 DNS 缓存、每 60 s 一次，把库/探针/NTP 正要用的 A 记录一并清掉，仪器变扰动源。→ 推迟到离线满 2 min 起。
- **I-2** 落盘（1.9 KB QSPI + 四次 LOG）排在 `bootMarkIntentional` 之前，日志锁被卡时标记写不进、复位不发生。→ 标记先写，落盘改为静默、在标记之后。
- **I-3** `putBytes` 返回 `int`（失败为负）却接进 `size_t`，失败被打成 "saved 4294967015 B"。→ 用 `int` 接并比对长度。
- **I-4/I-5** 复位与自愈共用一个键、块无身份 → 两个键 + 头行 `# <why> n= off= rows=`。
- **I-6** 四个 KVStore 写者各自 `new TDBStore` 无互斥，批 10 把云线程写从"复位一瞬"放大到"每次 ≥5 min 离线"。→ 加 `g_kvMutex`（2 s trylock），标记与事件日志都走它；主线程 WiFi 覆盖写留无锁（人工触发、随即重启），记录在此。
- Minor 已顺手：`[HB]` 打 `dns=-/0` 而非 `-1/0`。未动：`episodeLogLoad()` 在 Ethernet 之前（无影响）、静态 RAM +约 6 KB、`dns=` 无年龄字段。

仪器有效性（终审核）：mbed `LWIPStack` 未覆写 `gethostbyname`，`nsapi_dns_reset()` 清的正是库在用的那份缓存，测量是真解析。
