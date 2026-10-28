cmake_minimum_required(VERSION 3.10)
project(http_server)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 OpenSSL 和 CURL
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# Gemma.cpp 预编译库路径（可通过环境变量覆盖）
set(GEMMA_BUILD_DIR "$ENV{GEMMA_BUILD_DIR}")
if(NOT GEMMA_BUILD_DIR OR GEMMA_BUILD_DIR STREQUAL "")
    set(GEMMA_BUILD_DIR /home/qiuku/gemma.cpp/build)
endif()
set(GEMMA_SOURCE_DIR "$ENV{GEMMA_SOURCE_DIR}")
if(NOT GEMMA_SOURCE_DIR OR GEMMA_SOURCE_DIR STREQUAL "")
    set(GEMMA_SOURCE_DIR /home/qiuku/gemma.cpp)
endif()

# 添加头文件路径
include_directories(
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/HttpServer/include
    ${PROJECT_SOURCE_DIR}/AIApps/ChatServer/include
    /usr/include/mysql-cppconn-8
    /usr/include/mysql
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
    /usr/include/opencv4          # OpenCV 头文件路径
    /usr/local/include            # ONNX Runtime
    /home/qiuku/.local/include    # AMQP-CPP (local install)
    ${GEMMA_SOURCE_DIR}           # Gemma.cpp 根目录
    ${GEMMA_BUILD_DIR}/_deps/sentencepiece-src/src  # SentencePiece
    ${GEMMA_BUILD_DIR}/_deps/highway-src             # Highway
    ${GEMMA_BUILD_DIR}/_deps/json-src/include        # nlohmann/json (gemma 依赖)
)

# 查找必要的库
find_library(MYSQLCPPCONN_LIBRARY
    NAMES mysqlcppconn mysqlcppconn8
    PATHS /usr/lib /usr/lib64 /usr/local/lib
)

find_library(ONNXRUNTIME_LIBRARY
    NAMES onnxruntime
    PATHS /usr/lib /usr/lib64 /usr/local/lib
)

# 添加源文件
file(GLOB_RECURSE HTTP_SERVER_SRC
    "${PROJECT_SOURCE_DIR}/HttpServer/src/*.cpp"
)

file(GLOB_RECURSE GOMOKU_SERVER_SRC
    "${PROJECT_SOURCE_DIR}/AIApps/ChatServer/src/*.cpp"
)

# 主程序文件
set(MAIN_SRC "${PROJECT_SOURCE_DIR}/AIApps/ChatServer/src/main.cpp")

# Gemma.cpp 预编译静态库（IMPORTED 目标）
add_library(libgemma STATIC IMPORTED GLOBAL)
set_target_properties(libgemma PROPERTIES IMPORTED_LOCATION ${GEMMA_BUILD_DIR}/libgemma.a)

add_library(hwy STATIC IMPORTED GLOBAL)
set_target_properties(hwy PROPERTIES IMPORTED_LOCATION ${GEMMA_BUILD_DIR}/_deps/highway-build/libhwy.a)

add_library(hwy_contrib STATIC IMPORTED GLOBAL)
set_target_properties(hwy_contrib PROPERTIES IMPORTED_LOCATION ${GEMMA_BUILD_DIR}/_deps/highway-build/libhwy_contrib.a)

add_library(sentencepiece-static STATIC IMPORTED GLOBAL)
set_target_properties(sentencepiece-static PROPERTIES IMPORTED_LOCATION ${GEMMA_BUILD_DIR}/_deps/sentencepiece-build/src/libsentencepiece.a)

# 添加可执行文件
add_executable(http_server
    ${MAIN_SRC}
    ${HTTP_SERVER_SRC}
    ${GOMOKU_SERVER_SRC}
)

# 链接库
target_link_libraries(http_server
    pthread
    muduo_net
    muduo_base
    mysqlcppconn
    mysqlclient
    ssl
    crypto
    ${CURL_LIBRARIES}
    ${OPENCV4_LIBRARIES}
    /usr/lib/x86_64-linux-gnu/libopencv_core.so
    /usr/lib/x86_64-linux-gnu/libopencv_imgproc.so
    /usr/lib/x86_64-linux-gnu/libopencv_highgui.so
    /usr/lib/x86_64-linux-gnu/libopencv_imgcodecs.so
    /usr/lib/x86_64-linux-gnu/libopencv_videoio.so
    /usr/lib/x86_64-linux-gnu/libopencv_dnn.so

    ${ONNXRUNTIME_LIBRARY}        # 自动查找的 ONNX Runtime 库

    /home/qiuku/.local/lib/libamqpcpp.a   # AMQP-CPP

    # Gemma.cpp 本地推理引擎（顺序重要：libgemma → hwy → hwy_contrib → sentencepiece）
    libgemma
    hwy
    hwy_contrib
    sentencepiece-static
)

# 如果库不在默认路径，添加链接目录
link_directories(/usr/local/lib /home/qiuku/.local/lib)

# 打印调试信息
message(STATUS "Include directories:")
get_property(dirs DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY INCLUDE_DIRECTORIES)
foreach(dir ${dirs})
    message(STATUS "  ${dir}")
endforeach()

message(STATUS "ONNX Runtime library: ${ONNXRUNTIME_LIBRARY}")
message(STATUS "Main source file: ${MAIN_SRC}")
message(STATUS "HTTP Server sources: ${HTTP_SERVER_SRC}")
message(STATUS "Gomoku Server sources: ${GOMOKU_SERVER_SRC}")
