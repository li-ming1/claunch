; chkstk.asm — 不链 CRT 的两个编译器桩：
;   1) __chkstk        栈探针（MSVC 约定：大小在 RAX），触发栈守护页正常增长
;   2) __GSHandlerCheck /GS 在异常路径上的引用点。正式实现在 ucrt，不链 CRT
;      时只能补空桩：本文件没有任何 __try/__except，异常根本走不到这里，
;      真正的 cookie 校验由 __security_check_cookie 完成
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
end
