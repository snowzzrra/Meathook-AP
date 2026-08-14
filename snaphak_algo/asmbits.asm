COMMENT @
    these routines are adapted from compiled versions of the normal snaphakalgo routines, except for the rbnode insert/erase functions
    which are rewrites of the linux rbnode functions that use indexing l/r to massively cut down on code size/branches
    
    these all use custom calling conventions, so calling from c/c++ requires GNU inline asm/register asm variables.
    they are intended for use in instrumentation hooks in the id tech 7 engine, to this end as many registers are preserved as possible
    and stack alignment (required by the msvc x64 abi) is not enforced. 
    
    these functions are also designed with amd cpus in mind (the writer has a ripper 3990x that he intends to run the instrumented build on)
    this means that jrcxz/loop/loope/loopne are fair game (on amd cpus they are single uop fast operations, on intel theyre implemented in microcode)
@

_text SEGMENT
PUBLIC fast_strlen_func_impl_avx2_asm
PUBLIC cs_rbnode_insert_and_color
PUBLIC cs_rbnode_erase_from
PUBLIC cs_rb_last
PUBLIC cs_rb_first
PUBLIC cs_rb_next_or_prev

short_call macro WHERE
   call WHERE
endm

;struct for saved "low" (original pre x64) registers
;we do not include rsp because we try not to mess with it
low_gpregs_t STRUCT
    m_rax dq ?
    m_rcx dq ?
    m_rdx dq ?
    m_rbx dq ?
    m_rsi dq ?
    m_rdi dq ?
    m_rbp dq ?
low_gpregs_t ENDS

;struct for saved 'high' (added in x64) registers
high_gpregs_t struct
    m_r8 dq ?
    m_r9 dq ?
    m_r10 dq ?
    m_r11 dq ?
    m_r12 dq ?
    m_r13 dq ?
    m_r14 dq ?
    m_r15 dq ?
high_gpregs_t ends


rb_node struct 
    rb_parent_color dq ?
    rb_right dq ?
    rb_left dq ?
rb_node ends



;also zeros eax
set_overflow_flag macro
    xor eax, eax
    cmp al, -128
endm
declare_low_gpreg_save macro BASEREG, SAVENAME, RESTNAME
    ; using the same address expression means we make use of zen 2's memory mirroring
    ALIGN 16
    SAVENAME PROC 
        mov [BASEREG+low_gpregs_t.m_rax], rax
        mov [BASEREG+low_gpregs_t.m_rcx], rcx
        mov [BASEREG+low_gpregs_t.m_rdx], rdx
        mov [BASEREG+low_gpregs_t.m_rbx], rbx
        mov [BASEREG+low_gpregs_t.m_rsi], rsi
        mov [BASEREG+low_gpregs_t.m_rdi], rdi
        mov [BASEREG+low_gpregs_t.m_rbp], rbp
        ret
    SAVENAME ENDP
    
    ALIGN 16
    RESTNAME PROC 
        mov rax, [BASEREG+low_gpregs_t.m_rax]
        mov rcx, [BASEREG+low_gpregs_t.m_rcx]
        mov rdx, [BASEREG+low_gpregs_t.m_rdx]
        mov rbx, [BASEREG+low_gpregs_t.m_rbx]
        mov rsi, [BASEREG+low_gpregs_t.m_rsi]
        mov rdi, [BASEREG+low_gpregs_t.m_rdi]
        mov rbp, [BASEREG+low_gpregs_t.m_rbp]
        ret
    RESTNAME ENDP
endm


declare_low_gpreg_save r10, save_low_gpregs_r10, rest_low_gpregs_r10

classify_pointer_simd_alignment proc
    xor eax, eax
    ;test cl, 15
    test rcx, 0Fh
    jnz unaligned
    ;inc ah ;set cf, indicating that its at least 16 byte aligned
    or eax, 0100h
    ;test rcx, 01Fh
    bt rcx, 4
    jc unaligned
    or eax, 04000h
    bt rcx, 5
    ;test rcx, 03Fh
    jc unaligned
    or eax, 08000h
