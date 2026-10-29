cmake_minimum_required(VERSION 3.16)
project(http_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui imgcodecs videoio dnn)

# Cache arguments take precedence over the environment. Both trees must match
# the pinned Gemma revision, target architecture and compiler ABI (deploy/README.md).
set(GEMMA_SOURCE_DIR "$ENV{GEMMA_SOURCE_DIR}" CACHE PATH "Gemma source tree")
set(GEMMA_BUILD_DIR "$ENV{GEMMA_BUILD_DIR}" CACHE PATH "Gemma build tree")
set(ONNXRUNTIME_ROOT "$ENV{ONNXRUNTIME_ROOT}" CACHE PATH "ONNX Runtime installation prefix")

find_path(MYSQLCPPCONN_INCLUDE_DIR mysql_driver.h
    PATH_SUFFIXES mysql-cppconn-8/jdbc mysql-cppconn-8 mysql-cppconn/jdbc)
find_library(MYSQLCPPCONN_LIBRARY NAMES mysqlcppconn)
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h)
find_library(MYSQLCLIENT_LIBRARY NAMES mysqlclient)
find_path(MUDUO_INCLUDE_DIR muduo/net/TcpServer.h)
find_library(MUDUO_NET_LIBRARY NAMES muduo_net)
find_library(MUDUO_BASE_LIBRARY NAMES muduo_base)
find_path(AMQPCPP_INCLUDE_DIR amqpcpp.h)
find_library(AMQPCPP_LIBRARY NAMES amqpcpp)
find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    HINTS "${ONNXRUNTIME_ROOT}" PATH_SUFFIXES include include/onnxruntime/core/session)
find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime
    HINTS "${ONNXRUNTIME_ROOT}" PATH_SUFFIXES lib lib64)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
    HINTS "${GEMMA_BUILD_DIR}/_deps/json-src/include")

find_path(GEMMA_INCLUDE_DIR gemma/gemma.h HINTS "${GEMMA_SOURCE_DIR}"
    NO_CMAKE_FIND_ROOT_PATH)
find_library(GEMMA_LIBRARY NAMES gemma HINTS "${GEMMA_BUILD_DIR}"
    NO_CMAKE_FIND_ROOT_PATH)
find_path(HIGHWAY_INCLUDE_DIR hwy/base.h
    HINTS "${GEMMA_BUILD_DIR}/_deps/highway-src" NO_CMAKE_FIND_ROOT_PATH)
find_library(HIGHWAY_LIBRARY NAMES hwy
    HINTS "${GEMMA_BUILD_DIR}/_deps/highway-build" NO_CMAKE_FIND_ROOT_PATH)
find_library(HIGHWAY_CONTRIB_LIBRARY NAMES hwy_contrib
    HINTS "${GEMMA_BUILD_DIR}/_deps/highway-build" NO_CMAKE_FIND_ROOT_PATH)
find_path(SENTENCEPIECE_INCLUDE_DIR sentencepiece_processor.h
    HINTS "${GEMMA_BUILD_DIR}/_deps/sentencepiece-src/src" NO_CMAKE_FIND_ROOT_PATH)
find_library(SENTENCEPIECE_LIBRARY NAMES sentencepiece
    HINTS "${GEMMA_BUILD_DIR}/_deps/sentencepiece-build/src" NO_CMAKE_FIND_ROOT_PATH)

function(require_dependencies)
    foreach(variable IN LISTS ARGN)
        if(NOT DEFINED ${variable} OR NOT EXISTS "${${variable}}")
            message(FATAL_ERROR
                "Missing dependency ${variable}. Set -D${variable}=<path>, "
                "CMAKE_PREFIX_PATH, or the documented Gemma build/source paths.")
        endif()
    endforeach()
endfunction()

