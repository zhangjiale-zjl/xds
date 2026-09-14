# HAL 页表生命周期回归测试

在 Linux 环境、仓库根目录执行：

```bash
make -C test check-mem-lifecycle
```

依赖 Python 3、GCC 或兼容的 C 编译器，以及 AddressSanitizer、UndefinedBehaviorSanitizer
运行库。测试将实际 `mem.c` 编译进用户态程序，以 pthread 实现同步原语并注入 HAL 行为。
构建产物位于自动清理的临时目录。测试不加载 ko，不需要 Ascend 驱动，也不访问块设备。

## 覆盖范围

| 用例 | 检查内容 |
| --- | --- |
| normal | 正常 get/put 配对，释放期间回调与 owner 解耦 |
| init-failure | 工作队列分配失败时释放 HAL 符号引用 |
| hal-get-failure | HAL get 失败时清理 XDS 对象 |
| prepare-invalidation | get 返回前发生回调，返回 `-ESTALE`，不等待尚未提交的 I/O |
| invalid-table | 页表校验失败时 get/put 配对 |
| copy-allocation-failure | PA 副本分配失败时 get/put 配对 |
| active-invalidation | 有效映射的回调等待 I/O，且可与 put 并发完成 |
| retry-unload | 两次 put 失败、owner 分离后的回调、重试成功，以及 worker 尚未退出时的卸载等待 |

默认启用 ASan/UBSan。只在编译器缺少运行库时，使用：

```bash
python3 test/run_mem_lifecycle_test.py --no-sanitize
```

脚本还会对实际 `compat.h` 做预处理检查：手工旧内核 NVMe request 适配限于 5.15.x；
5.4、5.10、5.14、5.16、5.19 被构建检查拒绝，6.6 进入独立的新内核适配路径。
该检查只验证版本选择，不代表已验证相应内核的完整构建或 NVMe 私有 ABI。

## 实机验证边界

这些用户态测试验证 C 实现的状态转换、资源配对和指定并发时序，不能替代目标内核的
模块编译、真实 HAL 行为、NVMe DMA 或硬件故障恢复验证。实机仍需检查 CRC、显式注销、
正常退出与模块卸载。HAL put 持续失败时，重试持有模块引用以保护回调上下文；
在途 NVMe 请求未完成时，内存失效回调仍需等待 DMA 停止后才能返回。
