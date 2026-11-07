# redstonesignal-parallel-optimizer

多线程并行红石 `CircuitSystem::evaluate`。命令 `/redstonesignal-parallel-optimizer` 简写别名为 `/rpo`，需要 OP。

## 命令

注意所有命令都需要管理员以及更高的级别

| 命令 | 说明 | 注意 |
| --- | --- | --- |
| `rpo thread_num <n>` | 线程数，普通服务器唯一需要配置使用的命令 | 上限为逻辑核心数 |
| `rpo mode <normal\|mchprs\|original>` | 切模式，支持 Tab 补全 | 见“模式” |
| `rpo speeds <n>` | mchprs 下每次 evaluate 的重复次数，用于加速 | 越大越容易一次卡顿 |
| `rpo compile` | 锁死当前红石的图的结构 | **图一变就崩服** |
| `rpo release` | 解除锁定 | |
| `rpo prof <ticks> [chunks]` | 统计接下来 `ticks` 个 tick 的 evaluate 耗时与元件数量，`chunks` 为每个维度输出的热点区块数，默认 3 | 见“性能统计” |
| `rpo set_parallel_threshold <n>` | 元件总数达到该值才走多线程，默认 4096，`normal` 与 `mchprs` 共用 | |
| `rpo set_normal_phase_threshold <n>` | `normal` 下单个相位（生产者 / 消费者）元件数不足该值时改由线程 0 串行处理，默认 2048 | |

## 模式

- `normal`：每次 evaluate 重新分类，四阶段并行；不跳红石粉，普通生存、生电场景使用，默认模式。
- `mchprs`：分类缓存 + `speeds`，恒定跳过红石粉（粉不参与 `mSources`，跳过不影响结果）；**不支持活塞、观察者等**。除主世界外的维度不做 evaluate。
- `original`：完全不优化，直接调用 BDS 原生 `CircuitSystem::evaluate`，用于对照 / 排查。

## 性能统计

`rpo prof <ticks> [chunks]` 启动后，会在服务端跑满 `ticks` 个 tick 时把结果通过聊天消息发给所有玩家（同时写日志），内容包含：

- 整体（所有维度求和）与每个维度各自一段；
- `evaluate` 的平摊耗时（ms/tick）、平均 / 最大 / 最小耗时（ms/次）、执行次数；
- 平均 / 最大 / 最小的元件总数、区块数；
- 平均 / 最大 / 最小的各类元件数量：红石粉、中继器、比较器、脉冲、火把、生产者、消费者、活塞、铁轨、充能方块、其它；
- 每个维度平均元件数最多的前 `chunks` 个区块（带维度）。

统计按 tick 计数，与 evaluate 触发次数无关；统计本身的开销不计入耗时。

## 特别注意

- **`compile` 危险**：锁死图结构和元件指针后，任何图结构变动（放/拆方块、活塞、观察者、区块卸载）都会让缓存指针悬空、直接崩服。只适合静态数电，改线前先 `release`。
- **`mchprs` 只对主世界生效**：其它维度直接跳过 evaluate，避免缓存指针跨维度失效。
- 线程数并非越多越快：`normal` 下缓存相位要写共享掩码数组，线程过多会互相抢缓存行；请按实际机器测出的最优值配置。

## 配置与构建

`config.json`：

| 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `mode` | `normal` | `normal` / `mchprs` / `original` |
| `thread_num` | 4 | 工作线程数 |
| `speeds` | 1 | 仅 `mchprs`，每次 evaluate 的重复次数 |
| `parallel_threshold` | 4096 | 元件总数达到该值才走多线程 |
| `normal_phase_threshold` | 2048 | 单个相位元件数不足该值时改串行 |

构建：`xmake -b -y`
