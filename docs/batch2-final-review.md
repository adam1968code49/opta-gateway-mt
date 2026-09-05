# 批 2 终审记录 — 水量核算（85efb94..67d737e）

终审 2026-09-05（opus），裁决：**可以 OTA**（在 `67d737e` 上干净重建后）。Adam 决定审查通过即 OTA。

## Task 3 审查抓到并已修的 8 条（提交 67d737e）

1. `EtherNetIP.cpp` `readRealsMSP` 两处 `readExact` 超时不断会话 → 流错位，下一条回复被当成这一条解析，别的 REAL 标签的值可能被当成累计基数**写回终身总量**。修：失败即 `end()`（与另两处一致）。副作用是好的：半死 PLC 下一拍从 1.5–3 分钟缩到约 1.5 s（后续交换瞬时失败）。
2. `waterOnReset` 先清账再写 PLC，写失败也报成功。修：先写五个标签（累计、显示、mark、行程、历史），全部成功才清账本与参考值；HMI 标志只在成功时写回（失败自动重试）；命令分支据返回值报告。
3. `config.h` HMI 握手注释写"PLC 自己清零"，与实际（网关清零，梯形图只置位——Adam 确认）矛盾。修注释。
4. 复位不动 `trip_watervolume_mark`，`trip = wrap(0 − mark)` 会在下个整点给客户显示 ≈500,000 L。修：复位一并清 mark、trip、历史。
5. `FLOW_TO_PLC_ENABLE` 无人引用。修：包住 `waterToPlc` 调用与复位命令分支。
6. `everOk` 永不复位。修：核心写连败 3 次退回 60 s 节拍。
7. 失败行文案 `INSANE` → `rejected/insane`。
8. （Task 4 审查）`drainCommands` 总闸关着丢弃一切命令，含 `CMD_FLOW_RESET_TOTAL`，与"复位不经总闸"矛盾。修：该命令豁免总闸（仍需 PLC 在线）。

复审判断：`waterOnReset` 部分成功（会话中途死）时不降参考值是对的——下一拍"下载恢复"把复位干净回滚；若改成信任 0 基数，mark 未清会让行程显示成终身累计。残留：断点恰在 mark 写成功之后 → 行程膨胀，可用 HMI 行程复位恢复，终身累计不受损。

## 写 PLC 的每条路径与守卫

| 写什么 | 守卫 |
|---|---|
| 复位五写 | HMI：拍首连接 + 节流 + 读到标志；云：静默期 + 队列 + `allowWrites` + 连接（豁免总闸） |
| 行程复位 | `tripProbe` + 标志 + 累计可读且合理（否则不确认） |
| 下载恢复写累计 | `baseSane && s_lastGoodValid && base < lastGood − 0.5` + `WATERVOL_RESTORE_ENABLE` |
| 提交累计 | `baseSane` + `flowCommitReady()`；写成功才扣账 |
| 显示/行程/历史 | `baseSane` + 整点或 `forceHmi`；历史仅真整点非首拍 |

基数每拍重读；`saneVol` 拒 NaN/负/>505000；**没有路径能在基数不可信时写累计**。

## 留到下一批

1. `sendRRData` 另四个"读完头就返回"的分支（cmd 不符、封装 status≠0、bodyLen 越界）同样不消费 body → 错位源。**下一批第一条。**
2. `waterOnReset` 部分成功：做成挂起重试，或 mark 只在 `cum:=0` 确认后才清。
3. `FLOW_ENABLE` 是死开关（`flow_meter.h` 无 `#if`）。
4. 会话中途掉线时 `valveOk[]` 留上一拍（批 1 遗留）。
5. `waterToPlc` 与 `drainCommands` 共用 `WD_AT_CIPWRITE`，加 `WD_AT_WATER`。
6. 批 1 遗留的加固批（DNS 缓存、LOG 加锁、stall 滑动窗口、TAKE 断言、build.sh 告警门、shared.h TU 守卫、UTC 再同步周期性解除总闸的观察）。

## 与旧固件的行为差异（Adam 须知）

1. 复位现在一并清行程 mark、trip、5 格历史（旧版只清累计+显示，复位后行程≈50 万升）。
2. 复位改成"PLC 全部写成功才算数"；失败不清账本、HMI 标志不回写（自动重试）、`lastError` 报失败。
3. 云端 `flowResetTotal` 现在真正落到 PLC 且不会被"下载恢复"撤销（旧版会）。
4. 行程/历史功能这次才第一次真正上线（旧固件里标签不存在）：`tripWaterVolume` 起初 ≈ 终身累计，直到按一次 HMI 行程复位把 mark 打到当前值。
5. 诊断类：提交被拒写 `lastError`；核心写连败 3 次退 60 s；墙钟换 `time(nullptr)`（同一块 RTC）；复位命令不经总闸。

## 操作员须知

1. **云端 `flowResetTotal` 会把 PLC 终身累计清 0**，并清行程 mark/行程/历史；客户看得见，网关无法恢复（只能人工从 InfluxDB 抄回）。按之前先记下 `cumulativeWaterVolume`。
2. 按下后 2–6 s 看四个总量是否归零、开关是否弹回；以数字为准，`lastError` 只活约 2 s。
3. `lastError` 出现 `water reset: PLC write failed` / `HMI reset: PLC write failed, retry` → 再按一次并核对四项全 0。
4. 不要同时按 HMI 两个复位按钮。
5. 上板后建议按一次 HMI 行程复位，让行程从现在起算。
6. 上板时 PLC 终身累计为 8.04 L（2026-09-04 12:00 至 2026-09-05 05:00 的产水未记入，无法补）。

## 上板后必须观察

1. `cumulativeWaterVolume` 只增不减、不反复归零（反复归零 + `lastError` 反复 "cumulative total reset (HMI)" = 梯形图每扫描都置位 → 关 `FLOW_TO_PLC_ENABLE` 回滚）。
2. `waterOwedL` 放水结束 30 s 内回 0，不单调爬升。
3. `plcTotalRestores` 保持 0；非 0 立刻核对累计值。
4. 整点：`hmiWriteCount+1`、`hmiWriteOk=True`、`hmiWaterTotal` 跳到 `displayWaterVolume`；`tripWaterVolume` 不是 ~500,000 量级；`stallWhere` 出现 `@10` 且 >12 s 说明 PLC 慢而不断。
