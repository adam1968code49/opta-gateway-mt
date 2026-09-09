# 批 7 终审记录 — 排水流量失配报警（flow watch）

范围：9f9ad5b..（本批最终提交）。spec：父仓库 `docs/superpowers/specs/2026-09-08-opta-gateway-mt-batch7-flow-watch-design.md`。

## 本批做了什么

- `flow_watch.h`（PLC 线程）：泵 `pumpCond` 起停为一次排水，泵停 45 s 后对照液位降幅与流量计升数；`降幅 ≥ 8 cm 且 升数 < 0.25 L` 判失配。
- 失配 → `lastError` 叠加 `flow mismatch: lvl -17cm meter 0mL x3`，锁存 60 min 或到下一次正常排水；真实错误（读失败、控制拒绝、探针）优先。
- 快照 `flowMismatchCount`，`[HB]` 追加 `flowMis=`；每次评估串口一行 `[FLOW] pump 24s level 38.2->19.1 meter 930mL ok`。
- 阈值与 20 次排水的实测依据在 `config.h` FLOW WATCH 块。

## 操作员须知

1. 看板 lastError 出现 `flow mismatch` 就是泵到过滤器那根软管又脱了（或流量计上游堵/关）；x 后面是开机以来的次数。**若每一次排水都失配，先怀疑 I1 脉冲线/接线，不是软管。**
2. 修好后下一次正常排水自动清掉；不修，60 分钟后文字也会消失，但 `[HB] flowMis=` 计数留着。任何复位（看门狗、批 6.1 的 15 min 云断复位）都把计数和锁存清零——`flowMis` 归零不等于故障消失。
3. 管子半脱（本批只过了 0.3 L 那种）不报，是有意的；要更敏感改 `FLOW_WATCH_MIN_L`。
4. 排水期间 PLC 断线，那一次不判；泵运转不足 10 s 的点动也不判（串口 `a jog, not judged`）。

## 终审（opus，2026-09-08）

**裁决：可以 OTA。** Critical 0，Important 2（已修），Minor 11（修 4 条，余 7 条记录如下）。

已修：
- **Important-1** 液位尖峰落在起点拍 + 短点动 → 假降幅误报：加 `FLOW_WATCH_PUMP_MIN_MS 10000`，泵运转不足 10 s 不评估。
- **Important-2** PLC 断线分支不重写 `lastError`，锁存文本会粘住越过 60 min 并把掉线显示成流量失配：新增 `flowWatchRelease()`，解除或到期时若 `lastError` 仍是本模块的文本则改回 "ok"。
- M1 双写注释、M3 起点拍液位未读改为有日志、M5 NaN 门、M11 操作员须知补 I1 接线一条。

八个场景推演结论：`flowWatchTick` 是本拍 `lastError` 的最后写者（`sharedPublish` 不碰它），真实错误优先成立；断线/重连不会误判上升沿；`millis()` 比较全部回绕安全；`--3cm` 不可达；ON_CHANGE 不会因逐拍同文本刷流量；与批 6.1 复位门无交互。

留到下一批：
- M2 `pumpOk &&` 冗余（已顺手去掉）。
- M4 排水在断线期间开始、重连时泵仍转：正确地不起边沿，但无日志。
- M6 SETTLING 期间泵再启且到期时仍在转：评估落在第二次排水进行中，`pumpS` 只反映第一次；理论上两次拼接可误报，正常周期 10 min 概率极低。
- M7 `8.0f` 旁 "~2x margin" 只对正常 14–24 cm 成立；8 cm 临界降幅约 0.36 L，`MIN_L` 0.25 L 意味着亏 30% 就触发。注释精度问题。
- M9 `flow_watch.h` 用 `LOG` 但不含其定义，只能经 `plc_thread.h` 间接包含（与仓库既有惯例一致）。
- M10 `flowWatchTick` 没有自己的 where-code（无 I/O，实际取不到）。
- 断线期间 `lastError` 粘住上一拍文本是既有行为，本批只收回自己的字；彻底做法是断线分支每拍写 `plc offline`。
