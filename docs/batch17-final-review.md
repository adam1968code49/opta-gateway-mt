# 批 17 终审记录 — 板子成为 IP2 在 InfluxDB 的唯一写入者(常态直写)

范围:`df5e31e..10ddbf4`。spec:父仓库 `docs/superpowers/specs/2026-09-18-opta-gateway-mt-batch17-influx-live-design.md`。

## 需求与决定

Adam(2026-09-18):把批 11"仅云断时直补"升级为**常态也由板子直写 InfluxDB**;数据先进 RAM,**每 10 秒发一次**(原提 2 分钟,为保看板实时改为 10 秒,与采样同步);时间戳**秒级**;断网仍缓存 **1 小时**。AWH_Bridge 对 `IP_2_thing` 的转发已由 Adam 在桥的设置里关掉(桥继续服务 Zeus/IP1/开发板,不能整停);从 **09:33 PT 起 IP2 在库里空白**,直到本批上板。标签不变(`thing_name=IP_2_thing`、`arduino_iot`、`value`/`value_str`),看板零改动。**不过开发板,直接上 IP2**(IP2 本来就是试运行机)。

## 四层数据(`influx_feed.h` 新增,`influx_replay.h` 改)

| 层 | 内容 | 节奏 | 断网缓存 | RAM |
|---|---|---|---|---|
| 1 | 现有 32 连续量(28 传感器、流量、累计、plcConnected) | 10 s 采样即发 | 现有环 360 条 = 1 h,阶梯复位前仍落 KVStore | 0 |
| 2 | 热泵 15 模拟量、hpDataAgeS、水量 5、板诊断 9 = 30 | 120 s | 环 30 条 = 1 h,不落 KVStore | 3.8 KB |
| 3 | 阀/泵/故障/热泵位与字/设定/比例阀位置(死区 0.5)/序列字/计数/三个开关 = 73 | 值变才发;每个新快照比对;开机首读即全量基线;**每 10 min 重发全量(心跳)** | 队列 256,满覆盖最老 | 3.3 KB |
| 4 | lastError/plcFailTag/plcStateText/doorStat/lacoStat/fwVersion/bootReason → `value_str` | 值变才发;同受 10 min 心跳 | 队列 16 | 2.9 KB |

实测静态 RAM +10.2 KB(248808 → 259048),与审查估算一致。

**发送**:第 1 层每 10 s 采样后置 flush;云线程每 pass 最多一个 POST,body 按 1(≤3 条)→2→3→4 顺序装到 10 KB,装不下的等下一 POST;2xx 出队,400/413/422 出队计 `rej`(第 3 层被拒条目重新武装、下次快照重发),其它失败保留退避。**稳态限一 POST/10 s**;积压(第 1 层 > 3 条或第 3 层半满)全速排空(3 条/2 s,1 h 积压约 4–5 min)。门沿用批 11:云回来先稳 2 min;短抖动不用探针路;OTA 期间停。

## 终审(Opus,2026-09-18)→ 修复(`10ddbf4`)

- **C1 无心跳** — 桥原来每次云更新都重发全部值;改板子直写后开关/设定"变化才发",Grafana `last()` 查 6 小时在跑了 3 天的板上返回 No Data,P&ID 叠加看板全空。→ 每 `FEED_REBASE_MS` 10 min 清空"已发"标记,下一快照全量重发第 3/4 层(约 80 行,10 min 一次)。
- **C2 排空无时间上限** — `influxPush` 在进入时打时间戳、一次 POST 约 2.3 s,所以只要第 3/4 层有东西(PLC 下载期间 lastError 每 2 s 翻)就会 2.3 s 连发,`update()` 被饿成每 2.3 s 一次——正是历史掉线的形态。→ 稳态门:上次 POST 不满 10 s 且无积压则不发。
- **I1** `pushStat` 每次 POST 都变、每 10 s 一次 String 发布 → 只在失败、队列刚空、或每 5 min 发。
- **I2** 被拒的第 3 层变化会永久丢到下一次变化 → 被拒条目 `s_l3Sent` 复位重发(仅拒绝路径,覆盖路径不做以免活锁)。
- **M4** 字串每 20 ms pass 比对 → 按快照 seq 比对。**M5** 第 2–4 层拒绝单独计数,`rej=` 仍指第 1 层记录。
- 核实无误(Q1):四种结果下的出队簿记无漏发无重发;`u1==0`/`n1==0` 两条早退分支正确。Q4:全部 `precision=s` 整秒,数值全 float 无 `i` 后缀。Q5:转义缓冲 248 足够 122 个引号。Q6:用钉死的 `arm-none-eabi-g++ 7.2.1` 实测无告警。Q7:云线程栈增量 ≤ 500 B。
- 未采纳:M2(`st[128]` 理论上超长,实际约 100 字)、M3(积压期第 2 层等第 1 层排完,环 1 h 够)。

