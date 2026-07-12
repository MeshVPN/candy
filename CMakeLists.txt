cmake_minimum_required(VERSION 3.16)
project(Candy VERSION 6.1.8)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(CANDY_VERSION="${PROJECT_VERSION}")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_SKIP_BUILD_RPATH TRUE)

if (${CANDY_STATIC})
    set(CANDY_STATIC_OPENSSL 1)
    set(CANDY_STATIC_POCO 1)
endif()

find_package(PkgConfig REQUIRED)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Fetch.cmake)

if (${CANDY_STATIC_OPENSSL})
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/openssl
    )
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DTARGET_OPENSSL=${TARGET_OPENSSL} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/openssl
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/openssl
        RESULT_VARIABLE result
    )
    if(NOT result EQUAL "0")
        message(FATAL_ERROR "OpenSSL CMake failed")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_CURRENT_BINARY_DIR}/openssl
        RESULT_VARIABLE result
    )
    if(NOT result EQUAL "0")
        message(FATAL_ERROR "OpenSSL Download or Configure failed")
    endif()

    include(ProcessorCount)
    ProcessorCount(nproc)
    if(nproc EQUAL 0)
        set(nproc 1)
    endif()
    set(OPENSSL_ROOT_DIR ${CMAKE_CURRENT_BINARY_DIR}/openssl/openssl/src/openssl)
    execute_process(
        COMMAND make -j${nproc}
        WORKING_DIRECTORY ${OPENSSL_ROOT_DIR}
        RESULT_VARIABLE result
    )
    if(NOT result EQUAL "0")
        message(FATAL_ERROR "OpenSSL Build failed")
    endif()

    set(OPENSSL_ROOT_DIR ${CMAKE_CURRENT_BINARY_DIR}/openssl/openssl/src/openssl)
    set(OPENSSL_INCLUDE ${OPENSSL_ROOT_DIR}/include)
    set(OPENSSL_LIB_CRYPTO ${OPENSSL_ROOT_DIR}/libcrypto.a)
    set(OPENSSL_LIB_SSL ${OPENSSL_ROOT_DIR}/libssl.a)
    include_directories(${OPENSSL_INCLUDE})
else()
    find_package(OpenSSL REQUIRED)
endif()

if (${CANDY_STATIC_POCO})
    set(ENABLE_DATA OFF CACHE BOOL "" FORCE)
    set(ENABLE_DATA_MYSQL OFF CACHE BOOL "" FORCE)
    set(ENABLE_DATA_POSTGRESQL OFF CACHE BOOL "" FORCE)
    set(ENABLE_DATA_SQLITE OFF CACHE BOOL "" FORCE)
    set(ENABLE_DATA_ODBC OFF CACHE BOOL "" FORCE)
    set(ENABLE_MONGODB OFF CACHE BOOL "" FORCE)
    set(ENABLE_REDIS OFF CACHE BOOL "" FORCE)
    set(ENABLE_ENCODINGS OFF CACHE BOOL "" FORCE)
    set(ENABLE_PROMETHEUS OFF CACHE BOOL "" FORCE)
    set(ENABLE_PAGECOMPILER OFF CACHE BOOL "" FORCE)
    set(ENABLE_PAGECOMPILER_FILE2PAGE OFF CACHE BOOL "" FORCE)
    set(ENABLE_ACTIVERECORD OFF CACHE BOOL "" FORCE)
    set(ENABLE_ACTIVERECORD_COMPILER OFF CACHE BOOL "" FORCE)
    set(ENABLE_ZIP OFF CACHE BOOL "" FORCE)
    set(ENABLE_JWT OFF CACHE BOOL "" FORCE)
    Fetch(poco "https://github.com/pocoproject/poco.git" "poco-1.14.2-release")
else()
    find_package(Poco REQUIRED COMPONENTS Foundation XML JSON Net NetSSL Util)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# 用户态协议栈（netstack + lwIP）编译开关。默认关闭：不编译任何 netstack 代码、
# 不拉取 lwIP。线上发行版打包（部分要求构建时无网络）保持关闭即可零影响。
# GitHub Release 的纯静态二进制可用 -DCANDY_NETSTACK=ON 开启此功能。
option(CANDY_NETSTACK "Build the embedded userspace network stack (lwIP)" OFF)
if (${CANDY_NETSTACK})
    add_compile_definitions(CANDY_NETSTACK)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/lwip.cmake)
endif()

add_subdirectory(candy)
add_subdirectory(candy-cli)
add_subdirectory(candy-service)

# C ABI 动态库：供 Flutter/dart:ffi 及移动端 FFI 加载（默认关闭，桌面/客户端 SDK 打开）。
# 直接编译 capi.cc 并链接静态核心库，保证 candy_* 导出符号一定进入动态库符号表。
option(CANDY_SHARED "Build the C ABI shared library (libcandy_ffi) for FFI bindings" OFF)
if (${CANDY_SHARED})
    add_library(candy_shared SHARED candy/src/candy/capi.cc)
    target_compile_definitions(candy_shared PRIVATE CANDY_BUILD_DLL)
    # capi.cc 经 client.h 间接包含 Poco/JSON 头，而 Poco 是核心库的 PRIVATE 依赖不会
    # 传递，故显式补 Foundation+JSON 的 include；核心逻辑仍由 Candy::Library 提供。
    target_link_libraries(candy_shared PRIVATE Candy::Library Poco::Foundation Poco::JSON)
    set_target_properties(candy_shared PROPERTIES OUTPUT_NAME "candy_ffi")
    # 链入动态库的静态库都必须是位置无关代码。macOS 默认即 PIC 可容错，但 Linux
    # （尤其 aarch64）会因 lwIP 非 PIC 而报 relocation 错误，故显式对相关静态库开启。
    set_property(TARGET candy-library PROPERTY POSITION_INDEPENDENT_CODE ON)
    if (TARGET lwip)
        set_property(TARGET lwip PROPERTY POSITION_INDEPENDENT_CODE ON)
    endif()
endif()
