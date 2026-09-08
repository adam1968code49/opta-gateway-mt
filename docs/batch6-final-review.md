# 批 6 终审记录 — 云端线程 + 三预算喂狗 + 加固

范围：c9c050a..（本批最终提交）。终审 2026-09-08（opus）。**裁决：可以 OTA，无必修项。**

## 本批做了什么

- `Cloud*` 与 `ArduinoCloud.update()` 搬进云端线程（`cloud_thread.h`，栈 24 KB，每趟 sleep 20 ms）。主线程只剩 web 配置页、串口配置、面板灯（读 `g_ledBits`）。
- 喂狗线程三心跳三预算：主/PLC 60 s，云 300 s。放手时按"超出自己预算比例最大"的线程写 KVStore 标记 `wd giveup @<码> <线程> <秒>s`，然后 **`NVIC_SystemReset()`**——因为 ArduinoIoTCloud 在 `update()` 内自己踢硬件狗、WiFi 驱动也踢（`WiFi.setFeedWatchdogFunc`），光停止踢狗不会复位。这是本批最关键的一处，逐任务审查（opus）抓出来的。
- 位置码 14（WiFi 断重关联）、15（WiFi 通云未连：DNS/TLS/NTP）、2 只表示已连接轮询。
- `loopStallMs/stallWhere` 改发 10 分钟滑动窗口最大值；`stackFree` 改为云端线程栈。
- 加固：云探针 DNS 否定缓存 60 s；`readExact` 整体截止 3×1500 ms；`sendRRData` 失步早返回收会话；`LOG` 互斥；`shared.h` 翻译单元守卫；`build.sh` 告警门（本仓库文件告警即失败；打开告警后修了 2 条：`who` 未用、`available()` 弃用）。

## 场景推演（终审）

| 场景 | 行为 |
|---|---|
| 上游断 3 分钟，WiFi 正常，域名已解析 | 探针每 5 s 敲 8885 失败 → `update()` 一次都不跑；云端线程每趟 20–520 ms，`[HB]` 照常，`stallWhere` 停 15。看板空 3 分钟后自愈，SYNC 后总闸需重翻。**不重启。** |
| 上游断 10 分钟 | 同上，线性延长，不复位。只有 `update()` 单次卡 >300 s 才复位，`bootReason="wd giveup @15 cloud 30Xs n=.."`。 |
| 冷启动时上游就断 | DNS 15 s 一次、60 s 否定缓存 → 每分钟一趟 15 s，安全；恢复最坏晚 60 s。 |
| WiFi 断 | 码 14，`update()` 单趟最坏 ~20 s，远低于 300 s。其余线程不受影响。 |
| PLC 断 | 不变，但**后果变了**：PLC 线程超 60 s 现在会真复位（批 5 时主线程的 `update()` 在替它踢硬件狗）。`eip.begin()` 历史最长 27.7 s，余量约 2 倍。看到 `bootReason=@5 plc 6Xs` 先怀疑预算太紧，不是真卡死。 |

**注意计划 Task 8 Step 2 的预期写反了**：拔上游 3 分钟时 `[HB]` **不应停**（探针门让线程快速回转）。现场若 `[HB]` 停了，反而是新发现——说明 `update()` 在探针放行后仍在内部长阻塞。

## 终审后已顺手改的两条

- `wd_feeder.h`：`bootMarkIntentional(tag)` 提到 LOG 之前（卡在串口的线程可能持着 `g_logMutex`，标记不能等它）。
- `EtherNetIP.cpp`：`j + len1 > bodyLen || len1 > replyCap` 那行退回 `return false`——body 已完整读完，不是失步；否则回复稍大就进"读→断会话→重连"风暴。

## 留到下一批

1. `g_ledBits` 三个位全由云端线程算：云端卡住时面板 PLC 灯和 web 页 "PLC link" 冻结在旧值。PLC 位应由 PLC 线程发布。
2. `loopMs` 语义翻转（现在含 `update()`，与 `cloudMs` 几乎重复）；`stackFree` 量级变化。Grafana 上会有台阶，需标注变更时刻。
3. `build.sh` 的门比看起来弱：mbed_opta `platform.txt` 里 `compiler.warning_flags.default` 为空，`--warnings default` 只有 gcc 缺省告警。要真把关用 `--warnings more` 并一次性清掉新暴露的。grep 依赖路径含 `opta-gateway-mt/`。
4. `[HB]` 仍是十几段 `LOG()`，别的线程可以整行插入；`snprintf` 拼一行或 scoped lock。
5. `wifiRssi = WiFi.RSSI()` 在 `WD_AT_NONE` 下执行，是真驱动调用；给诊断块一个码。
6. `handleConfigClient` 头字段循环无条数/总时长上限（每行 500 ms），慢客户端理论上能把主线程按出 60 s。加"最多 32 行 / 3 s"。
7. `s_wdRefusals`/`wdRefusals()`/`wdSeen()` 已是死代码。
8. `thingProperties.h` 三处注释仍写"main pass / main thread stack / BOTH threads"。
9. 批 5 留项 1–6 未动：`hpFails` 上云、`hpDataStale` 语义、`hpDataAgeS` 跨掉线累计、`config.h` 死的 `HP_*` 宏、Thing 里 8 个设定点仍可写、属性序号备查。
10. 整数比例死区、双空行、启动日志顺序：关闭。

## 操作员须知（上板后）

1. 开机 `bootReason` 应为 `... n=45`；`stallWhere` 开机应是 **15**，不再是 2；`stackFree` 现在是云端线程栈（8000–14000 量级）；`loopStallMs` 会自己回落。
2. 云端断线不再重启板子。看板空一段、云灯灭，是正常的"等上游"，最长这样待 5 分钟才动作。
3. 真复位时 `bootReason` 自己说话：`@15 cloud`（连接阶段）、`@2 cloud`（轮询）、`@14 cloud`（WiFi 重关联）、`@5 plc`（PLC 连接）、`@1 main`（web 页）。**`@5 plc 6Xs` 先怀疑 60 s 预算太紧。**
4. 云端卡住期间面板 PLC 灯和 web 页 "PLC link" 冻结在旧值，那段时间看 Studio 5000。
5. 云端线程活着就能再 OTA；哪怕进了"每 5 分钟复位"循环，每次开机都有 5 分钟窗口推下一版。彻底连不上云才需到现场（RJ45 web 页改 WiFi，或 USB DFU）。
6. 控制仍是批 1 那套：每次开机、每次云端重连后，`controlEnabled` 关一次再开。

## 最值得串口验证的一点

上板头 5 分钟：`[HB]` 是否持续每 3 s 打印、`cloudMs` 与 `loopMs` 差值很小——同时证明云端线程在自己栈上跑、探针门让线程快速回转、`wdBeatCloud()` 每趟落地。然后拔上游 3 分钟，**预期 `[HB]` 不停**。
