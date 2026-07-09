include(FetchContent)

set(LWIP_GIT_REPOSITORY "https://github.com/Bepartofyou/lwip.git")
set(LWIP_GIT_TAG "9b6fe14f0a44c477e10bf0e68da56b3498a91352")

FetchContent_Declare(
    lwip
    GIT_REPOSITORY ${LWIP_GIT_REPOSITORY}
    GIT_TAG        ${LWIP_GIT_TAG}
)
FetchContent_GetProperties(lwip)
if(NOT lwip_POPULATED)
    FetchContent_Populate(lwip)
endif()

set(LWIP_SRC_DIR ${lwip_SOURCE_DIR}/src)
# 直接复用 fork（Bepartofyou/lwip）自带的移植层与配置：
# - src/ports/unix|win32/lib/sys_arch.c：平台适配，不再手写。
# - src/ports/include/arch/*.h：转发头，按平台自动转发到 unix/win32 实现头。
# - src/ports/include/lwipopts.h：已在 fork 内改成 candy 定制配置，
#   candy 不再单独维护 lwipopts.h。
# 另：udp.c 的 IPv4-only 兼容修复（IP_GET_TYPE）也已合入 fork，无需再打补丁。
set(LWIP_PORT_INCLUDE_DIR ${LWIP_SRC_DIR}/ports/include)
if(WIN32)
    set(LWIP_PORT_DIR ${LWIP_SRC_DIR}/ports/win32)
else()
    set(LWIP_PORT_DIR ${LWIP_SRC_DIR}/ports/unix)
endif()

set(LWIP_CORE_SOURCES
    ${LWIP_SRC_DIR}/core/init.c
    ${LWIP_SRC_DIR}/core/def.c
    ${LWIP_SRC_DIR}/core/inet_chksum.c
    ${LWIP_SRC_DIR}/core/ip.c
    ${LWIP_SRC_DIR}/core/mem.c
    ${LWIP_SRC_DIR}/core/memp.c
    ${LWIP_SRC_DIR}/core/netif.c
    ${LWIP_SRC_DIR}/core/pbuf.c
    ${LWIP_SRC_DIR}/core/raw.c
    ${LWIP_SRC_DIR}/core/stats.c
    ${LWIP_SRC_DIR}/core/sys.c
    ${LWIP_SRC_DIR}/core/tcp.c
    ${LWIP_SRC_DIR}/core/tcp_in.c
    ${LWIP_SRC_DIR}/core/tcp_out.c
    ${LWIP_SRC_DIR}/core/udp.c
    ${LWIP_SRC_DIR}/core/timeouts.c
)

set(LWIP_CORE_IPV4_SOURCES
    ${LWIP_SRC_DIR}/core/ipv4/icmp.c
    ${LWIP_SRC_DIR}/core/ipv4/ip4.c
    ${LWIP_SRC_DIR}/core/ipv4/ip4_addr.c
    ${LWIP_SRC_DIR}/core/ipv4/ip4_frag.c
)

set(LWIP_PORT_SOURCES
    ${LWIP_PORT_DIR}/lib/sys_arch.c
)

add_library(lwip STATIC
    ${LWIP_CORE_SOURCES}
    ${LWIP_CORE_IPV4_SOURCES}
    ${LWIP_PORT_SOURCES}
)

target_include_directories(lwip PUBLIC
    ${LWIP_PORT_INCLUDE_DIR}
    ${LWIP_PORT_DIR}/include
    ${LWIP_SRC_DIR}/include
)
