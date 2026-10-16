#ifndef _ASM_LKL_SETUP_H
#define _ASM_LKL_SETUP_H

#define COMMAND_LINE_SIZE 4096

#ifndef __ASSEMBLY__
/* wasm: the sections wasm-first.o orders are in place (else a panic) */
void wasm_layout_check(void);
#endif

#endif
