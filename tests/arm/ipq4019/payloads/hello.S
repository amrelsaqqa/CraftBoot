	.syntax unified
	.arm
	.text
	.global _start
_start:
	ldr	r0, =0x078af000
	ldr	r1, =0x0a494251	@ "\nQBI"
	@ write each byte through TF
	mov	r2, #0x49
	str	r2, [r0, #0x70]
	mov	r2, #0x50
	str	r2, [r0, #0x70]
	mov	r2, #0x51
	str	r2, [r0, #0x70]
	ldr	r2, =0x0a2e4b4f	@ "OK.\n"
	str	r2, [r0, #0x70]
	str	r2, [r0, #0x74]
	str	r2, [r0, #0x78]
	str	r2, [r0, #0x7c]
	ldr	r0, =0x80000000
	ldr	r1, =0xdeadbeef
	str	r1, [r0]
	ldr	r2, [r0]
	cmp	r1, r2
	bne	1f
	mov	r2, #0x52	@ 'R'
	str	r2, [r0, #0x70]	@ r0 is 0x80000000 now: wrong, reload
1:	ldr	r0, =0x078af000
	mov	r2, #0x21	@ '!'
	str	r2, [r0, #0x70]
	mov	r2, #0x0a
	str	r2, [r0, #0x70]
2:	b	2b
	.ltorg