unaligned:
    sahf
    ret
classify_pointer_simd_alignment endp

; does a vzeroupper state transition :/ so this spoils the top half of every ymm
; spoils the entirety of ymm1, ymm2, 
; preserves rcx and rdx
; length is returned in eax
ALIGN 16
fast_strlen_func_impl_avx2_asm PROC     

                call classify_pointer_simd_alignment
                push rcx
                push rdx

                vpxor   xmm0, xmm0, xmm0
                mov     edx, 0FFFFFFE0h
                jz short loop_ymm ;aligned 32 bytes
                jc short loop_xmm ;aligned 16 bytes
                ;if we got here, eax is already zero from classify_pointer_simd_alignment
               ; xor eax, eax
                xchg rcx, rdx
ALIGN 16
simple_strlen_loop:
                movzx ecx, byte ptr [rdx+rax]
                jecxz loc_1800A7448
                inc eax
                movzx ecx, byte ptr [rdx+rax]
                jecxz loc_1800A7448
                inc eax
                jmp short simple_strlen_loop
ALIGN 16
loop_ymm:                         
                vmovdqu ymm1, ymmword ptr [rcx]
                vpcmpeqb ymm2, ymm1, ymm0
                add     rcx, 20h
                add     edx, 20h
                vptest  ymm2, ymm2
                jz      short loop_ymm
                jmp     short loc_1800A7442
ALIGN 16
loop_xmm:
                vmovdqu xmm1, xmmword ptr [rcx]
                vpcmpeqb xmm2, xmm1, xmm0
                add     rcx, 10h
                add     edx, 10h
                vptest  xmm2, xmm2
                jz      short loop_xmm

loc_1800A7442:
                vpmovmskb eax, ymm2
                tzcnt   eax, eax
                add     eax, edx

loc_1800A7448: 
                vzeroupper
                pop rdx
                pop rcx
                ret
fast_strlen_func_impl_avx2_asm endp

;the rbnode functions do not use r11, r10, or r13.
;the rotate_direction func does use r11 and r10 as temporaries, but saves and restores them
;r14 is used as the pointer to the tree root. it is not spoiled by any functions
;allocate r10 for the gp save/restore area


;node = rcx
;root = r14
;spoils r9, rdx, r10, r8, r11, rax

;does not spoil rcx or r14
ALIGN 16
cs_rbnode_rotate_direction proc    
                push    r11
                push    r10
                mov     rdx, r14
                mov     r10d, r8d
                mov     r9, [rcx+rb_node.rb_parent_color]
                xor     r8d, 1
                mov     r11, [rcx+r10*8+rb_node.rb_right]
                and     r9, 0FFFFFFFFFFFFFFF8h
                mov     rax, [r11+r8*8+rb_node.rb_right]
                mov     [rcx+r10*8+rb_node.rb_right], rax
                test    rax, rax
                jz      short loc_180032053

                mov     r10, [r11+r8*8+rb_node.rb_right]
                mov     rax, [r10+rb_node.rb_parent_color]
                and     eax, 1
                or      rax, rcx
                mov     [r10+rb_node.rb_parent_color], rax
loc_180032053:                  
                mov     [r11+r8*8+rb_node.rb_right], rcx
                mov     rax, [r11+rb_node.rb_parent_color]
                and     eax, 1
                or      rax, r9
                mov     [r11+rb_node.rb_parent_color], rax
                test    r9, r9
                jz      short end_rotate


                xor     eax, eax
                cmp     [r9+rb_node.rb_right], rcx
                setnz   al
                lea     rdx, [r9+rax*8+rb_node.rb_right]

end_rotate:              
                ; set input nodes parent
                mov     [rdx], r11
                mov     rax, [rcx]
                and     eax, 1
                or      rax, r11
                mov     [rcx], rax
                pop     r10
                pop     r11
                ret
cs_rbnode_rotate_direction endp

