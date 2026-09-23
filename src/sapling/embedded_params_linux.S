#if defined(__linux__)
    .section .rodata.korsh_sapling_params,"a",@progbits
    .balign 16

    .globl korsh_sapling_spend_params_start
    .hidden korsh_sapling_spend_params_start
    .type korsh_sapling_spend_params_start, @object
korsh_sapling_spend_params_start:
    .incbin "../params/sapling-spend.params"
    .globl korsh_sapling_spend_params_end
    .hidden korsh_sapling_spend_params_end
    .type korsh_sapling_spend_params_end, @object
korsh_sapling_spend_params_end:
    .size korsh_sapling_spend_params_start, korsh_sapling_spend_params_end - korsh_sapling_spend_params_start

    .balign 16
    .globl korsh_sapling_output_params_start
    .hidden korsh_sapling_output_params_start
    .type korsh_sapling_output_params_start, @object
korsh_sapling_output_params_start:
    .incbin "../params/sapling-output.params"
    .globl korsh_sapling_output_params_end
    .hidden korsh_sapling_output_params_end
    .type korsh_sapling_output_params_end, @object
korsh_sapling_output_params_end:
    .size korsh_sapling_output_params_start, korsh_sapling_output_params_end - korsh_sapling_output_params_start

    .section .note.GNU-stack,"",@progbits
#endif
