# 批 11 终审记录 — 板子直写 InfluxDB：传输自测（阶段 1）+ 云断回放（阶段 2）

范围：a46301b..（本批最终提交）。spec：父仓库 `docs/superpowers/specs/2026-09-10-opta-gateway-mt-batch11-influx-direct-design.md`。

## 需求与取舍

Adam：云断导致的缺口要补进 InfluxDB，关键是**对齐**；不经 PC 中转。USB 日志方案作废（USB-C host 模式杀串口与 DFU）。

## 阶段 1（`bae05df` → 换 token 后 `f975cde`，2026-09-10）

- `influx_push.h`：BearSSL（云库在 Opta 上就是 BearSSL）+ ArduinoBearSSL 默认 13 个信任锚（含 Amazon Root CA 1）。**不能用 mbed `WiFiSSLClient`**：审查核出 `/wlan/` 证书包 48 张里没有 Amazon 根、也没有 Starfield 交叉签发者，且 mbedTLS 一次握手堆峰值约 100 KB。`BearSSLClient` 对象含约 18 KB 收发缓冲，放文件作用域。
- 自测写 `opta_selftest`（独立表），结果发 `pushStat`（Thing 里老固件留下的变量）。首个 token 得 403（无写权限），换 token 后 `ok code=204 ms=2573 heap 7048>1976>6528 n=1/0`，InfluxDB 里查到该行。
- 审查抓到：`WD_AT_PUSH` 早已定义为 4（我加的 18 重复）→ 复用 4；`snprintf` 返回值检查；堆峰值改在会话中取样；thing 名改宏。

## 阶段 2

- `influx_replay.h`（云线程）：云断期间每 10 s 一条 `ReplayRec`（136 B：epoch、okMask、28 传感器、flowRate/flowBatch/liveCum、plc），360 条环 = 60 min。阶梯复位前 8 KB 一块写 KVStore（标记之后、静默），开机读回并删键。云回来 2 min 后每趟最多一个 POST、3 条一批，行形 `arduino_iot,thing_name=IP_2_thing,variable_name=<云变量名> value=<x.xxx> <epoch>`，与 `usb_backfill.py` 已验证过的在线点同形。2xx 才删。
- 终审（opus）4 Critical 全修：
  - C1 成功后仍等 30 s 退避 → 360 条要 60 min 排空。改为成功后 2 s 即可下一批，约 5 min 排空；失败退避 30 s 起倍增；401/403/404 直接 15 min 顶。
  - C2 4 条/批装不进 10 KB 体 → 3 条。
  - C3 只有 2xx 才删 → 400/413/422 会永久堵住队头。改为这三类丢掉该批并计 `rej=`；401/403/404 与传输失败保留数据退避。
  - C4 水量与 plcConnected 无新鲜度门：PLC 未通时 `liveCum=0` 会每 10 s 往 `displayWaterVolume` 写 0；PLC 线程卡死时 `s_local` 冻结却 `ok[]` 仍真会造 28 路假数。改为快照年龄 > 6 s 不抓；`displayWaterVolume` 只在 `plcConnected` 时写（okMask bit 29）；flowRate/flowBatch 是 Opta 自己的表（bit 28）。
- Important 记录：I1 WiFi 自身断的云断抓不到（RTC 靠 NTP，`handle_SyncTime` 在 MQTT 前，只要 WiFi 通就有时间）；I2 新增约 67 KB 静态 RAM，`fordblks` 不是可用内存的口径；I3 装完后的**下一次 OTA** 才是 67 KB 的真正考试；I4 只有阶梯复位持久化，喂狗放手与 OTA 重启丢环；I5 meta 加了魔数+版本；I7 `pushStat` 被回放状态接管；I8 首趟回来先刷一次 `q=`；I9 只补 32 个变量（28 传感器 + 3 水量 + plcConnected），阀门/热泵不在其中。
- 12 条核实正确：槽位映射与 TAKE 列表 0 处不符；`value=1` 无 `i` 后缀是 float，与在线字段不冲突；`precision=s` 与既有回填一致。

## 操作员须知

1. 云断恢复后 2 分钟起，看板 `pushStat` 变成 `replay q=<待发> sent=<已发> posts=<次>/<败> last=<码> drop= rej=`，`q` 归零即补完。
2. 回放只补 28 个传感器、flowRate、flowBatch、displayWaterVolume、plcConnected；阀位、热泵、状态字不补。
3. 重启那 30 秒板子不在，谁也补不了；RTC 未对时（断电后到 NTP 前）的记录不保留。
4. 每次换 token 要重编 + OTA（编进固件）。token 只有对 `Atoco_Opta_Live` 的写权限。
5. 待查：批 10 起每次 OTA 计两次开机（n=63、n=65 无上报即重启）。
