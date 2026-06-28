FROM huanheart/ai-service-based-on-http-framework-2

RUN apt-get update && apt-get install -y --no-install-recommends \
    libopencv-dev \
    && rm -rf /var/lib/apt/lists/*

# 创建应用程序专用库目录
RUN mkdir -p /opt/app-libs

# 将所有依赖库复制到 /opt/app-libs（不覆盖系统库）
COPY *.so* /opt/app-libs/

# 设置动态库搜索路径，优先使用 /opt/app-libs
ENV LD_LIBRARY_PATH=/opt/app-libs:/usr/local/lib

WORKDIR /root/httpserver_vsersion2/build
CMD ["./http_server", "-p", "8117"]
