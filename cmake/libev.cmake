# libev：事件循环库（reactor 后端，一份代码全平台）。
# 依赖自定义 fork（Bepartofyou/libev），通过 FetchContent 拉取，不再在仓库内 vendored 源码，
# 也不依赖系统库，保证三平台开箱即用。
#
# libev 的源码组织很特殊：只需编译 ev.c 一个翻译单元，它在内部按宏条件
# #include 具体后端（ev_epoll.c / ev_kqueue.c / ev_select.c 等）。因此这里
# 只把 ev.c 加入编译，并通过 EV_STANDALONE=1 跳过 autotools 的 config.h 探测，
# 改由我们按平台显式指定后端宏。
#
# 重要陷阱（务必保留这些显式宏）：
#   1. EV_STANDALONE 下，epoll 的自动探测条件是 __linux && __GLIBC__，
#      而 Alpine/musl 下 __GLIBC__ 未定义，会退化到 select。故 Linux 必须显式 EV_USE_EPOLL=1。
#   2. EV_STANDALONE 下 kqueue 默认关闭（EV_USE_KQUEUE 默认 0），故 macOS 必须显式 EV_USE_KQUEUE=1。
#   3. Windows 用 select 后端，并需 EV_SELECT_IS_WINSOCKET=1 让 select 操作 SOCKET 句柄。

include(FetchContent)

set(LIBEV_GIT_REPOSITORY "https://github.com/Bepartofyou/libev.git")
set(LIBEV_GIT_TAG "a56570ae6d076d7471b945116171c88724ba14a9")

FetchContent_Declare(
    libev
    GIT_REPOSITORY ${LIBEV_GIT_REPOSITORY}
    GIT_TAG        ${LIBEV_GIT_TAG}
)
FetchContent_GetProperties(libev)
if(NOT libev_POPULATED)
    FetchContent_Populate(libev)
endif()

set(LIBEV_DIR ${libev_SOURCE_DIR})

add_library(ev STATIC ${LIBEV_DIR}/ev.c)

# 仅暴露头文件目录；ev_*.c 由 ev.c 内部 include，不单独编译。
target_include_directories(ev PUBLIC ${LIBEV_DIR})

# 公共：standalone（不读 config.h），关闭用不到的特性以缩小跨平台风险面。
# reactor 仅用到 ev_io / ev_async / ev_run / ev_break，无需 timer/periodic/signal/child/stat/idle/fork/embed。
target_compile_definitions(ev PUBLIC
    EV_STANDALONE=1
    EV_USE_FLOOR=0            # 不依赖 libm 的 floor，使用 libev 内置近似实现
    EV_PERIODIC_ENABLE=0
    EV_STAT_ENABLE=0
    EV_IDLE_ENABLE=0
    EV_FORK_ENABLE=0
    EV_SIGNAL_ENABLE=0
    EV_CHILD_ENABLE=0
    EV_EMBED_ENABLE=0
    EV_ASYNC_ENABLE=1         # 跨线程唤醒（post）依赖 ev_async
)

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux" OR ${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    # Linux/Android：epoll 后端。musl 下 __GLIBC__ 未定义，必须显式开启 epoll，
    # 否则退化到 select。eventfd 用于 ev_async 唤醒；clock 用 libc 的 clock_gettime
    # （musl 自带），关闭 glibc 专属的 clock syscall 路径以兼容 musl。
    target_compile_definitions(ev PRIVATE
        EV_USE_EPOLL=1
        EV_USE_SELECT=0
        EV_USE_POLL=0
        EV_USE_EVENTFD=1
        EV_USE_CLOCK_SYSCALL=0
        EV_USE_MONOTONIC=1
        EV_USE_REALTIME=1
        EV_USE_NANOSLEEP=1
    )
elseif (${CMAKE_SYSTEM_NAME} STREQUAL "Darwin" OR ${CMAKE_SYSTEM_NAME} STREQUAL "iOS")
    # macOS/iOS：kqueue 后端（standalone 下默认关闭，必须显式开启）。
    target_compile_definitions(ev PRIVATE
        EV_USE_KQUEUE=1
        EV_USE_EPOLL=0
        EV_USE_POLL=0
        EV_USE_SELECT=0
        EV_USE_MONOTONIC=1
        EV_USE_REALTIME=1
        EV_USE_NANOSLEEP=1
    )
elseif (${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    # Windows：select 后端，select 操作 winsock SOCKET 句柄。
    target_compile_definitions(ev PRIVATE
        EV_USE_SELECT=1
        EV_SELECT_IS_WINSOCKET=1
        EV_USE_EPOLL=0
        EV_USE_KQUEUE=0
        EV_USE_POLL=0
        EV_USE_MONOTONIC=1
        EV_USE_REALTIME=1
    )
    target_link_libraries(ev PUBLIC ws2_32)
endif()

add_library(Candy::ev ALIAS ev)
