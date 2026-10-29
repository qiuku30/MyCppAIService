# 2026-10-30 稳定性边界修复与验证

本轮按已批准的设计完成日志、登录 Session 生命周期、JPEG/PNG 资源边界三个任务。原有 P1/P2、图片预处理及 ORT 泄漏修复均保留，验证基于本地工作区完成，提交状态以 Git 历史为准。整体独立复审已通过，无 Critical/Important 遗留问题；以下测试结果均已实际执行。

设计：[稳定性边界](../../docs/superpowers/specs/2026-10-30-stability-boundaries-design.md)；计划：[执行计划](../../docs/superpowers/plans/2026-10-30-stability-boundaries.md)。原始回执统一保存在 [持久证据目录](/Users/kudoshinichi/.local/share/mycppaiservice-integration/stability-20261030/evidence)。私有账号、密码、Cookie 不进入源码或公开报告。

## 修复行为

| 范围 | 修复后行为 |
| --- | --- |
| HTTP、路由、中间件、语音日志 | 去掉响应正文、原始未知路径、Origin 和异常详情等打印；仅记录进程内请求编号、方法、受信路由模板/固定类别、状态、耗时和普通响应序列化字节数。路由标签限长并转义。 |
| SSE 日志 | 记录 `stream_open`；不把开启流当作生成完成，也不把序列化字节数称为客户端已收到字节数。 |
| 登录 Session | 使用单调时钟判定闲置过期；内存存储定期清理未再访问的过期对象；同步刷新、清理和容量检查；容量满返回 503，失败时不下发新 Cookie，不驱逐有效 Session。 |
| 生命周期 | 清理线程可唤醒并由析构函数 join；请求持有的 Session 在管理器销毁后仍能安全操作本地数据，自动保存不会访问已销毁的管理器。 |
| 图片解码 | 只支持 JPEG/PNG；在解码前检查格式、头部、原始像素和预计 BGR 字节预算，在 HTTP 模型查找/构造前预检；解码后再次核对类型和尺寸，保留 EXIF 方向与原有预处理/置信度行为。 |
| 图片并发 | 所有 file/Mat/buffer 入口共用进程级 RAII 配额；文件入口先获取配额再读文件；超额立即返回 503，正常及异常路径均归还配额。 |

Session 默认闲置期限 3600 秒、容量 10000、清理间隔 60 秒；可通过构造参数覆盖，非正值拒绝。旧自定义存储保留串行兼容默认方法；并发自定义后端需要覆盖生命周期操作，才能获得内存后端实现的原子刷新、容量和对象身份保证。

图片参数在进程首次使用时读取一次，须在启动前设置；仅接受正十进制整数，拒绝溢出，编码长度还需能由 OpenCV 的 int 表示。Compose 和示例配置已接入同名参数，详见 [部署说明](../../deploy/README.md)。

| 参数 | 默认值 |
| --- | --- |
| `IMAGE_MAX_ENCODED_BYTES` | 16777216，16 MiB |
| `IMAGE_MAX_ORIGINAL_PIXELS` | 16777216，16 Mi pixels |
| `IMAGE_MAX_BGR_BYTES` | 67108864，64 MiB |
| `IMAGE_MAX_CONCURRENT_JOBS` | 2 |

HTTP 图片格式不支持/损坏返回 400，新增的原始解码/编码预算超限返回 413，配额耗尽返回 503。既有极端长宽比造成的 resize 预算错误仍为 400。HTTP 层原有 10 MiB 请求体上限继续生效，因此 JSON/base64 上传可接受的实际图片字节数也受它限制；16 MiB 编码上限还覆盖直接调用 C++ buffer/file 接口的场景。

## 有效 RED 与 GREEN