;doesnt use r13 atm
;r14 = rb_root
;rdx = rb_node to erase
ALIGN 16
cs_rbnode_erase_from proc
                push    r15
                ;push    r14
                push    r12
               ; push    rsi
                ;push    rdi
                ;push    rbp
                ;push    rbx
                ;mov     r14, rcx
                call    save_low_gpregs_r10
                
                mov     rcx, [rdx+rb_node.rb_right]
                mov     r15, [rdx+rb_node.rb_left]
                test    r15, r15
                jz      short loc_180032315
                test    rcx, rcx
                jz      short loc_18003232D
find_zero_left:                      
                mov     rdi, rcx
                mov     rcx, [rcx+rb_node.rb_left]
                ;test    rcx, rcx
                ;test rcx, rcx = 3 bytes, jrcxz = 2
                jrcxz  exit_find_zero_left
                jmp     short find_zero_left
exit_find_zero_left:
                mov     rax, [rdx+rb_node.rb_parent_color]
                and     rax, 0FFFFFFFFFFFFFFF8h
                jz      short loc_180032368
                xor     ecx, ecx
                cmp     [rax+rb_node.rb_left], rdx
                setz    cl
                lea     rax, [rax+rcx*8+rb_node.rb_right]
                jmp     short loc_18003236B
loc_180032315: 
                mov     rax, [rdx+rb_node.rb_parent_color]
                mov     r12, rax
                and     r12, 0FFFFFFFFFFFFFFF8h
                test    rcx, rcx
                jz      loc_1800323DB
                mov     r15, rcx
                jmp     short loc_180032337
loc_18003232D: 
                mov     rax, [rdx+rb_node.rb_parent_color]
                mov     r12, rax
                and     r12, 0FFFFFFFFFFFFFFF8h
loc_180032337:
                mov     rcx, [r15+rb_node.rb_parent_color]
                and     ecx, 1
                or      rcx, r12
                mov     [r15+rb_node.rb_parent_color], rcx
                test    r12, r12
                jz      loc_1800323E7
loc_18003234C:
                xor     ecx, ecx
                cmp     [r12+rb_node.rb_left], rdx
                setz    cl
                mov     [r12+rcx*8+rb_node.rb_right], r15
                test    al, 1
                jnz     loc_18003240E
                jmp     loc_18003250E
loc_180032368:  
                mov     rax, r14
loc_18003236B:   
                mov     [rax], rdi
                mov     rax, [rdi+rb_node.rb_parent_color]
                mov     r15, [rdi+rb_node.rb_right]
                mov     r12, rax
                and     r12, 0FFFFFFFFFFFFFFF8h
                cmp     r12, rdx
                jz      short loc_1800323B1
                test    r15, r15
                jz      short loc_180032392
                mov     rcx, [r15+rb_node.rb_parent_color]
                and     ecx, 1
                or      rcx, r12
                mov     [r15+rb_node.rb_parent_color], rcx
loc_180032392:
                mov     [r12+rb_node.rb_left], r15
                mov     rcx, [rdx+rb_node.rb_right]
                mov     [rdi+rb_node.rb_right], rcx
                mov     rcx, [rdx+rb_node.rb_right]
                mov     rbp, [rcx+rb_node.rb_parent_color]
                and     ebp, 1
                or      rbp, rdi
                mov     [rcx+rb_node.rb_parent_color], rbp
                jmp     short loc_1800323B4
loc_1800323B1:
                mov     r12, rdi
loc_1800323B4: 
                mov     rcx, [rdx+rb_node.rb_parent_color]
                mov     [rdi+rb_node.rb_parent_color], rcx
                mov     rcx, [rdx+rb_node.rb_left]
                mov     [rdi+rb_node.rb_left], rcx
                mov     rcx, [rdx+rb_node.rb_left]
                mov     rdx, [rcx+rb_node.rb_parent_color]
                and     edx, 1
                or      rdx, rdi
                mov     [rcx+rb_node.rb_parent_color], rdx
                test    al, 1
                jnz     short loc_18003240E
                jmp     loc_18003250E
