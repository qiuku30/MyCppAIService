> 2026-10-30 后续稳定性修复：日志、Session 生命周期与 JPEG/PNG 资源边界已完成源码和完整构建/联调验证，详见 [最新记录](../../tests/stability/RESULTS.md)。下面保留 2026-10-29 的构建及 ORT 修复历史证据。

# 2026-10-29 本地完整构建与联调记录

当前源码已在 Mac 上的 Colima Linux ARM64 环境完成全部 42 个 C++ 编译单元的编译和链接。真实 HTTP、MySQL、RabbitMQ 及图片推理功能验证通过。ORT 初始化的 48 字节泄漏已修复：修复镜像通过独立 Env、真实图片 ASan/UBSan/LSan、完整应用干净构建及 56 项联调复验。Gemma 权重在 Windows 上，真实文本生成仍缺少输入条件。详见 [ORT 修复记录](../../tests/integration/ORT_LEAK_FIX.md)。

## 验证结果

| 项目 | 结果 | 原始证据 |
| --- | --- | --- |
| Linux 依赖镜像、Gemma/Highway/SentencePiece 静态库 | 通过 | [环境清单](/Users/kudoshinichi/.local/share/mycppaiservice-integration/environment-manifest.json) |
| 完整应用编译、动态依赖及 protobuf 符号隔离 | 修复 ORT 后干净重编译 42 个单元，动态依赖无缺项 | [干净构建](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/evidence/app-clean-build.log)、[实际二进制与依赖](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/evidence/running-binary-libraries.log) |
| 真实核心联调 | 8 个阶段、56 项检查通过 | [ORT 修复后完整运行日志](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/evidence/live-full.log) |
| 真实 OpenCV/ORT 图片 C++ 合同 | 11 项通过 | [本次正常合同](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/installed-image/normal.log)、[资源合同](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/installed-image/resource.log) |
| 真实图片 HTTP | 3 张正例分类/分数一致；极端 PNG 返回 400 | [ORT 修复后 HTTP 回执](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/evidence/image-http.json) |
| 现有 P1/P2 完整普通回归 | 通过，exit 0 | [本次回归日志](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/evidence/p1-p2-regression.log) |
| 首次完整构建的 VM 冷启动与持久目录复验 | 当时 13 条聊天记录的行数、内容摘要完全一致；本轮 ORT 升级另外核对相同 13 条记录摘要 | [重启前](/Users/kudoshinichi/.local/share/mycppaiservice-integration/vm-restart-before.json)、[重启后](/Users/kudoshinichi/.local/share/mycppaiservice-integration/vm-restart-after.json)、[历史复验](/Users/kudoshinichi/.local/share/mycppaiservice-integration/live-after-vm-restart.log)、[图片复验](/Users/kudoshinichi/.local/share/mycppaiservice-integration/image-check/http-after-vm-restart.json) |
| Gemma 缺少权重时的真实 SSE | 返回 model-not-loaded，数据库没有新增聊天行；生成能力记为 unavailable | [回执日志](/Users/kudoshinichi/.local/share/mycppaiservice-integration/gemma-unavailable-live.log) |
| ORT 修复后的 ASan/UBSan/LSan | 独立 Env 5 进程共 50 次生命周期、图片正常/资源 11 项均 exit 0；保留旧版 RED | [修复回执](/Users/kudoshinichi/.local/share/mycppaiservice-integration/ort-fix/evidence/summary.json)、[原始失败回执](/Users/kudoshinichi/.local/share/mycppaiservice-integration/image-asan/sanitizer-results.json) |

核心联调的 56 项检查分布：环境 3、HTTP 19、认证 11、生产 MQManager 发布 2、持久化 4、应用重启 5、MySQL 重启 5、RabbitMQ 重启与重连 7。

认证验证了真实 PBKDF2 存储、独立密码校验、不同盐值、SQL 字面量用户名以及未认证访问拒绝。消息验证了真实生产类投递、三次重复发布只形成两条聊天行、引号/反斜杠/中文保真、旧 SQL 消息隔离，以及重启后的角色、时间戳和用户归属。管理 API 注入的数据与真实生产 MQManager 投递有独立标记；没有把入队接受值当作发布确认回执。

## 本轮修复

