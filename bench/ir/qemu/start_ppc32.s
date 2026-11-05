# Process entry for freestanding qemu-user runs (ppc32): call test_main, exit.
	.text
	.globl _start
_start:
	li 0,0
	stwu 1,-64(1)
	bl test_main
	li 0,1
	sc