loc_1800323DB:
                xor     r15d, r15d
                test    r12, r12
                jnz     loc_18003234C
loc_1800323E7:
                mov     [r14], r15
                xor     r12d, r12d
                test    al, 1
                jnz     short loc_18003240E
                jmp     loc_18003250E
                align 16
loc_180032400: 
                and     byte ptr [rcx+rb_node.rb_parent_color], 0FEh
                mov     r15, r12
                mov     r12, [r12+rb_node.rb_parent_color]
                and     r12, 0FFFFFFFFFFFFFFF8h
loc_18003240E:
                test    r15, r15
                jz      short loc_18003241E
                mov     rax, [r15]
                test    al, 1
                jz      loc_180032507
loc_18003241E:    
                cmp     r15, [r14]
                jz      loc_1800324FF
                mov     rsi, [r12+rb_node.rb_left]
                xor     ebp, ebp
                cmp     rsi, r15
                setz    bpl
                mov     ebx, ebp
                xor     ebx, 1
                mov     rcx, [r12+rbx*8+rb_node.rb_right]
                mov     rax, [rcx+rb_node.rb_parent_color]
                test    al, 1
                jnz     short loc_180032465
                or      rax, 1
                ;mov     rdx, r14
                mov     r8d, ebx
                mov     [rcx+rb_node.rb_parent_color], rax
                and     byte ptr [r12+rb_node.rb_parent_color], 0FEh
                mov     rcx, r12
                call    cs_rbnode_rotate_direction
                mov     rcx, [r12+rbx*8+rb_node.rb_right]
loc_180032465: 
                mov     rax, [rcx+rb_node.rb_left]
                test    rax, rax
                jz      short loc_180032473
                test    byte ptr [rax+rb_node.rb_parent_color], 1
                jz      short loc_180032498
loc_180032473:
                mov     rdx, [rcx+rb_node.rb_right]
                test    rdx, rdx
                jz      short loc_180032400
                test    byte ptr [rdx+rb_node.rb_parent_color], 1
                jnz     loc_180032400
                cmp     rsi, r15
                jz      short loc_1800324CC
                test    rax, rax
                jz      short loc_180032494
                test    byte ptr [rax+rb_node.rb_parent_color], 1
                jz      short loc_1800324CC
loc_180032494: 
                xor     eax, eax
                jmp     short loc_1800324B0
loc_180032498: 
                cmp     rsi, r15
                jnz     short loc_1800324CC
                mov     rdx, [rcx+rb_node.rb_right]
                mov     eax, 1
                test    rdx, rdx
                jz      short loc_1800324B0
                test    byte ptr [rdx+rb_node.rb_parent_color], 1
                jz      short loc_1800324CC
loc_1800324B0:
                mov     rax, [rcx+rax*8+rb_node.rb_right]
                ;mov     rdx, r14
                mov     r8d, ebp
                or      qword ptr [rax+rb_node.rb_parent_color], 1
                and     byte ptr [rcx+rb_node.rb_parent_color], 0FEh
                call    cs_rbnode_rotate_direction
                mov     rcx, [r12+rbx*8+rb_node.rb_right]
loc_1800324CC:
                mov     rax, [r12+rb_node.rb_parent_color]
                mov     rdx, [rcx+rb_node.rb_parent_color]
                mov     r8d, ebx
                and     eax, 1
                and     rdx, 0FFFFFFFFFFFFFFFEh
                or      rdx, rax
                mov     [rcx+rb_node.rb_parent_color], rdx
                or      byte ptr [r12+rb_node.rb_parent_color], 1
                ;mov     rdx, r14
                mov     rax, [rcx+rbx*8+rb_node.rb_right]
                mov     rcx, r12
                or      qword ptr [rax+rb_node.rb_parent_color], 1
                call    cs_rbnode_rotate_direction
                mov     r15, [r14]