1. **构建可靠性**：增加 APT 下载重试、BuildKit 包缓存、可选 ARM64 镜像和明确的 pkg-config 依赖。首次代理 502 和连接超时有保留日志；后续通过直连签名仓库完成安装。
2. **muduo ARM64 编译**：保留数值长度断言，将该架构的格式化预留从 32 改为 48 字节，适配 binary128 long double。
3. **Gemma 的 GCC 11/C++17 兼容**：以[一行补丁](../../deploy/gemma-cxx17-compat.patch)将模板分支的断言改为依赖查询类型，仍拒绝不支持的类型。
4. **真实启动崩溃**：GDB 复现了 main 前的 protobuf 初始化 SIGSEGV。Linux 链接现将 SentencePiece 静态库的内置 protobuf 符号设为私有，避免覆盖 OpenCV 的系统 protobuf；重启与动态符号检查通过。[原始调用栈](/Users/kudoshinichi/.local/share/mycppaiservice-integration/startup-gdb.log)
5. **图片识别**：修正 RGB、短边 256、居中裁剪、ImageNet 均值/标准差和 NCHW；校验模型输入输出与 1000 个标签；用真实 logits 的稳定 softmax 返回置信度，替换固定的 0.95。原有字符串预测接口保持可调用。
6. **图片资源预算**：resize 前限制中间图不超过 16 Mi pixels。49 KiB、1×16384 的图片原本会申请 3 GiB 中间图；安全分配观察器建立了 RED，最终在分配和推理前拒绝，HTTP 返回 400。
7. **ORT 初始化泄漏**：定位 ARM64 cpuinfo 的全局缓存所有权被 LTO 移除，以限定目标的编译补丁重建真实 ORT；独立 Env、图片 sanitizer、完整干净构建及服务联调均复验通过，见 [修复记录](../../tests/integration/ORT_LEAK_FIX.md)。

图片修复保留了独立证据：[最初 C++ RED](/Users/kudoshinichi/.local/share/mycppaiservice-integration/image-check/cpp-red-run.json)、[HTTP 固定分数 RED](/Users/kudoshinichi/.local/share/mycppaiservice-integration/image-check/http-red.json)、[资源 RED](/Users/kudoshinichi/.local/share/mycppaiservice-integration/image-check/resource-red-run.json)。图片资产来自固定版本的官方 ONNX Model Zoo，并经过 SHA256 校验。

联调脚本另修正了两项测试假设：204 响应不要求 Content-Length；连续请求使用保持连接的正常入口，终止响应另验证尾部请求不会继续执行。这些属于测试修正，未据此修改正确的 HTTP 服务器行为。

## 尚未通过或未覆盖

- **Gemma 真实生成**：尚无 Mac 本地 `.sbs` 权重和 `.spm` tokenizer；未用其他模型冒充 Gemma。依赖库已完整编译，缺失文件的失败路径已经实际验证。
- **云端生成**：测试环境没有配置云端 API Key，未执行真实云端生成、RAG 或付费调用。
- 本次为隔离服务功能验证，没有形成吞吐/延迟压测、HTTPS、生产旧库迁移或异常断电保证。

## 当前环境与复用

- 服务：[http://127.0.0.1:18117](http://127.0.0.1:18117)。DB、RabbitMQ 和管理端口只在专用 Docker 网络内开放。
- VM：`mycppaiservice`，2 CPU、4 GiB 内存、40 GiB 数据盘；Docker context 为 `colima-mycppaiservice`。
- [独立持久目录](/Users/kudoshinichi/.local/share/mycppaiservice-integration)：Gemma 源码/静态库、图片模型、私有测试配置、测试账号状态和全部回执。生成的 `stack.env` 与账号状态为 mode 0600，请勿上传。
- 实际库版本：OpenCV 4.5.4、ONNX Runtime 1.18.0（已应用 cpuinfo 所有权修复）、MySQL 8.0.46、RabbitMQ 3.13.7；Gemma 固定为 `6abb6b1` 加兼容补丁。当前默认依赖镜像已更新，版本/哈希和本轮完整复验见 [ORT 修复记录](../../tests/integration/ORT_LEAK_FIX.md)。
- 自动审批曾拒绝整个项目的可写 VM 挂载。最终采用项目只读、独立依赖目录可写；[内核挂载标志与代理状态](/Users/kudoshinichi/.local/share/mycppaiservice-integration/vm-mount-verification.log)已核对，临时 Docker 代理覆盖已停用。

在项目根目录复用：

```sh
export IT_ROOT=/Users/kudoshinichi/.local/share/mycppaiservice-integration
export DOCKER_CONFIG="$IT_ROOT/docker-config"
export IT_DOCKER_CONTEXT=colima-mycppaiservice
colima --profile mycppaiservice start --activate=false --ssh-config=false
docker --context "$IT_DOCKER_CONTEXT" compose --env-file "$IT_ROOT/stack.env" \
  -f tests/integration/compose.yml -f tests/integration/image_mounts.yml \
  up -d mysql rabbitmq control app
python3 tests/integration/run.py --state "$IT_ROOT/harness-state.json" \
  --evidence-dir "$IT_ROOT/evidence" \
  --stage prepare,http,auth,producer,persistence,restart,db-restart,broker
```

Windows 模型文件后续可放入该持久目录的 `models/`，再在私有配置中指定对应文件名并重启应用；默认文件名为 `2.0-2b-it-sfp.sbs` 和 `tokenizer.spm`。真实生成验证需在文件到位后另行执行。

原有 P1/P2 工作未丢失：首次完整构建起点的 204 个文件没有删除；当时 195 个保持相同内容，9 个涉及构建、图片实现、部署测试及进度文档。后续 ORT 修复新增了依赖构建脚本、补丁、泄漏探针和记录，没有修改业务 C++ 源码；此前图片实现的哈希已重新核对。以上为提交前的验证快照，当前提交状态以 Git 历史为准。