## 验收(2026-09-18 14:38–14:44 PT,IP2 上电后)

IP2 断电改硬件约 5 h;14:06 曾连上云一瞬(只发出 SYNC 时的 `controlEnabled=false`)又掉,14:38 稳定回来,OTA 立即投递、成功,`fwVersion 10ddbf4`,n=91→93。5 分钟内:

| 项 | 结果 |
|---|---|
| 库里恢复写入 | 1210 行、142 个变量(桥时期 160,差的是有意不进库的按钮/自指量) |
| 第 1 层节奏 | `t1HotTank` 32 点,间距全部 10 s,毫秒位全 0 |
| 第 2 层 | `hpLoopTemp`/`uptimeS` 每 120 s 一点 |
| 第 3 层 | `valveS1` 基线 + 1 次变化;`hpHtgSp1`/`posS4`/`pumpCond`/`controlEnabled` 各 1(基线) |
| 第 4 层 | `plcStateText`/`doorStat`/`lastError` 以 `value_str` 落库 |
| pushStat | `live q=0/0/0/0 sent=32+120 posts=22/0 last=204 drop=0 rej=0+0 badt=0` |
| 云线程 | `cloudMs` 2 ms(POST 在 update() 之外,不计入);`wifiRssi` −17 |
| **RAM** | `heapFree` 开机 8600、330 s 时 **8168**;自检握手时 8600→**3528**。太贴地 → 批 17.1 |

### 批 17.1:缓冲缩半(同日)

`FEED_SLOW_MAX 30→15`(慢变量断网缓存 1 h→30 min)、`FEED_CHG_MAX 256→128`、`FEED_STR_MAX 16→8`,静态 RAM 259048→254568(−4.5 KB);**第 1 层 32 个连续量的 1 小时缓存不变**。

上板(`05b2529`,15:07 PT,n=94):开机 271 s 时 `heapFree` **12432**(17 同时点 8168);10 s 节奏、ms=0、`posts=16/0`、`cloudMs` 3 ms。

**事后更正**:库里第 2 层的 `heapFree` 样本显示 10ddbf4 跑到 25 min 时稳态是 **16.1–16.4 KB**——我据以决定缩半的 8168 是开机 5 min 的瞬态,不是稳态。17.1 因此不是必需的,但它把握手低点(约 −5 KB)的余量做厚了,保留。教训:堆要看 ≥20 min 的稳态,不看开机值。

**留项**:(a) 开机 2 min 稳定期内字串队列 8 条不够,`drop=4`(7 条基线 + 几次 lastError/doorStat 变化),被 10 min 心跳补回,无数据后果;要更严可把 `FEED_STR_MAX` 回到 12(+512 B)。(b) 每次开机第 2 层的第一条 `heapFree`/`uptimeS` 样本是 0(诊断 CloudInt 尚未算出),可在 `feedCaptureSlow` 里对 `uptimeS==0` 跳过诊断位。

## 回滚(批 17.3,2026-09-18 晚)— 直写功能撤下,保留"截止时间门"

**现象**:17.1 上板后 2 h 43 min 稳跑,随后上游(Starlink)在傍晚反复抖动,板子接连重启:n=95(未留下记录)、n=96 `off 22m p=4/22 fo=3`(阶梯复位,探针 26 次失败 22 次)、**n=97 `wd giveup @4 cloud 301s`**、n=98 `off 22m p=23/12`。Adam 决定**暂时回滚到稳定版、不上直写功能,同时把这类问题尽量修掉**。

**根因(`wd giveup @4`)**:`influxPush()` 里 BearSSL 的握手循环 `run_until()`(ssl_io.c:123–131)对底层 `read()` 的"暂无数据"只是 `continue`,无超时,只靠 `connected()` 变 false 退出;而 mbed 的 `MbedClient::connected()` = `status() || available()`,`status()` **只反映 WiFi 接口是否在线,不反映 TCP 对端**。上游在握手中途一抖,TCP 半开、接口仍在线 → 云线程在 where=4 自旋到 300 s 预算耗尽 → 看门狗放弃重启。批 11 这条路只在断网后走几次,撞不上;批 17 每 10 s 走一次,第一个坏天气的傍晚就成了重启风暴。