loc_1800324FF:   
                test    r15, r15
                jz      short loc_18003250E
                mov     rax, [r15+rb_node.rb_parent_color]
loc_180032507:         
                or      rax, 1
                mov     [r15+rb_node.rb_parent_color], rax
loc_18003250E:                        
                ;pop     rbx
                ;pop     rbp
                ;pop     rdi
                ;pop     rsi
                call    rest_low_gpregs_r10
                pop     r12
                ;pop     r14
                pop     r15
                ret
cs_rbnode_erase_from endp

;r14 = root
;rcx = node
;does not spoil r14
;spoils no xmm regs, does not use xmm state so no vzeroupper required before call
ALIGN 16
cs_rbnode_insert_and_color proc
             ;   push    r14
                ;push    rsi
                ;push    rdi
                ;push    rbp
                ;push    rbx
               ; push    rax
                ;push    rdx
                ;push    rcx
                call    save_low_gpregs_r10
                
                mov     rdi, rcx
                mov     rcx, [rcx+rb_node.rb_parent_color]
              ;  mov     r14, rdx
                and     rcx, 0FFFFFFFFFFFFFFF8h
                jnz     short loc_180031FB0
                jmp     loc_180032003
loc_180031F60:
                cmp     [rcx+r8*8+rb_node.rb_right], rdi
                jz      short loc_180031F90
loc_180031F67:
                or      rax, 1
               ; mov     rdx, r14
                mov     r8d, ebp
                mov     [rcx+rb_node.rb_parent_color], rax
                and     byte ptr [rbx+rb_node.rb_parent_color], 0FEh
                mov     rcx, rbx
                short_call cs_rbnode_rotate_direction
                ;call    cs_rbnode_rotate_direction
                mov     rcx, [rdi+rb_node.rb_parent_color]
                and     rcx, 0FFFFFFFFFFFFFFF8h
                jnz     short loc_180031FB0
                jmp     short loc_180032003
loc_180031F90: 
               ; mov     rdx, r14
                mov     rsi, rcx
                call    cs_rbnode_rotate_direction
                mov     rax, [rdi+rb_node.rb_parent_color]
                mov     rcx, rdi
                mov     rdi, rsi
                jmp     short loc_180031F67
loc_180031FB0: 
                mov     rax, [rcx+rb_node.rb_parent_color]
                test    al, 1
                jnz     short loc_180032003
                mov     rbx, rax
                xor     ebp, ebp
                and     rbx, 0FFFFFFFFFFFFFFF8h
                cmp     [rbx+rb_node.rb_left], rcx
                setz    bpl
                mov     r8d, ebp
                xor     r8d, 1
                mov     rdx, [rbx+r8*8+rb_node.rb_right]
                test    rdx, rdx
                jz      short loc_180031F60
                mov     rsi, [rdx+rb_node.rb_parent_color]
                test    sil, 1
                jnz     loc_180031F60
                or      rsi, 1
                mov     rdi, rbx
                mov     [rdx+rb_node.rb_parent_color], rsi
                or      byte ptr [rcx+rb_node.rb_parent_color], 1
                mov     rcx, [rbx+rb_node.rb_parent_color]
                and     rcx, 0FFFFFFFFFFFFFFFEh
                mov     [rbx+rb_node.rb_parent_color], rcx
                and     rcx, 0FFFFFFFFFFFFFFF8h
                jnz     short loc_180031FB0
loc_180032003: 
                mov     rax, [r14]
                or      qword ptr [rax+rb_node.rb_parent_color], 1
                jmp     rest_low_gpregs_r10 ;tail call
                ;call    rest_low_gpregs_r10
                ;ret
cs_rbnode_insert_and_color endp

;spoils no flags
;spoils edx (offset)
ALIGN 16
cs_rb_last proc
    mov edx, rb_node.rb_right
    jmp first_or_last_entrypoint
cs_rb_last endp
ALIGN 16
cs_rb_first proc
    mov edx, rb_node.rb_left
    ;fallthrough
