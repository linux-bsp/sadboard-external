/* All defective operations stay within allocated objects or a guarded probe. */
#if defined(__aarch64__)
.text
.p2align 2

.macro function name
.global \name
.type \name, %function
\name:
.endm
.macro end_function name
.size \name, .-\name
.endm

function sb_asm_width_bug
    ldr x0, [x0]
    ret
end_function sb_asm_width_bug
function sb_asm_width_fixed
    ldr w0, [x0]
    ret
end_function sb_asm_width_fixed

function sb_asm_sign_bug
    ldrb w0, [x0]
    ret
end_function sb_asm_sign_bug
function sb_asm_sign_fixed
    ldrsb x0, [x0]
    ret
end_function sb_asm_sign_fixed

function sb_asm_pointer_bug
    mov w0, w0
    ret
end_function sb_asm_pointer_bug
function sb_asm_pointer_fixed
    ret
end_function sb_asm_pointer_fixed

function sb_asm_branch_bug
    cmp w0, w1
    cset w0, lt
    ret
end_function sb_asm_branch_bug
function sb_asm_branch_fixed
    cmp w0, w1
    cset w0, lo
    ret
end_function sb_asm_branch_fixed

/* Deliberately clobber a caller-saved register, which is legal for this callee. */
function sb_asm_clobber
    mov x9, #85
    add x0, x0, #1
    ret
end_function sb_asm_clobber
function sb_asm_call_bug
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x9, x0
    bl sb_asm_clobber
    add x0, x0, x9
    ldp x29, x30, [sp], #16
    ret
end_function sb_asm_call_bug
function sb_asm_call_fixed
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x0, [sp, #16]
    bl sb_asm_clobber
    ldr x9, [sp, #16]
    add x0, x0, x9
    ldp x29, x30, [sp], #32
    ret
end_function sb_asm_call_fixed

function sb_asm_layout_bug
    ldur w0, [x0, #1]
    ret
end_function sb_asm_layout_bug
function sb_asm_layout_fixed
    ldr w0, [x0, #4]
    ret
end_function sb_asm_layout_fixed

/* x2 receives the real iteration count; the fifth allocated word is a sentinel. */
.macro sum_function name, condition
function \name
    mov x3, #0
    mov x4, #0
1:
    cmp x3, x1
    b.\condition 2f
    ldr w5, [x0, x3, lsl #2]
    add x4, x4, x5
    add x3, x3, #1
    b 1b
2:
    str x3, [x2]
    mov x0, x4
    ret
end_function \name
.endm
sum_function sb_asm_bounds_bug, hi
sum_function sb_asm_bounds_fixed, hs

function sb_asm_bad_callee
    mov x19, #85
    ret
end_function sb_asm_bad_callee
function sb_asm_good_callee
    str x19, [sp, #-16]!
    mov x19, #85
    ldr x19, [sp], #16
    ret
end_function sb_asm_good_callee
/* Restore the C caller's original X19 even when the inner callee is defective. */
.macro preserve_probe name, callee
function \name
    stp x29, x30, [sp, #-32]!
    mov x29, sp
    str x19, [sp, #16]
    mov x19, #0x1234
    bl \callee
    mov x0, x19
    ldr x19, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
end_function \name
.endm
preserve_probe sb_asm_preserve_bug, sb_asm_bad_callee
preserve_probe sb_asm_preserve_fixed, sb_asm_good_callee

.section .note.GNU-stack,"",%progbits
#else
.section .note.GNU-stack,"",@progbits
#endif
