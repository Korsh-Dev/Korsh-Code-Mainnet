# Embedded Sapling parameters for macOS (Mach-O).
#
# Same trick as embedded_params_linux.S, but with Mach-O syntax: the leading
# underscore on the symbols is what a C declaration expects on Darwin. The file
# expands to nothing on every other platform, so it can be listed
# unconditionally in the Makefile.
#if defined(__APPLE__)
    .section __DATA,__const
    .p2align 4

    .globl _korsh_sapling_spend_params_start
    .private_extern _korsh_sapling_spend_params_start
_korsh_sapling_spend_params_start:
    .incbin "../params/sapling-spend.params"
    .globl _korsh_sapling_spend_params_end
    .private_extern _korsh_sapling_spend_params_end
_korsh_sapling_spend_params_end:

    .p2align 4
    .globl _korsh_sapling_output_params_start
    .private_extern _korsh_sapling_output_params_start
_korsh_sapling_output_params_start:
    .incbin "../params/sapling-output.params"
    .globl _korsh_sapling_output_params_end
    .private_extern _korsh_sapling_output_params_end
_korsh_sapling_output_params_end:
#endif
