# 2026-10-29 ORT 初始化 48 字节泄漏修复

Linux ARM64 下的 ORT 初始化泄漏已修复。修复后的真实运行库、生产图片识别代码及完整应用均已验证；泄漏检测保持启用。当前默认依赖镜像已更新，旧镜像另存用于回退。

## 根因与改动

官方 ORT 1.18.0 固定使用 cpuinfo 提交 `959002f82d7962a473d8bf301845f2af720e0aa4`。其 ARM64 初始化会建立三张表：uarch 表、Linux CPU 到 processor 的映射、Linux CPU 到 core 的映射。在本次双核 Linux VM 中分别占 16、16、16 字节。

上游代码把这些数组交给全局变量，再清空局部指针以避免清理路径释放它们。这些缓存按 cpuinfo 原设计保留到进程退出，`cpuinfo_deinitialize()` 为空。官方 ARM64 二进制的优化结果保留了分配，却移除了三个未被当前消费者读取的私有全局所有权写入，使这 48 字节变成不可达分配。

因果实验使用**同一份真实 cpuinfo 源码、相同工具链和私有共享库边界**：实际启用 LTO 时稳定报告 48 字节；仅关闭该依赖的 IPO 后，CPU 元数据仍有效，LSan 正常退出。补丁在父工程仍要求 IPO 时同样通过。实验显式设置 CMP0069=NEW 并核对实际 `-flto` 参数，排除了旧版 CMake 策略静默忽略 IPO 的假对照。

[所有权补丁](../../deploy/ort-cpuinfo-ownership.patch)仅对 Linux ARM64、GNU/Clang 的两个 cpuinfo 目标禁用 IPO，并使用 `-fno-lto`。CPU 检测、`-O3`、默认全部 CPU 算子、ML/contrib 算子及其余 ORT 模块的 LTO 均保留。缓存继续由原有全局变量持有和复用。

[构建脚本](../../deploy/build-ort.sh)固定 ORT 1.18.0 提交 `45737400a2f3015c11f005ed7603611eaed306a6`，校验下载与源码提交，应用补丁后重建运行库。[依赖镜像配方](../../deploy/Dockerfile.build)的 ARM64 路径调用该脚本；最终镜像只拷贝运行库 SDK。该修复没有修改业务 C++ 实现。

Eigen 的旧 ZIP SHA1 已变化；本次把 ZIP 的 1,784 个文件逐一与原固定提交的 Git blob 比较，零缺失、零差异。构建脚本改用同一个经过校验的 Git 提交，不忽略归档校验，也不更换 Eigen 版本。

## 测试结果

| 验证 | 结果 |
| --- | --- |
| 原版最小 `Ort::Env` 程序 | exit 1，48 字节 / 3 次分配；新回归探针同样检出 |
| cpuinfo 同源码 LTO 对照 | 原版 LTO ON：exit 1；OFF 控制组及补丁组：exit 0，CPU 元数据有效 |
| 修复镜像中的独立 Env 探针 | 5 个独立进程，每进程 10 次创建/析构；全部 exit 0 |
| 原来报错的同一个探针二进制，加载修复库 | exit 0，仍为真实 ORT 1.18.0 |
| 真实 OpenCV/ORT 图片 ASan/UBSan/LSan | 正常 10 项 + 资源 1 项通过，两种模式均 exit 0 |
| 图片推理结果保留 | 三张图片的生产分类和置信度与修复前一致，最大置信度差为 0 |
| 完整应用干净构建 | 全部 42 个 C++ 编译单元重新编译并成功链接，动态依赖无缺项 |
| 真实服务联调 | 8 阶段、56 项通过，exit 0；包含 HTTP、认证、真实 MQManager、MySQL 持久化及重启重连 |
| 图片 HTTP | 3 张图片的类别和分数通过；极端尺寸 PNG 返回 400 |
| 升级与服务恢复的数据保留 | 原有 13 条测试聊天记录摘要完全相同；随后联调新增的测试数据另由合同校验 |
| P1/P2 普通回归、最终部署合同、BuildKit 检查 | 全部 exit 0；BuildKit 无警告 |

Sanitizer 参数为 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`LSAN_OPTIONS=exitcode=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`。未添加 suppression。探针和生产图片代码进行了 ASan/UBSan 插桩，第三方运行库内部没有全面插桩；这里的结果覆盖实际运行中的分配泄漏和已插桩源码检查。

一次早期诊断目录内的图片运行与 SDK 重装交叠，出现无有效回溯的 SEGV。该轮日志完整保留，不用于产品或验收结论。最终 Env/图片验收在封装完成的固定镜像内独立执行：网络关闭、项目和模型只读、仅测试输出目录可写，两个进程模式均正常退出。

## 产物与复用

- 当前镜像：`mycppaiservice-build:integration`，同时标记为 `mycppaiservice-build:ort-leak-fixed`。
- 实际镜像 ID：`sha256:4fda1161ab8a1bab89587cf72ace880dcc67964ace425fc35ad1a3f9b4b64b49`。
- ORT 库 SHA256：`32347fb2f6c1ae9fbfd2dc4a59f0e286ffc02f354866e16d7c5883aacd3a370d`。
- 旧镜像：`mycppaiservice-build:ort-leak-red-20261029`。
- 服务：[http://127.0.0.1:18117](http://127.0.0.1:18117)，现已恢复运行。
- [汇总回执](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/evidence/summary.json)：源码/镜像/库哈希、各项退出码、功能配置与边界。
- [初始化检查](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/evidence/installed-env-green.log)、[图片正常检查](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/installed-image/normal.log)、[资源检查](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/installed-image/resource.log)、[联调日志](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/evidence/live-full.log)、[图片 HTTP](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/evidence/image-http.json)。

[探针说明与复现命令](../../tests/integration/ort_leak/README.md)已加入源码。现有联调启动命令仍见 [RESULTS.md](../../tests/integration/RESULTS.md)，默认镜像标签已指向修复版。

完整 ORT helper 在 Linux 内实际构建并验证；本轮复用先前已验证的基础 SDK 封装修复运行库，没有重复执行 Docker 基础依赖的所有冷构建步骤。新多阶段配方经过实际 BuildKit 检查。x64 的原有归档路径未在本轮执行。Gemma 权重/tokenizer 仍在 Windows，云端 Key 未配置，真实 Gemma/云端生成不在此次通过范围内。本记录保存提交前的验证快照，当前提交状态以 Git 历史为准。