require_dependencies(
    MYSQLCPPCONN_INCLUDE_DIR MYSQLCPPCONN_LIBRARY MYSQL_INCLUDE_DIR MYSQLCLIENT_LIBRARY
    MUDUO_INCLUDE_DIR MUDUO_NET_LIBRARY MUDUO_BASE_LIBRARY
    AMQPCPP_INCLUDE_DIR AMQPCPP_LIBRARY
    ONNXRUNTIME_INCLUDE_DIR ONNXRUNTIME_LIBRARY NLOHMANN_JSON_INCLUDE_DIR
    GEMMA_INCLUDE_DIR GEMMA_LIBRARY HIGHWAY_INCLUDE_DIR HIGHWAY_LIBRARY
    HIGHWAY_CONTRIB_LIBRARY SENTENCEPIECE_INCLUDE_DIR SENTENCEPIECE_LIBRARY
)
if(NOT EXISTS "${AMQPCPP_INCLUDE_DIR}/amqpcpp/linux_tcp/tcpconnection.h")
    message(FATAL_ERROR "AMQPCPP_INCLUDE_DIR must provide AMQP-CPP Linux TCP headers.")
endif()

# Imported libraries retain explicit paths; no user-specific global link directories.
add_library(gemma_external STATIC IMPORTED)
set_target_properties(gemma_external PROPERTIES IMPORTED_LOCATION "${GEMMA_LIBRARY}")
add_library(highway_external STATIC IMPORTED)
set_target_properties(highway_external PROPERTIES IMPORTED_LOCATION "${HIGHWAY_LIBRARY}")
add_library(highway_contrib_external STATIC IMPORTED)
set_target_properties(highway_contrib_external PROPERTIES
    IMPORTED_LOCATION "${HIGHWAY_CONTRIB_LIBRARY}"
    INTERFACE_LINK_LIBRARIES highway_external)
add_library(sentencepiece_external STATIC IMPORTED)
set_target_properties(sentencepiece_external PROPERTIES
    IMPORTED_LOCATION "${SENTENCEPIECE_LIBRARY}")
target_link_libraries(gemma_external INTERFACE
    highway_contrib_external highway_external sentencepiece_external)

file(GLOB_RECURSE HTTP_SERVER_SRC CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/HttpServer/src/*.cpp")
file(GLOB_RECURSE CHAT_SERVER_SRC CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/AIApps/ChatServer/src/*.cpp")
add_executable(http_server ${HTTP_SERVER_SRC} ${CHAT_SERVER_SRC})
target_include_directories(http_server PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/HttpServer/include"
    "${PROJECT_SOURCE_DIR}/AIApps/ChatServer/include"
    "${MYSQLCPPCONN_INCLUDE_DIR}" "${MYSQL_INCLUDE_DIR}" "${MUDUO_INCLUDE_DIR}"
    "${AMQPCPP_INCLUDE_DIR}" "${ONNXRUNTIME_INCLUDE_DIR}" "${NLOHMANN_JSON_INCLUDE_DIR}"
    "${GEMMA_INCLUDE_DIR}" "${HIGHWAY_INCLUDE_DIR}" "${SENTENCEPIECE_INCLUDE_DIR}"
    ${OpenCV_INCLUDE_DIRS}
)
target_link_libraries(http_server PRIVATE
    "${MUDUO_NET_LIBRARY}" "${MUDUO_BASE_LIBRARY}"
    "${MYSQLCPPCONN_LIBRARY}" "${MYSQLCLIENT_LIBRARY}"
    "${AMQPCPP_LIBRARY}" "${ONNXRUNTIME_LIBRARY}"
    gemma_external ${OpenCV_LIBS}
    CURL::libcurl OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS}
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # SentencePiece bundles protobuf-lite; OpenCV DNN loads the system protobuf.
    # Keep the archive's symbols private so its different ABI cannot interpose
    # on the shared system runtime during initialization, before main().
    get_filename_component(SENTENCEPIECE_ARCHIVE_NAME "${SENTENCEPIECE_LIBRARY}" NAME)
    target_link_options(http_server PRIVATE
        "LINKER:--exclude-libs,${SENTENCEPIECE_ARCHIVE_NAME}")
endif()
message(STATUS "AMQP-CPP library: ${AMQPCPP_LIBRARY}")
message(STATUS "ONNX Runtime library: ${ONNXRUNTIME_LIBRARY}")
message(STATUS "Gemma library: ${GEMMA_LIBRARY}")
