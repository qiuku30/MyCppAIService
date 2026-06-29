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
- **容器化部署** — 一键 `docker compose up`

## 快速开始

### 环境要求

- Docker & Docker Compose
- 或手动安装：C++17 编译器、CMake、muduo、OpenCV、ONNX Runtime、MySQL Connector C++、SimpleAmqpClient、libcurl、OpenSSL

### 使用 Docker 部署

```bash
cd deploy
cp .env.example .env
# 编辑 .env 填入 API Key
docker compose up -d
```

### 手动编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
./http_server -p 8117
```

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