| 问题 | 旧源码实际失败 | 修复后 |
| --- | --- | --- |
| 日志泄露 | 实际 HttpServer 源码测试 HTTP 41 项中失败 23 项、语音 8 项中失败 2 项；独立 Linux INFO 网络服务泄露 5 个合成敏感标记，exit 1。 | 普通与 sanitizer 源码检查 49/49；相同真实 muduo 网络请求未泄露标记，普通响应、异常响应、未知路由、SSE 的线上内容检查通过。 |
| Session 清理/生命周期 | 公开 `cleanExpiredSessions()` 无法回收仅由存储持有的过期对象，exit 1；其他分阶段 RED 包括旧管理器回调的 ASan UAF、容量竞争和旧后端过期会话被重新激活。 | 20 项实际源码场景在普通、ASan/UBSan、TSan 模式通过；复审发现的兼容后端问题增加 4 项行为断言并修复，原始独立复现转绿。 |
| 解码前预算 | 有效的 5000×5000 压缩 PNG 触发旧入口申请 75000000 字节解码缓冲，安全分配观察器拦截后 exit 1。未向系统提交该大额分配。 | 同一入口在解码器分配前拒绝，观察到申请次数 0、推理次数 0，exit 0。 |

日志网络验证使用真实 muduo/HttpServer 与合成业务回调，避免接触真实凭据。完整应用的 main 默认日志级别为 WARN，其最初的零字节捕获记为“不可验证”，没有计作隐私通过或 RED；有效网络对照来自明确启用 INFO 的独立服务。

## 最终验证

| 验证 | 结果 | 证据位置 |
| --- | --- | --- |
| 既有 P1/P2 合并回归 | 普通、ASan/UBSan 均通过；包含 HTTP、Session、数据库、Gemma 源码路径、历史、RAG 和部署契约。 | `controller/final/p1-p2-{normal,asan}.log` |
| 本轮日志 | 源码 HTTP 41 + 语音 8 项普通及 ASan/UBSan 通过；真实 INFO 网络 RED/GREEN 通过。 | `controller/logging/`、`controller/log-network-{red,green}.json` |
| 本轮 Session | 20 项普通/ASan/TSan 通过；真实 Linux 网络 11 项通过。 | `controller/session/`、`controller/session-network-green.json` |
| 本轮图片 | 127 项边界场景 + 9 项配置检查，普通及 ASan/UBSan/LSan 均通过；每次 29 次真实成功 ORT Run。 | `controller/image/final-{normal,asan}.log` |
| 既有真实图片契约 | 最终源码重新验证 11 项模型/张量/分类/分数/标签契约及极端 resize 资源契约，普通与 ASan/UBSan/LSan 均通过。 | `controller/final/image-regression-{normal,asan}.log`、`../image/final-*/` |
| Linux 完整干净构建 | 42 个 C++ 编译单元重新编译并链接成功，实际应用启动成功，动态依赖无缺项。 | `controller/final/app-final-build.log`、`running-binary-libraries.log` |
| 真实 HTTP/MySQL/RabbitMQ | 8 阶段、56 项通过：环境 3、HTTP 19、认证 11、真实生产者 2、持久化 4、应用重启 5、MySQL 重启 5、RabbitMQ 重启/重连 7。 | `controller/final/live56.log`、`live/` |
| 真实图片 HTTP 分类 | 3 张 PNG 的分类和 softmax 分数与独立 oracle 一致；旧极端长宽比返回 400。 | `image-http.json` |
| 真实图片 HTTP 资源错误与恢复 | 6 个场景、25 项断言通过：BMP 400、大尺寸 PNG 413、截断 PNG 400；此后 baseline JPEG、progressive JPEG、PNG 均成功推理。 | `image-resource-http.json` |
| 部署参数 | 12 项部署回归；基础/可选图片 compose 配置和默认值/覆盖值解析通过。 | `controller/image/deploy-followthrough-tests.log`、Task3 报告 |

Session 真实网络使用容量 2、TTL 2 秒、清理间隔 25 毫秒的隔离策略，验证容量 503 且无 Cookie、旧 Cookie 继续可用、显式删除后容量复用、不再访问的对象自动回收及持有对象跨管理器替换。替换期间没有并发调用原始管理器接口，不声称允许任意线程与管理器析构并发。

图片并发通过真实 ORT Run 入口的条件变量屏障占住两个独立识别器，验证第三个 buffer/Mat/file 入口返回 503，释放后可再次推理。正常路径向实际库转发原始输入，一次注入 Run 错误仅用于配额释放验证。真实应用 HTTP 没有执行确定性的 503 并发压测；503 的源入口和 HTTP 状态映射有独立覆盖。

