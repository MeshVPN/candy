#ifndef CANDY_NETSTACK_LWIP_PORT_ARCH_CC_H
#define CANDY_NETSTACK_LWIP_PORT_ARCH_CC_H

#if defined(_WIN32) || defined(_WIN64)
#else
#if defined __ANDROID__
#define LWIP_UNIX_ANDROID
#elif defined __linux__
#define LWIP_UNIX_LINUX
#elif defined __APPLE__
#define LWIP_UNIX_MACH
#endif
#endif

#define LWIP_TIMEVAL_PRIVATE 0
#include <sys/time.h>

#define LWIP_ERRNO_INCLUDE <errno.h>

#if defined(LWIP_UNIX_LINUX) || defined(LWIP_UNIX_ANDROID)
#define LWIP_ERRNO_STDINCLUDE 1
#endif

extern unsigned int lwip_port_rand(void);
#define LWIP_RAND() (lwip_port_rand())

#if defined(LWIP_UNIX_MACH)
#include <sys/types.h>
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS
#endif

typedef unsigned int sys_prot_t;

#endif
