# 批 7 终审记录 — 排水流量失配报警（flow watch）

范围：9f9ad5b..（本批最终提交）。spec：父仓库 `docs/superpowers/specs/2026-09-08-opta-gateway-mt-batch7-flow-watch-design.md`。

## 本批做了什么

- `flow_watch.h`（PLC 线程）：泵 `pumpCond` 起停为一次排水，泵停 45 s 后对照液位降幅与流量计升数；`降幅 ≥ 8 cm 且 升数 < 0.25 L` 判失配。
- 失配 → `lastError` 叠加 `flow mismatch: lvl -17cm meter 0mL x3`，锁存 60 min 或到下一次正常排水；真实错误（读失败、控制拒绝、探针）优先。
- 快照 `flowMismatchCount`，`[HB]` 追加 `flowMis=`；每次评估串口一行 `[FLOW] pump 24s level 38.2->19.1 meter 930mL ok`。
- 阈值与 20 次排水的实测依据在 `config.h` FLOW WATCH 块。

## 操作员须知

1. 看板 lastError 出现 `flow mismatch` 就是泵到过滤器那根软管又脱了（或流量计上游堵/关）；x 后面是开机以来的次数。
2. 修好后下一次正常排水自动清掉；不修，60 分钟后文字也会消失，但 `[HB] flowMis=` 计数留着。
3. 管子半脱（本批只过了 0.3 L 那种）不报，是有意的；要更敏感改 `FLOW_WATCH_MIN_L`。
4. 排水期间 PLC 断线，那一次不判。

## 终审
（待补）
