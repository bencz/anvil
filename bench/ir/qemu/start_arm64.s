# Process entry for freestanding qemu-user runs (arm64): call test_main, exit.
	.text
	.globl _start
_start:
	bl test_main
	mov x8, #93
	svc #0
