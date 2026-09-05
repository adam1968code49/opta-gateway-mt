# 批 1 终审与复审记录 — 上板前必读

范围：94b422b..8120373（六个实施任务 + 一次三条必修的修复 + 两条次要顺手修）。终审 2026-09-04 晚（opus），
复审修复同日。**复审裁决：可以 OTA。** Adam 决定暂不 OTA，等他通知。

## 终审抓到并已修的三条（都是"机器无人自启"的路径）

1. 静默期原本钉在 `ArduinoCloud.connected()`：MQTT 一连上就为真，而云端存值的回放要等 Thing 同步
   （库里 30 s 超时可重试 10 次）。回放晚于 15 s 落地 → 三门全开 → 存着的 `controlEnabled=true`+`systemRun=true`
   直接写 Start_Button。**修：**静默期改钉库的 `SYNC` 事件（库在回放回调全部执行完之后才触发它，
   `ArduinoIoTCloudTCP.cpp:545-549`）；SYNC 之前一律忽略。
2. `s_cloudSeen` 只记第一次，每次重连都会重新回放存值。**修：**`DISCONNECT` 事件把 `s_syncSeen` 清零并解除总闸；
   每次 SYNC 也解除总闸并把云属性 `controlEnabled` 写回 false。
3. 重连拍的 `drainCommands` 用拍末 `eip.connected()`，PLC 刚连上那一拍会把离线期间攒的命令写进去。
   **修：**`drainCommands(w, allowWrites)`，重连分支传 false 只清队列并报 "control: plc offline"。

复审五个时序场景（开机回放 / 运行中 WiFi 抖动回放 / 回放晚到 40 s / SYNC 后 10 s 就翻总闸 / SYNC 里写回 false）
均无非操作员触发的 Start 写入。回放只在"本地值 ≠ 云端存值"时才进回调（`CloudWrapperBool.h:32`），
SYNC 把云属性写回 false 是第二道保险。

## 上板后必须观察的一条（复审新发现）

`ArduinoIoTCloudThing.cpp:172-174`：UTC 偏移到期时 Thing 直接回到 `RequestLastValues`，**不经 DISCONNECT**，
于是每次都再触发一次 SYNC，而我们每次 SYNC 都解除总闸。若该 Thing 的时区信息（`tz_dst_until`）没送到或已过期，
可能每 30 s 一次 SYNC → 总闸每 30 s 被解除一次 → **操作员永远开不了机**。
判据：串口 `[CTRL] cloud SYNC:` 稳态下只在连上时出现一次；云端 `controlEnabled` 若在操作员打开后自己周期性变 false，
就是这条路。修法候选：SYNC 只在 `!s_syncSeen`（即上次 DISCONNECT 之后的第一次）才解除总闸。

## 留到下一批

1. `EtherNetIP.cpp` `readExact` 每收一字节重置计时 → 单次交换理论无界；批 1 把每拍交换次数从 8 提到 19+16 写。
   加整体截止（`deadline = start + 3*timeoutMs`）。批 0 就点名、仍未做，**最值得串口验证**（`nc -l 44818` 假 PLC）。
2. `applyCommand` 直接信 `c.value`，钳位只在主线程；批 2 若有别的生产者要复制钳位进 PLC 线程。
3. `desorpPreMin` 只回读 T6，写却是 T6+T11；T11 写失败会静默分叉，至少把 T11 读回来比较。
4. 命令结果的 `lastError`（"start set"）2 s 后被 "ok" 覆盖；失败信息一起消失。加粘滞或独立属性。
5. `w.stateFails` 进了快照但没赋给任何云属性。
6. `TAKE()/TAKE_V*` 槽位覆盖仍无编译期断言（现在 64 槽）。
7. 最坏一拍 ≈105 s（19 读 + 17 写 × 3 s），喂狗安全但期间不发布 → 主线程报 "plc thread stalled"、`loopStallMs` 被顶上去，是诊断噪声。
8. 两线程共用 `Serial`（批 0 第 8 条），批 1 日志量更大，**接串口调试时**风险升高。
9. 批 0 留下的：shared.h 多翻译单元守卫、cloud_probe DNS 否定缓存、stall 滑动窗口、build.sh 告警门。
10. `shared.h:73` `lastError` 注释补回命令结果示例。

## 操作员须知（上板后）

1. **每次开机、每次云端重连之后**，看板总闸 `controlEnabled` 必须关一次再开，任何按钮才会到 PLC。
2. 等的是"最后一次 SYNC 起 15 s"，不是"上电起 15 s"；早翻了开关会自己弹回 OFF，`lastError` 显示 "control: syncing, wait 15 s"。
3. 重连后看板 `systemRun` 可能仍显示 ON 但 PLC 没收到任何东西（DISCONNECT 只解除本地总闸）；要启动得把它关一次再开。
4. 总闸"自己弹回 OFF"是正常现象：每次 SYNC 都会写回 false 以显示真实状态。
5. 按钮到 PLC 2–4 s；`plcStateWord` 的跟随再慢一拍（6 s 状态扫描）。
6. `lastError` 的 "start set" / "write ... failed" 只停约 2 s 就被 "ok" 覆盖。
7. 吸附 5–60 分钟、脱附 10–60 分钟，越界会被改成边界值并回显；设定后看板可能闪一下旧值再回新值。
8. FAULT 灯现在也包含 `pressError/tempError/genError`。
9. OTA 之前必须用云 API 把五个可写 BOOL 置 false（计划 Task 7 Step 1）。
