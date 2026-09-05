# 批 5 终审记录 — 热泵经 PLC

范围：8bf4468..bf7f74b（4 个任务）。终审 2026-09-05（opus）。**裁决：可以 OTA，无必须现在修的项。**

## 做了什么

- `hpEnable` → `HP_Enable`，`hpModeCool` → `HP_Mode_CoolRequest`，走批 1 的命令路径（SYNC 静默期 / 总闸 / PLC 在线 / PLC 线程再查总闸）。
- 每 6 s（`tickN % 3 == 1`，与状态字拍错开）读 29 REAL（3 组 MSP）+ 11 BOOL（1 组 MSP，旧固件是 11 次单读）进快照；主线程只在 `hpSeq` 变化时赋 40 个属性。
- 8 个设定点/delta 注册为 **READ** 回显 PLC 真值。库源核实：`READ` 属性的云端下发与首次同步回放在 `PropertyContainer.cpp:108` 的 `isWriteableByCloud()` 门就被整体丢弃，不写本地、不触发回调；`applyCommand` 没有对应 case。
- `hpHtgHighLimit`、`hpManualOverride` 不注册。
- 热泵读失败不写 `lastError`、不点 FAULT、不改 `plcConnected`。
- 快照约 868 B（`< 960` / `< 1024` 断言均过）；flash 487508，RAM 145656。

## 留到下一批

1. `hpFails` 在快照里但没有云属性——热泵读失败在看板上不可见（与批 1 遗留的 `stateFails` 同类）。**下一批最该做。**
2. `hpDataStale` 把"HP_In 冻结"和"标签 PLC 里根本没有"混成一个信号（12 个监视槽全读不到时也会 5 分钟后置 true）。旧固件同病。
3. `hpDataAgeS` 跨 PLC 掉线继续累计。
4. `config.h` 的 `HP_STATUS_ENABLE/HP_CONTROL_ENABLE/HP_POLL_INTERVAL_MS/HP_STALE_DETECT/HP_SP_*/HP_ALLOW_*` 在新固件里无引用（只有 `HP_STALE_TIMEOUT_MS` 是活的）；`config.h:179`、`:202` 的注释已过时。补 `#if` 或改注释。
5. 云端 Thing 里 8 个设定点仍是 READ_WRITE，看板还给滑块；建议在 Arduino Cloud 改成只读。
6. 属性 105→149 使库内属性序号移位；库按名字解析所以正常，若云端改用整数轻载荷会错位。备查。
7. 批 1/2 全部遗留仍在，`readExact` 无整体截止排第一。

## 操作员须知

1. 热泵两个开关和其他按钮一样：每次开机、每次云端重连后，`controlEnabled` 关一次再开，`hpEnable`/`hpModeCool` 才会到 PLC；早翻会弹回并显示 `control: syncing, wait 15 s`。
2. 看板上的 `hpEnable` 不是热泵真实状态，`hpEnableSt` 才是；`hpModeCool` 的真值看 `hpCoolRequest`。重连后 `hpEnable` 可能被写回 OFF 而 PLC 里 `HP_Enable` 仍是 1——网关从不主动关热泵。
3. 8 个设定点只读回显：看板上拖了不下发，6 s 后弹回。改设定去 Studio 5000。
4. `hpDataStale=true`、`hpDataAgeS` 一直涨 = HP_In 12 个模拟量 5 分钟没动。PLC↔热泵链路本来就断的话这是正确信息。
5. 热泵读失败只在串口 `[HP] read FAIL:` 可见；命令结果 "hp enable set" 约 2 s 后被 "ok" 覆盖。

## 上板后最值得看的一点

`hpHtgSp1`/`hpHtgSp2`（槽 21/22）是 40 个槽里唯一能与 Studio 5000 已知值直接对照的：一致则整张 29 槽 REAL 表可信；是 0 或荒谬值则先别信任何 `hp*` 读数，查 MSP 分块 0 与 UDT 成员对齐。同时看 `loopStallMs` 与 `n`：每 6 s 多 4 次 CIP 交换，若 `n` 跳或 `loopStallMs` 上台阶，是批 1 遗留第 1 条被放大。
