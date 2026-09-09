# 批 9 终审记录 — 半开连接守卫 + 三条证据

范围：fd8d063..（本批最终提交）。spec：父仓库 `docs/superpowers/specs/2026-09-09-opta-gateway-mt-batch9-evidence-design.md`。

## 起因

1. 2026-09-01 三次"`Connected`、`cloud=up`，云端却零属性"。库按 QoS 0 发属性，设备无法知道有没有落地；MQTT 层（30 s ping / 60 s 自断）和 Thing 层（last-values 30 s × 10）都盖不住"挂上了、SYNC 没来、库又没放弃"的状态，批 8 阶梯在 `connected()` 为真时不动作。
2. 阶梯复位标记 `p=0/…` 分不清 Starlink 上游断与路由器自身死。
3. 同事频繁改 PLC 程序，`read fail x2 (cip=0x0)` 不知道是哪两个标签。
4. 事后没有"距上次 SYNC 多久"的记录。

## 本批做了什么

- **无 SYNC 守卫**（`cloud_ladder.h`）：`connected()` 连续 6 min 仍没收到 SYNC → `WiFi.disconnect()` 一次（每次连接只一次），`[HB] nsk=` 计数。这是 QoS 0 下设备手里唯一的应用层证人。
- **复位标记加网关 ping**：复位前 `WiFi.ping(gatewayIP, 1 s)`，标记改为 `offline 60m p=9999/9999 fo=999 wr=2 gw=ok|fail|na`（≤ 43 字符）。`gw=fail` 去重启路由器；`gw=ok` 且 `p=0` 等 Starlink。
- **`plcFailTag` 点名传感器**：传感器扫描的第一个未读标签优先写入，阀位扫描只在没有传感器失败时写。`plcReadFails` 语义不变（仍只数阀位扫描）。
- **`[HB]`** 加 `sync=<秒>`（-1 = 本次连接尚未同步）和 `nsk=`。

## 没做、也做不到的

发出去的属性云端不收但 SYNC 正常：QoS 0，设备看不见。判据在服务器侧：`uptimeS` 每 30 s 无条件上报，超过 5 min 没更新且设备没重启就是零上报。ops 监视脚本另立，不需要 OTA。

## 操作员须知

1. `plcFailTag` 现在会显示传感器标签名（如 `Raw_Module_1_Pressure_Grey`）。同事改 PLC 程序后看这里就知道哪个标签对不上。传感器全部读不到时显示 `all sensors`（下载中或会话丢了），不点名。`plcReadFails` 仍只数阀位扫描，所以 `plcReadFails=0` 配一个传感器名是合法组合。
2. bootReason 的复位标记格式变了：`offline 20m p=12/300 fo=50 wr=2 gw=ok n=57`。看 `gw=`：fail 是路由器自己不应答，去重启路由器；ok 且 `p=0` 是上游断，等 Starlink。
3. `[HB] sync=` 正常连接下应持续增长；连接刚建立时 -1 几十秒是正常的。`nsk>0` 说明守卫踹过，中间有一次断线重连，总闸要重拨。
4. **`nsk=0` 不等于 9 月 1 日那类故障被修掉了。** 见终审 I-1。复发时的判定步骤：串口看 `[HB] sync=` 是否为 -1、有没有 `[CTRL] cloud SYNC` 行；云端看 `uptimeS` 是否 5 min 没更新。
5. 开机连上云后串口会打一行 `[CLOUD] gateway 192.168.1.1 ping N ms`；若是 `FAILED`，说明路由器不答 ICMP，以后标记里的 `gw=fail` 不能信。

## 终审（opus，2026-09-09）

**裁决：通过（带条件）→ 条件已满足。** Critical 0，Important 6（修 3，文档化 2，留 1），Minor 9。

已修：
- **I-3** 库在 MQTT 不断的情况下也会发 DISCONNECT（Thing detach/换 thing_id），`s_syncSeen` 被清而 `connected()` 仍真，守卫会零宽限立刻踹并打出"connected 47 min without SYNC"的假日志。现在 `ctrlSyncSeen()` 真→假的跃变重新起算 6 min。
- **I-4** 传感器全部未读时 `plcFailTag` 会显示 `Temp_1`（第一个），假精确。改为 `all sensors`。
- **I-2** 网关 ping 这个仪器没被证明过能出声，而它只在几周一次的复位那一刻响。加了开机连云后一次性的 ping 进串口。

文档化：
- **I-1（本批最重要的一句）** 把库的三条放弃路径量算后：守卫真正能救的是"thing_id 应答为空 → registered 但未 attach"（库要约 106 min）和"detach 后再挂载"（库再也不发 SYNC）两种状态；"已 attach 但 last-values 收不到"库自己 5 min 就断，守卫踹不到。9 月 1 日那次打印了 `Connected to Arduino IoT Cloud`（出自 attach），落在后者或"SYNC 正常但 QoS 0 出方向丢包"，**两者守卫都不动**。所以本批对 9 月 1 日的真实交付是 `sync=` 这个判别器，不是修复；`nsk` 长期为 0 只说明没进过前两种状态。
- **I-5** `plcReadFails` 与 `plcFailTag` 语义脱钩：ops 的 `gen_diagnostics.py` 两块面板说明已同步改写；历史曲线在 2026-09-09 批 9 上板时刻断层，无版本标记，以 README 时间为判据。

留：
- **I-6** `sync=`/`nsk=` 只在串口，故障现场恰好没串口。审查建议踹时写 `lastError`，但 `lastError` 每 2 s 被 PLC 快照覆盖，且踹的下一刻连接就断，写不上云；真正的服务器侧证据是 `uptimeS` 5 min 不更新，另立 ops 监视脚本。
- Minor：标记超过约 3 天离线时 `snprintf` 截尾会掉 `gw=`；`gatewayIP()` 为 0.0.0.0 时 ping 必失败；`cloud_ctrl.h` 文件头 "MAIN THREAD ONLY" 已过时；`sync=` 语义是"本次连接内"而非"上次"。

七个场景结论：库 `connected()` 先变假、DISCONNECT 回调下一趟才来，方向恰好安全（每次重连两者都干净起算）；守卫踹的时候总闸本来就是关的，对操作员无新解除；`MBED_CONF_LWIP_RAW_SOCKET_ENABLED` 在 OPTA 变体为 1，ICMP 真会发；1 s ping 在位置码 14 下、复位之前，安全；`LOG(long)` 重载唯一；标记拼接 75 < 96；`millis()` 回绕全部安全。
