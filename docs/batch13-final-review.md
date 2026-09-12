# 批 13 终审记录 — S4 排气阀 / Lefoo 冷凝泵远程手动开关

范围：`5bee9cb..2bb5f73`。spec：父仓库 `docs/superpowers/specs/2026-09-11-opta-gateway-mt-batch13-design.md`，计划 `docs/superpowers/plans/2026-09-11-opta-gateway-mt-batch13.md`。

## 需求与背景

2026-09-11 晚分析：三轮循环都在 Action_6+8+9 步（S8 通气 → 等 p4/p5 升 → S4 开 → 冷凝泵排水）被 Gen_Error 掐掉，泵一次没起，集水罐液位 49 → 78 无人排。Adam 要两个开关远程手动开 S4、开泵把罐子排空；液位 18 停泵。

## 实现（`c40bc0e`）

- `manS4Open` / `manCondPump`（READWRITE）。`Air_S4_Output` 是 REAL 0–100（`writeReal` 100/0），`Cond_Pump` 是 BOOL。
- 云线程 `ctrlManual()`：`ctrlGate` 三道门 + 开时序列必须空闲（`actionWord == 0`）；拒绝时开关写回 PLC 真实状态。
- PLC 线程：泵开需 `posS4 >= 50` 读回；关 S4 若泵在跑先停泵；`manualTick()` 自动停泵（液位 < 18 或 180 s）+ 4 s 读回核对（`re-asserted by PLC`）。
- `cloud_side.h` 镜像：两个开关每拍等于 PLC 实际输出（命令后短暂挂起），PLC 覆盖就弹回。

## 终审（Opus）→ 修复（`2bb5f73`）

- **C1 所有权**：自动排水时序列自己开 S4/泵，镜像让开关显示 ON，操作员一拨 OFF 就会关掉序列的阀、停掉序列的泵。→ 加所有权：只有网关自己打开的输出才允许从看板关（否则 `S4: not a manual output` / `pump: not a manual output`）；所有权在操作员关闭、PLC 收回（读回 OFF）、核对判 re-asserted 时结束。
- **I2** 泵没停成功仍关阀 → 会让泵对着密闭罐抽真空。→ 停泵失败即返回，不写 S4。
- **I3** PLC 断线把泵所有权和计时清掉 → 重连后液位/180 s 停泵都不再生效。→ 断线只撤销核对，保留所有权与起始时间。
- **I4** 空闲门只看云线程那份最多 ~8 s 旧的动作字。→ PLC 线程执行时用自己的 `w.actionWord` 再查一次。
- **I5** `manual: cycle running` 等拒绝文案没通知 `cloud_side` 去重 → 会一直挂在看板。→ 所有拒绝文案（含原有 `ctrlGate` 三条、队列满）都调 `cloudSideNoteLastError()`。
- **I6** 核对无超时，自动停泵的写也会挂核对。→ 到期超 15 s 或没读到报 `unverified`；自动停不挂核对。
- Minor：`ctrlPost` 返回 bool，投递失败不挂镜像；镜像挂起改为"消费到命令后两拍的快照即解除，10 s 上限"；自动停后喂狗、恢复 where 码。
- 核实无误：CloudBool 本地赋值不触发 onChange（`CloudBool.h:53-57`），重连回放被静默期挡住无乒乒；`writeReal` 签名与 REAL 扫描类型一致，`VSLOT_POS_S4` 有 `static_assert`；文案全 ≤ 47；millis 回绕安全。

## 看板

"IP2 AWG v9"（`9d4dc360-988d-490e-a932-9fcbba01719f`）：v8 模板 + `S4 vent (manual)`(12,137) + `Lefoo pump (manual)`(16,137)，手机 (4,318)/(0,322)。校验：185 个部件、名字集合、同页、几何、两开关 READ_WRITE。v8 留备份。

## 验证（2026-09-11 21:38–21:45 PT）

- OTA `3b0dafae…` 成功；`fwVersion 2bb5f73`，n=80→81；开机时两开关镜像为 false（`posS4=0`、`pumpCond=0`），序列空闲，`tankLevel` 78.5。
- 真机排水由 Adam 操作：Control enable 开 → `S4 vent (manual)` 开 → 看 `posS4` 变 100、`lastError`=`S4 opened`；→ `Lefoo pump (manual)` 开 → `pumpCond=1`、`flowRate>0`、`tankLevel` 下降 → 到 18 自动停 `pump stopped: level 18`。任一环节报 `re-asserted by PLC` 并弹回 = PLC 每周期重写该输出，需同事在 PLC 加手动位。

## 操作员须知

1. 两个开关**显示的是 PLC 实际输出**：自动排水时它们自己会亮，那时拨 OFF 会被拒绝（"not a manual output"），只能关自己开的。
2. 开的顺序：先 S4，再泵；关的顺序随意（关 S4 会先把泵停掉）。
3. 泵自己会停：液位 < 18 或跑满 180 s；要再排就再拨一次。
4. 序列在跑时开不了（"manual: cycle running"）；开机/重连后 15 s 内、Control enable 关着、PLC 掉线时同其它控制。
