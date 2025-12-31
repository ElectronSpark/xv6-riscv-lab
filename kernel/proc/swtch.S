# Context switch
#
#   void swtch(struct context *old, struct context *new);
# 
# Save current registers in old. Load from new.	

# uint64 __swtch_context(struct context *current, struct context *target, uint64 pcb_arg);
# 
# Save current registers in old. Load from new.
# The pcb_arg passed in a2 is returned to the target context in a0.
.globl __swtch_context
__swtch_context:
        sd ra, 0(a0)
        sd sp, 8(a0)
        sd s0, 16(a0)
        sd s1, 24(a0)
        sd s2, 32(a0)
        sd s3, 40(a0)
        sd s4, 48(a0)
        sd s5, 56(a0)
        sd s6, 64(a0)
        sd s7, 72(a0)
        sd s8, 80(a0)
        sd s9, 88(a0)
        sd s10, 96(a0)
        sd s11, 104(a0)

        ld ra, 0(a1)
        ld sp, 8(a1)
        ld s0, 16(a1)
        ld s1, 24(a1)
        ld s2, 32(a1)
        ld s3, 40(a1)
        ld s4, 48(a1)
        ld s5, 56(a1)
        ld s6, 64(a1)
        ld s7, 72(a1)
        ld s8, 80(a1)
        ld s9, 88(a1)
        ld s10, 96(a1)
        ld s11, 104(a1)

        addi a0, a2, 0
        
        ret

        #
        # Jump to an address and change the sp pointer without return
        # void __switch_noreturn(uint64 sp, uint64 s0, sw_noret_cb_t addr);
        # the addr should point to a function with this prototype:
        # typedef void (*sw_noret_cb_t)(uint64 ksp, uint64 s0);
.globl __switch_noreturn
.align 4
__switch_noreturn:
        # fake return point in convenient of debugging
        # ld        ra, 8(sp)
        # ld        s0, 0(sp)
        # addi      sp, a0, -16
        # sd        ra, 8(sp)
        # sd        s0, 0(sp)
        # mv        t0, s0
        # mv        s0, a0
        # mv        a0, t0
        mv        t0, sp
        mv        sp, a0
        mv        a0, t0
        jr        a2
        # should not return