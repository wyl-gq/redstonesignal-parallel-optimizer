# redstonesignal-parallel-optimizer

多线程并行红石 `CircuitSystem::evaluate`。命令 `/redstonesignal-parallel-optimizer` 简写别名为 `/rpo`，需要 OP。

## 命令

注意所有命令都需要管理员以及更高的级别

| 命令 | 说明 | 注意 |
| --- | --- | --- |
| `rpo set_thread_num <n>` | 线程数，普通服务器唯一需要配置使用的命令 | 上限为逻辑核心数 |
| `rpo set_mode <normal\|mchprs>` | 切模式 | 见“模式” |
| `rpo set_speeds <n>` | mchprs 下每次 evaluate 的重复次数，用于加速 | 越大越容易一次卡顿 |
| `rpo compile` | 锁死当前红石的图的结构 | **图一变就崩服** |
| `rpo release` | 解除锁定 | |
| `rpo prof` | 统计接下来 20 次 evaluate | 多维度不准，仅数电调试用用 |
| `rpo lever <x> <y> <z> <true\|false>` | 内部调试：直接写元件强度 | 正常游戏别用，开发者测试用的 |
| `rpo set_layered <true\|false>` | 实验开关 | 别动 |

## 模式

- `normal`：每次 evaluate 重新分类，四阶段并行；不跳红石粉，普通生存，生电场景使用，默认模式。
- `mchprs`：分类缓存 + `speeds`，恒定跳过红石粉（粉不参与 `mSources`，跳过不影响结果）；**不支持活塞、观察者等**。

## 特别注意

- **`compile` 危险**：锁死图结构和原件指针后，任何图结构变动（放/拆方块、活塞、观察者、区块卸载）都会让缓存指针悬空、直接崩服。只适合静态数电，改线前先 `release`。
- **`rpo lever` 是内部调试命令**：直接写元件强度、绕过方块更新，只配合 `compile` 调试用；
- **只支持单维度**：多维度同时有红石活动时 `mchprs` 和 `rpo prof` 会串数据。

## 配置与构建

`config.json`：`mode`（默认 `normal`）、`thread_num`（默认 4）、`speeds`（默认 1，仅 `mchprs`）。

构建：`xmake -b -y`