图片负例中的 `libpng error: IDAT: incorrect header check` 是故意损坏 IDAT 的预期解码诊断。256 次有界字节变异仅作为终止/内存安全 smoke 检查，不能解释成 256 个具有格式判定 oracle 的测试，也不能替代完整 fuzzing。

各任务分别完成独立复审，最终整体复审核对了 44 个源码/部署/测试文件与增量包哈希，结论为要求满足、Approved，无 Critical/Important 遗留。两项非阻塞提示（预期 libpng stderr、变异 smoke 的判定范围）已明确记录。完整复审见 `controller/final-review.md`。

## 数据、依赖与隔离边界

应用切换前后既有 19 条测试聊天记录数量和内容摘要一致：`10e5e0ce9eac9dd700eafad9412bd3a1f8e04de10e285a384bfabe1d5ad95789`。之后真实生产者及 broker 恢复测试按预期增加专用测试消息；没有把有意插入的测试消息当作历史损坏。应用/MySQL 重启前后的记录另由 56 项联调核对。

实际运行镜像仍为 `sha256:4fda1161ab8a1bab89587cf72ace880dcc67964ace425fc35ad1a3f9b4b64b49`，OpenCV 4.5.4、ORT 1.18.0。修复后的 ORT 库 SHA256 仍为 `32347fb2f6c1ae9fbfd2dc4a59f0e286ffc02f354866e16d7c5883aacd3a370d`。未重建或替换依赖，LSan 保持启用，无抑制规则。项目目录在 VM 和容器中仍为只读，构建、证据和服务数据使用独立可写目录/卷。

本轮起点的 230 个文件均未删除；业务源修改均在批准范围内。原有修复、数据结构和 `.serena` 保留。最终文件摘要清单见 `controller/final/preservation.json`；文档、进度与新增测试单独列出。

64 MiB 限制约束预计 BGR 输出缓冲，不是进程 RSS 上限，也不涵盖第三方 codec 临时内存、模型实例缓存、已接收的 HTTP/base64 字符串或调用方已有的 Mat。两个识别器测试进程的观测 maxRSS：普通 147656 KiB，sanitizer 513580 KiB；没有据此承诺任意输入的内存或耗时上界。推理线程池、模型实例复用/回收、整进程内存上限和延迟/吞吐压测留待后续任务。

Gemma `.sbs` 权重与 `.spm` tokenizer 仍在 Windows，Mac 无文件；云端 API Key 也未配置。本轮没有验证真实 Gemma/云端生成，不构成打包发布或用户验收。

## 复现入口

普通回归：`MYCPP_TEST_DEPS=/private/tmp/mycpp-p1-deps bash tests/p2/run_all.sh normal`；将最后参数换为 `asan` 执行合并 sanitizer 回归。新增源码场景：`tests/stability/logging/run.sh normal`、`tests/stability/session/run_tests.sh normal`；Session 额外支持 `asan`、`tsan`。

真实图片和 sanitizer 的完整 SDK 挂载、fixture 生成方式见 [图片测试说明](../../tests/stability/image/README.md)。使用专用 Docker config/context，项目 `/project:ro`、模型 `/models:ro`、持久 SDK 输出 `/out:rw`；不要将宿主机未共享的 `/private/tmp` 用作 VM 输入绑定。

真实 HTTP 资源验证先在相同 OpenCV SDK 编译 `tests/stability/image/http_fixtures.cpp`，生成四种小图；再用 `make_fixture.py` 生成 `oversize.png`。运行：

```sh
python3 tests/stability/image/http_probe.py \
  --container mycpp-it-app --port 18117 \
  --fixtures /Users/kudoshinichi/.local/share/mycppaiservice-integration/stability-20261030/image/http-fixtures \
  --receipt /Users/kudoshinichi/.local/share/mycppaiservice-integration/stability-20261030/evidence/image-resource-http.json
```

脚本会核验容器专用标记和 loopback 端口，使用随机专用测试账号，Cookie/密码仅保存在内存。完整服务联调复用 [既有联调入口](../../tests/integration/RESULTS.md)，本轮阶段为 `prepare,http,auth,producer,persistence,restart,db-restart,broker`。
