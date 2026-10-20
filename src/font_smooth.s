.section .rodata
.align 16
.global font_smooth_atlas
font_smooth_atlas:
.incbin "src/font_smooth.bin"
.section .note.GNU-stack,"",@progbits
