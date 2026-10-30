多线程并行红石信号的计算，使用线程数可以在配置中设置, 默认线程数为4, 上限为设备逻辑核心数。

如果没有优化效果，可能的原因如下：

1. 本身卡顿就小。
2. 红石卡顿不是以信号为主，插件仅对信号计算 `evaluate` 进行优化。
3. 设备为单核。
4. 一个设备开多个服务器，每个服务器都使用优化插件线程冲突等相互干扰问题

命令(没有简写命令)：

- `redstonesignal_parallel_optimizer set_thread_num <thread_num>`：设置线程数，立即生效。
- `redstonesignal_parallel_optimizer set_skip_redstone_dust <true|false>`：开启后不计算红石粉(`TransporterComponent`)。

配置项：

- `thread_num`：线程数，默认 4。
- `skip_redstone_dust`：是否跳过红石粉，默认 `false`。