cs_rb_first endp

first_or_last_entrypoint proc
    push rcx
    mov rcx, [rcx]
    jrcxz nullnode
ALIGN 16
loop_find_first_or_last:
    mov rax, rcx
    mov rcx, [rcx+rdx]
    jrcxz done
    mov rax, rcx
    mov rcx, [rcx+rdx]
    jrcxz done
    jmp loop_find_first_or_last
nullnode:
    ;was originally xor eax, eax but then i realized i could preserve all flags
    ;if i just reuse the 0 from rcx
    mov eax, ecx
done:
    pop rcx
    ret
first_or_last_entrypoint endp


;rbx = direction (pass in 8 for next, 16 for prev)
;rbx is not spoiled
;rax is the next or prev node, rdx is spoiled, rcx is spoiled
ALIGN 16
cs_rb_next_or_prev proc
                mov     rax, [rcx]
                and     rax, 0FFFFFFFFFFFFFFFEh
                cmp     rax, rcx
                jz      return_null
                mov     rdx, [rcx+rbx]
                test    rdx, rdx
                jz      check_null_then_search_parent
                push    rbx ;save and restore it here so that stack tracing has no problem renaming it
                            ;also, push and pop for rbx = less bytes than xoring again
                xor     ebx, 24 ;if 8, becomes 16, if 16 becomes 8
                mov     rcx, rdx
ALIGN 16
search_in_direction: ;we save bytes here by using jrcxz
                mov     rax, rcx
                mov     rcx, [rcx+rbx]
                jrcxz   exit_search_in_direction
                mov     rax, rcx
                mov     rcx, [rcx+rbx]
                jrcxz   exit_search_in_direction
                jmp     search_in_direction
                
exit_search_in_direction:
                pop rbx
early_return:
                ret
check_null_then_search_parent: 
                test    rax, rax
                jz      return_null
ALIGN 16
search_parent_direction: 
                cmp     rcx, [rax+rbx]
                jnz     early_return
                mov     rcx, rax
                mov     rax, [rax]
                and     rax, 0FFFFFFFFFFFFFFFEh
                jnz     search_parent_direction
return_null:
                xor     eax, eax
                ret
cs_rb_next_or_prev endp


ALIGN 16
;args - rcx = key, rdx = array base, r8 (high part) = number of elements, r8 (low part) = size of elements, r9 = compare function
; compare function must spoil no registers, and sets Z if the elements are equal, S if the lhs is less than the rhs, and neither if the element is greater
;
cs_bsearch_slo proc
                push    r14
                push    rsi
                push    rdi
                push    rbx
                mov     r10, rdx
                mov     rax, r8
                shr     rax, 20h
                lea     r11d, [rax-1]
                imul    r11d, r8d
                add     r11, rdx
                mov     esi, r8d
                neg     esi
                mov     r14d, r8d
ALIGN 16
loc_180027680:    
                mov     ebx, eax
                shr     ebx, 1
                jz      short loc_1800276BC
                and     eax, 1
                add     eax, ebx
                add     eax, 0FFFFFFFFh
                mov     edx, eax
                imul    edx, r8d
                add     rdx, r10
                call    r9
                jz      short loc_1800276D0
                lea     rdi, [rdx+rsi]
                lea     rdx, [rdx+r14]
                cmovs   r10, rdx
                cmovns  r11, rdi
                cmovs   eax, ebx
                cmp     r10, r11
                jbe     short loc_180027680
                jmp     short loc_1800276CE
loc_1800276BC:      
                test    eax, eax
                jz      short loc_1800276CE
                xor     eax, eax
                mov     rdx, r10
                call    r9
                cmovz   rax, r10
                jmp     short loc_1800276D3
loc_1800276CE:    
                xor     edx, edx
loc_1800276D0:        
                mov     rax, rdx
loc_1800276D3: 
                pop     rbx
                pop     rdi
                pop     rsi
                pop     r14
                ret
cs_bsearch_slo endp






_text ENDS
END