/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_SYSCALL_WRAPPER_H
#define _ASM_LKL_SYSCALL_WRAPPER_H

/*
 * Uniform syscall entry points for hosts where calling a function through a
 * pointer of a different type traps (wasm call_indirect): every syscall gets
 *
 *	long __lkl_sys_<name>(const long *args);
 *
 * which unpacks the argument array lkl_syscall() was given. On 32-bit, a
 * 64-bit argument occupies two consecutive slots, low word first.
 */

asmlinkage long sys_ni_syscall(void);

#define __LKL_SC_IS_SPLIT(t)	(__BITS_PER_LONG == 32 && __TYPE_IS_LL(t))

#define __LKL_SC_LOAD(t, a)						\
	__typeof(__builtin_choose_expr(__TYPE_IS_LL(t), 0LL, 0L)) a =	\
		__builtin_choose_expr(__LKL_SC_IS_SPLIT(t),		\
			(long long)((unsigned long long)(unsigned long)__args[0] | \
				    ((unsigned long long)(unsigned long)__args[1] << 32)), \
			__args[0]);					\
	__args += __LKL_SC_IS_SPLIT(t) ? 2 : 1;

/* like __MAP, but emits statements instead of a comma-separated list */
#define __LKL_MAP1(m, t, a, ...) m(t, a)
#define __LKL_MAP2(m, t, a, ...) m(t, a) __LKL_MAP1(m, __VA_ARGS__)
#define __LKL_MAP3(m, t, a, ...) m(t, a) __LKL_MAP2(m, __VA_ARGS__)
#define __LKL_MAP4(m, t, a, ...) m(t, a) __LKL_MAP3(m, __VA_ARGS__)
#define __LKL_MAP5(m, t, a, ...) m(t, a) __LKL_MAP4(m, __VA_ARGS__)
#define __LKL_MAP6(m, t, a, ...) m(t, a) __LKL_MAP5(m, __VA_ARGS__)
#define __LKL_MAP(n, ...) __LKL_MAP##n(__VA_ARGS__)

#define __SYSCALL_DEFINEx(x, name, ...)					\
	static long __se_sys##name(__MAP(x, __SC_LONG, __VA_ARGS__));	\
	static inline long __do_sys##name(__MAP(x, __SC_DECL, __VA_ARGS__)); \
	long __lkl_sys##name(const long *__args);			\
	long __lkl_sys##name(const long *__args)			\
	{								\
		__LKL_MAP(x, __LKL_SC_LOAD, __VA_ARGS__)		\
		return __se_sys##name(__MAP(x, __SC_ARGS, __VA_ARGS__)); \
	}								\
	static long __se_sys##name(__MAP(x, __SC_LONG, __VA_ARGS__))	\
	{								\
		long ret = __do_sys##name(__MAP(x, __SC_CAST, __VA_ARGS__)); \
		__MAP(x, __SC_TEST, __VA_ARGS__);			\
		__PROTECT(x, ret, __MAP(x, __SC_ARGS, __VA_ARGS__));	\
		return ret;						\
	}								\
	static inline long __do_sys##name(__MAP(x, __SC_DECL, __VA_ARGS__))

#define SYSCALL_DEFINE0(sname)						\
	SYSCALL_METADATA(_##sname, 0);					\
	long __lkl_sys_##sname(const long *__unused);			\
	long __lkl_sys_##sname(const long *__unused)

#define COND_SYSCALL(name)						\
	long __lkl_sys_##name(const long *__unused);			\
	__weak long __lkl_sys_##name(const long *__unused)		\
	{								\
		return sys_ni_syscall();				\
	}

#define COND_SYSCALL_COMPAT(name)

#endif /* _ASM_LKL_SYSCALL_WRAPPER_H */
