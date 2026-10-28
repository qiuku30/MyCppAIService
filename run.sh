#!/bin/bash
export DB_HOST=127.0.0.1 DB_USER=root DB_PASSWORD=123456 DB_NAME=ChatHttpServer
export RABBITMQ_HOST=127.0.0.1
export DASHSCOPE_API_KEY=sk-your_dashscope_key_here
export DOUBAO_API_KEY=ark-your_doubao_key_here

echo "正在加载 Gemma 模型（约 15 秒）..."
cd /home/qiuku/ai-project/build2
exec ./http_server -p 8118
