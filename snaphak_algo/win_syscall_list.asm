

_text SEGMENT

PUBLIC perform_syscall_impl

ALIGN 8
doswi PROC
	mov r10, rcx
	syscall
	ret

	
doswi endp
ALIGN 16
perform_syscall_impl PROC 
	Counter = 0

	REPEAT 462
	ALIGN 16
	mov eax, Counter
	jmp doswi
	Counter = Counter + 1
	ENDM
perform_syscall_impl endp

_text ENDS
END
