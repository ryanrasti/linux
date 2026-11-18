#include <uapi/asm/unistd.h>

__SYSCALL(__NR_virtio_mmio_device_add, sys_virtio_mmio_device_add)
__SYSCALL(__NR_new_thread_group_leader, sys_new_thread_group_leader)
__SYSCALL(__NR_new_thread, sys_new_thread)
__SYSCALL(__NR_attach_thread, sys_attach_thread)
__SYSCALL(__NR_host_fd, sys_host_fd)
__SYSCALL(__NR_new_exec, sys_new_exec)
#ifdef CONFIG_BLOCK
__SYSCALL(__NR_memblk_add, sys_memblk_add)
#endif

#define __SC_ASCII(t, a) #t "," #a

#define __ASCII_MAP0(m,...)
#define __ASCII_MAP1(m,t,a) m(t,a)
#define __ASCII_MAP2(m,t,a,...) m(t,a) "," __ASCII_MAP1(m,__VA_ARGS__)
#define __ASCII_MAP3(m,t,a,...) m(t,a) "," __ASCII_MAP2(m,__VA_ARGS__)
#define __ASCII_MAP4(m,t,a,...) m(t,a) "," __ASCII_MAP3(m,__VA_ARGS__)
#define __ASCII_MAP5(m,t,a,...) m(t,a) "," __ASCII_MAP4(m,__VA_ARGS__)
#define __ASCII_MAP6(m,t,a,...) m(t,a) "," __ASCII_MAP5(m,__VA_ARGS__)
#define __ASCII_MAP(n,...) __ASCII_MAP##n(__VA_ARGS__)

#ifdef __MINGW32__
#define SECTION_ATTRS "n0"
#else
#define SECTION_ATTRS "a"
#endif

#define __SYSCALL_DEFINE_ARCH(x, name, ...)				\
	asm(".section .syscall_defs,\"" SECTION_ATTRS "\"\n"		\
	    ".ascii \"#ifdef __NR" #name "\\n\"\n"			\
	    ".ascii \"SYSCALL_DEFINE" #x "(" #name ","			\
	    __ASCII_MAP(x, __SC_ASCII, __VA_ARGS__) ")\\n\"\n"		\
	    ".ascii \"#endif\\n\"\n"					\
	    ".section .text\n");
