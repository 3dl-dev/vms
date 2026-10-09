/*
 * test_imgact_eihd_images.S -- the real OpenVMS Alpha images
 * (tests/native-images/alpha/, LINKed on the lab Alpha V8.4 node from our own
 * MACRO-32 sources) embedded byte for byte into test_imgact_eihd (vms-3b3f).
 * The include directory is passed as -Wa,-I<dir> by tests/qemu/CMakeLists.txt.
 */
	.section .rodata
	.balign 16
	.globl eihd_img_main3, eihd_img_main3_end
eihd_img_main3:
	.incbin "MAIN3.EXE"
eihd_img_main3_end:
	.balign 16
	.globl eihd_img_myshr, eihd_img_myshr_end
eihd_img_myshr:
	.incbin "MYSHR.EXE"
eihd_img_myshr_end:
	.balign 16
	.globl eihd_img_hello, eihd_img_hello_end
eihd_img_hello:
	.incbin "HELLO.EXE"
eihd_img_hello_end:
	.balign 16
	.globl eihd_img_text, eihd_img_text_end
eihd_img_text:
	.incbin "HELLO.MAR"
eihd_img_text_end:
	.balign 16
	.globl eihd_img_main4, eihd_img_main4_end
eihd_img_main4:
	.incbin "MAIN4.EXE"
eihd_img_main4_end:
	.balign 16
	.globl eihd_img_myshrv2, eihd_img_myshrv2_end
eihd_img_myshrv2:
	.incbin "MYSHRV2.EXE"
eihd_img_myshrv2_end:
	.section .note.GNU-stack,"",%progbits
