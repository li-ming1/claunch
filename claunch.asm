; claunch.asm — 不链 CRT 的编译器桩与 mem* 实现：
;   1) __chkstk        栈探针（MSVC 约定：大小在 RAX），触发栈守护页正常增长
;   2) __GSHandlerCheck /GS 在异常路径上的引用点。正式实现在 ucrt，不链 CRT
;      时只能补空桩：本文件没有任何 __try/__except，异常根本走不到这里，
;      真正的 cookie 校验由 __security_check_cookie 完成
;   3) memcpy/memmove/memset/memcmp  —— VCRUNTIME140.dll 的全部引用点
;      （dumpbin /imports 实测只有这四个符号）。补上后链接行去掉
;      vcruntime.lib，导入表里不再出现任何 CRT。只能用汇编实现：C 里定义
;      同名函数会与 CRT 冲突，而 tmp/ 下 include claunch.c 的探针仍要链 CRT
.code
public __chkstk
__chkstk PROC
        push    rcx
        push    rax                 ; 保存大小
        mov     rcx, rsp            ; 探针起点
        sub     rcx, rax            ; 终点 = 栈顶 - 大小
        mov     rax, rsp
probe:
        sub     rax, 1000h          ; 一页
        mov     byte ptr [rax], 0   ; 触碰即提交
        cmp     rax, rcx
        ja      probe
        pop     rax
        pop     rcx
        ret
__chkstk ENDP

public __GSHandlerCheck
__GSHandlerCheck PROC
        ret
__GSHandlerCheck ENDP

; 以下四个故意保持最朴素：热路径上一次最大 155KB（Item 数组整体挪位），
; 其余调用点都是几十字节级。rep movsb/stosb 在现代 CPU 上走 ERMS，够用
; 且不必引入 SIMD 分支；memcmp 只在 COM QueryInterface 的 16 字节 GUID 上
; 出现。rsi/rdi 是 Win64 非易失寄存器，逐条存取都要还原

public memcpy
memcpy PROC
        mov     rax, rcx                ; 返回值 = dst
        mov     r9,  rdi
        mov     r10, rsi
        mov     rdi, rcx
        mov     rsi, rdx
        mov     rcx, r8                 ; count
        rep     movsb
        mov     rdi, r9
        mov     rsi, r10
        ret
memcpy ENDP

public memmove
memmove PROC
        mov     rax, rcx                ; 返回值 = dst
        cmp     rcx, rdx
        ja      mm_maybe_overlap        ; dst > src 才可能反向重叠
mm_forward:
        mov     r9,  rdi
        mov     r10, rsi
        mov     rdi, rcx
        mov     rsi, rdx
        mov     rcx, r8
        rep     movsb
        mov     rdi, r9
        mov     rsi, r10
        ret
mm_maybe_overlap:
        lea     r9, [rdx+r8]            ; src+n
        cmp     r9, rcx
        jbe     mm_forward              ; 不重叠：正向拷贝（快路径）
        mov     r9,  rdi
        mov     r10, rsi
        lea     rdi, [rcx+r8-1]         ; 尾部起
        lea     rsi, [rdx+r8-1]
        mov     rcx, r8
        std                             ; 反向：DF=1
        rep     movsb
        cld                             ; ABI 要求出口 DF=0
        mov     rdi, r9
        mov     rsi, r10
        ret
memmove ENDP

public memset
memset PROC
        mov     r11, rcx                ; 返回值 = dst（r11 易失，可当暂存）
        mov     r9,  rdi
        movzx   r10d, dl                ; c & 0xFF
        mov     rdi, rcx
        mov     rcx, r8
        mov     eax, r10d
        rep     stosb
        mov     rax, r11
        mov     rdi, r9
        ret
memset ENDP

public memcmp
memcmp PROC
        xor     eax, eax                ; n=0 时结果 0
        test    r8, r8
        je      mc_done
mc_loop:
        movzx   eax, byte ptr [rcx]
        movzx   r9d, byte ptr [rdx]
        sub     eax, r9d
        jne     mc_done
        inc     rcx
        inc     rdx
        dec     r8
        jne     mc_loop
mc_done:
        ret
memcmp ENDP
end
