	.syntax unified
	.arm
	.text
	.global _start
_start:
	ldr	r0, =0x078af000
	mrc	p15, 0, r3, c9, c13, 0	@ PMCCNTR
	mov	r6, #0
1:	mrc	p15, 0, r4, c9, c13, 0
	sub	r5, r4, r3
	ldr	r7, =200000
	cmp	r5, r7
	blt	1b
	mov	r2, #0x54		@ 'T'
	str	r2, [r0, #0x70]
	mov	r2, #0x0a
	str	r2, [r0, #0x70]
2:	b	2b
	.ltorg
