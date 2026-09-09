# 批 8 终审记录 — 云离线分级恢复（cloud ladder）

范围：a3cc7aa..（本批最终提交）。spec：父仓库 `docs/superpowers/specs/2026-09-09-opta-gateway-mt-batch8-cloud-ladder-design.md`。

## 起因

2026-09-09 03:44 云断 18 min（WiFi 通、PLC 正常），04:02:46 批 6.1 的 15 min 门整机复位，`bootReason = cloud offline 15min n=54`，重启后立刻连上。Adam 问能否只重启云任务、不重启整机。

## 库给的边界

- `ArduinoCloud.disconnect()`（ArduinoIoTCloud 2.10.0）把私有 `_auto_reconnect` 置 false 并停在 Disconnected，没有公开 `connect()`。库层没有软重启入口。
- 杀云线程重建不可行：lwIP/mbedTLS/驱动锁不释放，TLS 上下文泄漏；`begin()` 二次调用重复注册属性。
- 可行的门：`WiFi.disconnect()`。`WiFiConnectionHandler` 每次 `check()` 看 `WiFi.status()`，看到断开就自己 `end()+begin()`，云库退回 ConnectPhy 重走 DNS/NTP/TLS，`_auto_reconnect` 完好。线程、堆、PLC 会话、流量脉冲都不动。

## 本批做了什么

`cloud_ladder.h`（云线程）替换批 6.1 的平直 15 min 门：

| 云断时长 | 动作 | 条件 |
|---|---|---|
| ≥ 1 min | 探针 fail-open（不变） | |
| 5 min | `WiFi.disconnect()` 重关联 | WiFi 已连续通 ≥ 2 min |
| 15 min | 再一次 | 同上 |
| 20 min | 标记复位 | 探针最近 10 min 内成功过（上游通、卡的是我们） |
| 60 min | 标记复位 | 无条件兜底 |

复位仍等在飞水量归零，最长再等 5 min。计时器只在云连上时清零（重关联导致的 WiFi 短断不清零）。

复位标记带证据：`cloud offline 20min p=12/300 fo=50 wr=2`（本次离线的探针成功/失败、fail-open、重关联次数）。云回来时串口一行 `[CLOUD] back after N min: p=.. fo=.. wr=..`。`[HB]` 加 `wr=`（终身重关联数）和 `off=`（当前离线分钟）。

## 操作员须知

1. 云断不再固定 15 min 重启。上游真断（Starlink 掉线）时 5、15 min 各重关联一次，60 min 才复位一次；上游通着但连不上时 20 min 复位。
2. `bootReason` 里 `p=0/..` 说明上游整段不通，重启没帮上忙，去查 Starlink；`p=N/..`（N>0）说明上游通、是网关这边卡了，重启是对症的。
3. 重关联瞬间 `wifiRssi`/`wifiUp` 会闪一下，是设计动作，不是故障。
4. 台面上 WiFi 通、Thing 没配的板子进 20 min 复位循环；`/clear` 断 WiFi 即停。
5. `[HB]` 的 `off=` 反复归零、`cloud=` 反复翻转，是上游抖动（连上几十秒又断），阶梯按设计不动作，查 Starlink 不查网关。
6. 离线期间发生的流量失配报警（批 7，只在 RAM）会被阶梯复位清掉，且复位前发不出去；比批 6.1 每 15 min 清一次已是改善。
7. `wr=` 是本次开机以来的重关联次数，复位归零。

## 终审（opus，2026-09-09）

**裁决：修后 OTA → 已修。** Critical 0，Important 1（已修），Minor 9（修 4，记 5）。

已修：
- **I-1 自伤失能**：5 min 的 `WiFi.disconnect()` 若没把 WiFi 拉回来，`s_ldWifiUpSince` 每趟清零，settle 门把四级全挡住，包括 60 min 无条件兜底，板子无限期离线且喂狗照常。修法：60 min 兜底在"本次已做过重关联"时不受 settle 门约束；AP 真掉电（未做过重关联）仍与以前一样不复位。
- M-4 头注释把恢复归功给 `WiFiConnectionHandler`；实测常态是同趟的 `wifiRescue()` 先赢（其 `lastTry` 在 WiFi 通时被清零，不受 60 s 节流），handler 根本不知道断过。注释改为两条路并存、谁赢无所谓。
- M-6 `owedL != 0.0f` → `fabsf(owedL) >= 0.01f`（10 mL 容差）。
- M-7 加 `<mbed.h>`、`<math.h>` 使头文件自足。

记录：
- M-2 离线超过约 3 天时标记数字位数会让 `snprintf` 截掉末字符；只失真不越界（`tag[48]` 有界，KVStore 47 字节往返、`s_bootReason[96]` 拼接后 75）。
- M-3 spec §5 里"重关联期间 `wifiUp` 短暂为 false"在常态下观测不到（`wifiRescue` 1–3 s 内拉回）；第二级仍 15:00 准点、复位级 20:00 准点。`wifiRescue` 失败时才走 spec 那条时间线。两分支都自洽。
- M-5 抖动上游（连上 30 s 断 10 min）永不升级，与批 6.1 同性质，可接受；已写进操作员须知 5。
- M-8 复位丢批 7 的 latch；须知 6。
- M-9 `millis()==0` 哨兵理论盲点 20 ms 自愈；`wr=` 口径已改"本次开机以来"。

八个场景结论：mbed `WiFi.disconnect()` 同步置 `WL_DISCONNECTED`，下一趟立刻可见；云库对网络层零回调，无副作用；标记极端 47 字符、拼接 75 < 96；`Serial.print` 无重载歧义；`millis()` 减法全部回绕安全；与 fail-open、flow watch、OTA 无冲突（OTA 时云是通的，阶梯不动）。
