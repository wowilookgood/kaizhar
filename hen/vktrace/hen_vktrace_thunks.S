// Trampolines for hen_vktrace.c: thunk i loads its index and enters the common
// code, which saves the argument registers, calls hen_vktrace_enter(i) (logs
// the call, returns the real function) and jumps to the real function with
// the arguments restored. x16/x17 are the procedure-call scratch registers.
	.text
	.align 3
	.global hen_vktrace_thunks
	.type hen_vktrace_thunks, %function
hen_vktrace_thunks:
	.set i, 0
	.rept 2048
	mov	w16, #i
	b	hen_vktrace_common
	.set i, i + 1
	.endr

	.type hen_vktrace_common, %function
hen_vktrace_common:
	stp	x29, x30, [sp, #-240]!
	mov	x29, sp
	stp	x0, x1, [sp, #16]
	stp	x2, x3, [sp, #32]
	stp	x4, x5, [sp, #48]
	stp	x6, x7, [sp, #64]
	str	x8, [sp, #80]
	stp	q0, q1, [sp, #96]
	stp	q2, q3, [sp, #128]
	stp	q4, q5, [sp, #160]
	stp	q6, q7, [sp, #192]
	mov	w0, w16
	bl	hen_vktrace_enter
	mov	x17, x0
	ldp	x0, x1, [sp, #16]
	ldp	x2, x3, [sp, #32]
	ldp	x4, x5, [sp, #48]
	ldp	x6, x7, [sp, #64]
	ldr	x8, [sp, #80]
	ldp	q0, q1, [sp, #96]
	ldp	q2, q3, [sp, #128]
	ldp	q4, q5, [sp, #160]
	ldp	q6, q7, [sp, #192]
	ldp	x29, x30, [sp], #240
	br	x17