**修(17.2,保留)**:`influx_push.h` 新增 `DeadlineClient`——夹在 `WiFiClient` 和 `BearSSLClient` 之间的 `Client` 包装,`influxPush()` 进入时武装 `INFLUX_DEADLINE_MS` 20 s;到点后 `connected()` 返 0、`read()` −1、`write()` 0,BearSSL 的 `clientRead/clientWrite` 把它们变成 IO 错误,`run_until` 让引擎失败,一次 POST 以 code −4 结束。不改库。Opus 复审:(C1)门过期后 `BearSSLClient::stop()` 整体被 `if(connected())` 跳过、半开 socket 和读线程漏到下次重试 → 在 `s_pushSsl.stop()` 后再 `s_pushTcp.stop()`(有界、幂等);(I2)>20 s 的慢成功会被覆写成 −4 → 只在 `code<0` 时覆写;(I3)`WiFiClient::connect()` 在门之外:DNS 5 s×3 + 裸 TCP 无 `set_timeout`(SYN 重试约 60–90 s),单次最坏约 130 s,仍在 300 s 内但只有 2 倍余量 → 超 30 s 记日志;`using Print::write;` 卫生项。

**回滚**:`influx_replay.h`、`cloud_thread.h` 恢复到批 15 时的版本(= 板上稳定版 `cac5e18` 的行为:仅云断时缓存、恢复后回放),删除 `influx_feed.h`。**批 17 的四层直写设计和实测结果留档在上文**,以后要恢复:从 `05b2529` 取回 `influx_feed.h` 与两处改动,叠加本节的门即可。

**同晚另见**(老代码路径,非本批):`loopStallMs` 228 s @14(`WiFi.begin` 关联)和 94 s @16(诊断块的 `WiFi.RSSI()` 驱动调用)——WiFi 驱动在 AP 抖动时长时间卡住,看门狗按设计兜底。17.3 顺手把 `WiFi.RSSI()` 改为仅在 `WL_CONNECTED` 时读,减少一半暴露面;@14 那一处无法从固件侧加超时。

**后果**:回滚后 IP2 的常态历史又依赖 AWH_Bridge——桥对 `IP_2_thing` 的转发已由 Adam 重新打开(2026-09-18 21:00 PT)。

### 根因更正(2026-09-18 21:30):是连接节奏耗尽了 lwIP 的 TCP 池,不是"放大老毛病"

Adam 指出改前 3 天无事;库里的开机标记证实:09-12 ~05:12 起板子连续运行 **271811 s = 3.1 天**,唯一一次非断电重启是 09-18 02:01 的 `off 20m p=8/0`(探针全通、仅 MQTT 断——老的罕见 wedge)。改后 3 h 内 4 次重启,且签名不同:`p=4/22`、`p=23/12`——**探针也大面积失败**,老固件上从未出现。

机制:Opta 核心 `MBED_CONF_LWIP_TCP_SOCKET_MAX = 4` → `MEMP_NUM_TCP_PCB = 4`(整块板同时最多 4 个 TCP 控制块,含 TIME_WAIT),`TCP_MSL = 60000` → 客户端主动关闭的连接在 TIME_WAIT 里占 PCB **120 s**。批 17 每 10 s 一条 HTTPS 连接并关闭 → 稳态需要约 12 个 TIME_WAIT PCB,池子只有 4 个,还要让给 MQTT(1)、PLC EtherNet/IP(1)、探针。lwIP `tcp_alloc` 池尽时先杀最老 TIME_WAIT,再不够就**杀活着的低优先级连接**——MQTT 被踢、探针建不起、EIP 会话可能被踢,于是"路由器正常、板子全线失联",随后半开连接上的 TLS 握手又把云线程挂到 300 s。原提的 2 min 一发正好 = 一个 TIME_WAIT 周期,处在安全边界;改成 10 s 时没有对照这个池子,是设计失误。

**遗留风险(下一项)**:批 11 的回放排空是恢复后每 2 s 一条连接,同样会撞 4-PCB 池(一次 1 h 积压 = 120 条连接)——可能也是过去几次"刚回来又掉"的成因。任何直写/回放都应改为**复用一条长连接(HTTP keep-alive)**,或把连接间隔拉到 ≥ 120 s;`TCP_SOCKET_MAX` 编在核心的 libmbed 里,sketch 改不了。

## 操作员须知

1. IP2 的历史数据现在**只**来自板子;桥对 IP2 已关,别再开回来(会双写)。
2. 断网 1 小时内的数据全部自动补齐;超过 1 小时丢最老的连续量、慢变量。
3. `pushStat` 四个数是四层队列深度,常态应接近 `0/0/0/0`,持续增长说明写库不通。
4. 后续如 TLS 开销(每 10 s 一次握手)在 `cloudMs`/堆上不理想,可改 HTTP keep-alive 复用连接(占一持久 socket,lwIP 上限 4,另评估)。
