# Process entry for freestanding qemu-user runs (ppc64le): call test_main, exit.
	.abiversion 2
	.text
	.globl _start
_start:
	bl 1f
1:	mflr 12
	addis 2,12,(.TOC.-1b)@ha
	addi 2,2,(.TOC.-1b)@l
	li 0,0
	stdu 1,-128(1)
	bl test_main
	nop
	li 0,1
	sc
