# Process entry for freestanding qemu-user runs (ppc64): call test_main, exit.
	.section .opd,"aw"
	.align 3
	.globl _start
_start:
	.quad ._start, .TOC.@tocbase, 0
	.text
._start:
	li 0,0
	stdu 1,-128(1)
	bl test_main
	nop
	li 0,1
	sc
