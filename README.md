# C++ AI 应用服务平台

基于自研 C++ HTTP 框架构建的 AI 应用服务平台，支持多模型对话、RAG 检索增强、MCP 工具调用、语音合成/识别、图像识别等功能。

## 技术栈

| 类别 | 技术 |
|------|------|
| 语言 | C++17 |
| 网络库 | muduo (Reactor 模式) |
| HTTP 框架 | 自研 (支持 HTTPS、路由、中间件、会话管理) |
| 数据库 | MySQL 8.0 (连接池 + 异步写入) |
| 消息队列 | RabbitMQ (生产者池 + 消费者线程池) |
| AI 推理 | ONNX Runtime、OpenCV |
| 语音 | 百度 TTS/ASR API |
| HTTP 客户端 | libcurl |
| 容器化 | Docker + Docker Compose |

## 项目结构

```
├── HttpServer/          # 自研 C++ HTTP 服务框架
│   ├── http/            # HTTP 请求/响应解析、服务器核心
│   ├── router/          # 静态路由 + 正则动态路由
│   ├── middleware/       # 中间件链 (含 CORS)
│   ├── session/         # Cookie 会话管理
│   ├── ssl/             # SSL/TLS (基于 OpenSSL)
│   └── utils/           # MySQL 连接池、JSON、文件工具
├── AIApps/ChatServer/   # AI 业务逻辑层
│   ├── AIUtil/          # 策略模式、MCP 工具调用、语音、图像识别、MQ
│   ├── handlers/        # HTTP 请求处理器
│   └── resource/        # HTML 模板、配置文件
├── deploy/              # Docker 部署配置
└── CMakeLists.txt
```

## 功能

- **多模型对话** — 支持阿里百炼、豆包等模型，策略模式 + 注册式工厂实现模型切换
- **MCP 工具调用** — 自研轻量级 Model Context Protocol，LLM 自主决策调用外部工具
- **RAG 检索增强** — 向量检索 + 知识库，支持带引用的回答
- **多会话管理** — 单用户多会话，独立上下文隔离
- **语音合成/识别** — 集成百度 TTS/ASR
- **图像识别** — 本地 ONNX 模型推理 + OpenCV 预处理
- **异步持久化** — RabbitMQ 解耦数据库写入，主线程不阻塞
- **容器化部署** — Compose 管理应用、MySQL 与 RabbitMQ

## 快速开始

### 环境要求

- Docker 与 Docker Compose v2，或匹配目标平台的 C++17 / CMake 3.16+ 工具链。
- 依赖包括 muduo、AMQP-CPP（启用 Linux TCP）、OpenCV、ONNX Runtime、MySQL Connector/C++、MySQL C 客户端、libcurl、OpenSSL、nlohmann/json，以及指定版本的 Gemma 静态库。
- Gemma 的源码、静态库和依赖头文件必须与目标 Linux 架构及编译器 ABI 一致。

### 使用 Docker 部署

先按 [Linux 构建与部署说明](deploy/README.md) 构建依赖镜像和 Gemma，再配置：

```bash
cd deploy
cp .env.example .env
# 编辑 .env 中的凭据、Gemma 宿主机目录及模型文件名。
docker compose config --quiet
docker compose build ai-service
docker compose up -d
```

图像识别通过 [可选模型挂载配置](deploy/README.md#optional-image-recognition-assets) 提供 ONNX 模型和标签文件。

新 MySQL 数据目录会在 `MYSQL_DATABASE` 指定的数据库中执行初始化 SQL。已有数据卷需要显式迁移，详见部署说明；更改数据库名不会搬迁旧数据。

### 手动编译

安装上述依赖并构建匹配版本的 Gemma 后：

```bash
cmake -S . -B build \
  -DGEMMA_SOURCE_DIR=/absolute/path/to/gemma.cpp \
  -DGEMMA_BUILD_DIR=/absolute/path/to/gemma.cpp/build
cmake --build build --parallel 2
# 在当前 shell 配置数据库、RabbitMQ、API 及模型文件环境变量。
bash run.sh -p 8117
```

非系统安装前缀、依赖版本、模型挂载、旧库迁移和实际 Linux 验证边界均见 [部署说明](deploy/README.md)。

## 测试与验证

- [P1/P2 源码回归](tests/p2/README.md)：`bash tests/p2/run_all.sh normal`；另支持 `asan` 和 `tsan`，依赖准备见测试说明。
- [真实 Linux 构建与联调](tests/integration/README.md)：隔离 HTTP、MySQL、RabbitMQ，使用真实 OpenCV/ORT 图片推理。
- [稳定性修复与验证记录](tests/stability/RESULTS.md)：日志隐私、Session 生命周期、JPEG/PNG 资源预算和并发限制。
- [ORT 初始化泄漏修复](tests/integration/ORT_LEAK_FIX.md)：保留可复现的源码补丁、泄漏探针和验证边界。

2026-10-30 的验证记录覆盖完整 Linux 构建、56 项真实服务联调及相关 sanitizer 检查。真实 Gemma/云端生成需要另行提供模型文件或 API 配置；详细覆盖范围见各测试记录。

## 架构

```
客户端请求 → HttpServer (muduo) → Router → Middleware → Handler
                                                    ↓
                                              AIHelper (策略编排)
                                              ↙        ↘
                                    AIStrategy (多模型)   AIConfig (MCP协议)
                                              ↓           ↓
                                       外部 LLM API    AIToolRegistry (工具执行)
                                              ↓
                                        MQManager → RabbitMQ → MySQL
```

## License

MIT
