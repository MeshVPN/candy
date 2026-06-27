include(FetchContent)

set(LWIP_GIT_REPOSITORY "https://github.com/heiher/lwip.git")
set(LWIP_GIT_TAG "8c69dfbe537835d5f2a5fd8c08c859f667b108ea")

FetchContent_Declare(
    lwip
    GIT_REPOSITORY ${LWIP_GIT_REPOSITORY}
    GIT_TAG        ${LWIP_GIT_TAG}
)
FetchContent_GetProperties(lwip)
if(NOT lwip_POPULATED)
    FetchContent_Populate(lwip)
endif()

# 补丁：heiher lwIP 的 UDP PRETEND 克隆路径用了 pcb->local_ip.type，
# 该字段仅在双栈(LWIP_IPV6=1)的 ip_addr_t 存在；本项目是 IPv4-only，
# 编译会报 "ip_addr_t has no member named type"。改用版本无关的
# IP_GET_TYPE() 宏（IPv4-only 下展开为 IPADDR_TYPE_V4），不引入 IPv6。
set(LWIP_UDP_C "${lwip_SOURCE_DIR}/src/core/udp.c")
file(READ "${LWIP_UDP_C}" _lwip_udp_src)
string(REPLACE
    "udp_new_ip_type(pcb->local_ip.type)"
    "udp_new_ip_type(IP_GET_TYPE(&pcb->local_ip))"
    _lwip_udp_src "${_lwip_udp_src}")
file(WRITE "${LWIP_UDP_C}" "${_lwip_udp_src}")

set(LWIP_SRC_DIR ${lwip_SOURCE_DIR}/src)
set(LWIP_PORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/candy/src/netstack/lwip-port)
set(LWIP_OPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/candy/src/netstack)

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
    ${LWIP_PORT_DIR}/sys_arch.c
)

add_library(lwip STATIC
    ${LWIP_CORE_SOURCES}
    ${LWIP_CORE_IPV4_SOURCES}
    ${LWIP_PORT_SOURCES}
)

target_include_directories(lwip PUBLIC
    ${LWIP_OPTS_DIR}
    ${LWIP_PORT_DIR}
    ${LWIP_SRC_DIR}/include
)
