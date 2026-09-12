# 批 12 终审记录 — 远程清除 PLC 故障位（Press_Error / Temp_Error / Gen_Error）

范围：`2801690..5bee9cb`。spec：父仓库 `docs/superpowers/specs/2026-09-11-opta-gateway-mt-batch12-design.md`，计划 `docs/superpowers/plans/2026-09-11-opta-gateway-mt-batch12.md`。

## 需求与取舍

Adam（2026-09-11）：工程师要能远程手动清这三个故障，按钮放在看板三个故障显示的正下方。决定：**网关直接把 PLC 标签写 0**（方案 1，新增三个清除变量；不改现有故障变量为可写，不走 `Reset_Button`，不等 PLC 加清除标签）。PLC 怎么维持这三个位不知道也不假设——写 0 后**读回核对**，把"清掉了 / 被 PLC 重新置位 / 没核实到"告诉工程师。

## 实现（`2e5f492`）

- `thingProperties.h`：`clearPressError` / `clearTempError` / `clearGenError`（READWRITE, ON_CHANGE）。Thing 上用 `ops/batch12_thing_vars.py` 建（幂等；注意 Arduino API 建变量是 **PUT** `/iot/v2/things/{id}/properties`，POST 返回 405）。
- `cloud_ctrl.h` `ctrlClearFault()`：只对 true 动作，过 `ctrlGate`（15 s 同步静默期 + `controlEnabled` + PLC 在线），入队，变量总是写回 false。归为"操作机器"而非记账，与 Reset Button 同门。
- `shared.h`：`CMD_CLEAR_PRESS_ERROR/TEMP/GEN` 追加在 `CmdTag` 末尾。
- `plc_thread.h`：`clearFault()` 写 0 并报 `<Tag> cleared` / `write <Tag> failed`，登记 4 s 后核对；`clearVerifyTick()` 在阀门扫描后按槽位 `VSLOT_*_ERROR 24/25/26` 读回，报 `clear confirmed` / `re-asserted by PLC` / `clear unverified`。
- `plc_tags.h`：`VSLOT_PRESS_ERROR/TEMP/GEN`；`cloud_side.h` 的 `TAKE_VB` 改用它们。

## 终审（Opus，2026-09-11）→ 修复（`5bee9cb`）

无 Critical。

- **I1 结论只活一拍**：`pollSensorsInto` 每拍把 `lastError` 写回 "ok"，结论只存在于一个快照；云线程那一拍若被 WiFi/TLS/OTA 卡住就永远看不到。→ 照 flow_watch 的写法锁存文本 60 s（`CLEAR_HOLD_MS`），只覆盖在 "ok" 的空档里，真错误仍然优先。"cleared" 也一并锁存，工程师能看到 "cleared → 结论" 两步。
- **I2 断线后的陈旧结论**：PLC 在写与核对之间掉线（IP2 常有的程序下载），重连后第一拍会对一个因别的原因变化的位下结论。→ 离线分支 `clearDisarm()` 撤销登记；到期超过 15 s（`CLEAR_VERIFY_STALE_MS`）只报 `unverified`。
- **I3 槽位无编译期约束**：往 `VALVE_TAGS` 中间插一个标签会让读回和 `pressError` 路由都静默错位。→ `VALVE_TAGS` 改 `constexpr`，`tagSlotIs()`（循环写法，不用递归）+ 四条 `static_assert`（含原有 `VSLOT_P_COND`）。
- **M4 快读误判**：门控的是扫描结束时间；一次写后 100 ms 起扫、拖过 4 s 才结束的扫描，读到的是 PLC 下一个周期之前的值。→ 记录 `s_valveSweepStartMs`，只对"起始时间 ≥ 到期时间"的扫描下结论。
- **M6** `clearFault` 补 `SHARED_ASSERT_ON_PLC()`。
- **M5 未采纳**：`clearVerifyTick` 不做 I/O，不另设喂狗 where 码。
- 核实无误：millis 回绕比较；flow_watch 只在 "ok" 时覆盖，不会盖掉结论；按钮松开（false）是纯空操作；重连回放的 true 在静默期被 `ctrlGate` 拒并写回 false，到不了 PLC；格式串全是 `%s` 对 `const char*`；零动态分配零递归。

## 看板（"IP2 AWG v8"，`ee836a7b-6406-41ae-9c47-5c62e0e96de6`）

REST v2 对 `pages` / `page_id` **完全无视**（2026-09-11 一次性测试看板实测：POST 和 PUT 之后 `pages` 为空、`page_id` 全 "0"），手机 App 按页渲染会打不开，所以不 PUT v7。`ops/batch12_dashboard_v8.py`：CLI `dashboard extract` v7 → 三个 `Push Button` 4×4 放 `y 137, x 0/4/8`（手机 `(0,314)/(4,314)/(0,318)`），其下部件桌面 `+4`、手机 `+8` → CLI `dashboard create` → 校验：183 个部件、名字集合 = v7 ∪ 三按钮、全在同一页、其余部件几何逐一相等、三按钮 REST 视图 `has_unlinked_variable=false` 且 READ_WRITE。全过。v7 原样保留作备份。

## 验证（2026-09-11 19:10–19:20 PT）

- OTA `07d9ff40…` 89 s 成功；`fwVersion 5bee9cb`，n=79→80（这次只计一次开机）；三个清除变量在云端为 false。
- 开机后 `controlEnabled` 为 false（同步后强制关闭，设计如此）。用 API 按 `clearPressError=true`：11 s 内 `lastError` = **"control disabled"**，变量回弹 false，PLC 未被写——门槛链路通。
- 板子当时 `genError=True`（机器上真有 General fault）。"confirmed / re-asserted" 两条路由 Adam 在 v8 上做：开 Control enable → 按 "Clear general fault" → 看 `lastError`。若为 `Gen_Error re-asserted by PLC`，说明 PLC 每周期重写该位，要同事在 PLC 侧加清除逻辑，网关不必再改。

## 操作员须知

1. 按钮要在 **Control enable 打开**、PLC 在线、开机/重连 15 s 之后才生效；否则 `lastError` 说明原因（"control disabled" / "control: syncing, wait 15 s" / "control: plc offline"）。
2. 按下后 `lastError` 依次：`<Tag> cleared` → 约 4 s 后 `<Tag> clear confirmed` 或 `<Tag> re-asserted by PLC` 或 `<Tag> clear unverified`（PLC 中途掉线 / 标签没读到）。文字在无其它错误时保留 60 s。
3. 三个按钮各自独立；写 0 到一个本来就是 0 的位没有副作用。
