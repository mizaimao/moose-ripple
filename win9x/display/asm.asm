; moose-ripple display driver for Windows 9x: assembly helpers.
;
;   - the library entry point (no C run-time is linked)
;   - the forwarders from the driver's exports to the DIB engine
;   - 386 instructions the 16-bit C compiler can't write: 32-bit port I/O, the VDD's
;     register-based calls, DPMI and BIOS interrupts
;
; MASM 6.11 (ml -c -Cp). Small model, 16-bit segments: .386 comes after .model on purpose.

        .model  small
        .386

        public  __acrtused              ; the C compiler asks for its start-up code by this name
__acrtused      equ     1

        .data
        extrn   _lpDriverPDevice:dword  ; enable.c
        extrn   _wPalettized:word
        extrn   _wEnabled:word
VddEntry        dd      0               ; the VDD's protected-mode API entry point (16:16)
VmHandle        dw      0               ; the system VM's handle, which that API wants in EBX
PrevInt2F       dd      0               ; the int 2Fh handler before ours

        .code
        extrn   _r9x_init:near          ; enable.c
        extrn   _r9x_reset:near

; ---- Library entry: KERNEL calls this once after loading the driver -------------------------
LibEntry proc far
        call    _r9x_init               ; DS is our data segment here
        mov     ax, 1
        ret
LibEntry endp

; ---- Forwarders ------------------------------------------------------------------------------
; With TRACE defined (ml -DTRACE) every forwarder first writes one letter to QEMU's debug port,
; so debug.log shows the last calls before a crash. AX is free on entry (Pascal convention).
TRACEC  macro   c
        ifdef   TRACE
        mov     al, c
        out     0E9h, al
        endif
        endm

; Most of the driver interface goes to the DIB engine unchanged: a far jump keeps the caller's
; parameters and return address in place.
FORWARD macro   name, target, c
        extrn   target:far
        public  name
name    proc    far
        TRACEC  c
        jmp     target
name    endp
        endm

; The engine's "Ext" functions take our PDEVICE as one more, last parameter: slip it in under
; the return address.
FORWARD_PDEV macro name, target, c
        extrn   target:far
        public  name
name    proc    far
        TRACEC  c
        mov     ax, DGROUP
        mov     es, ax
        pop     ecx                     ; the caller's far return address
        push    dword ptr es:[_lpDriverPDevice]
        push    ecx
        jmp     target
name    endp
        endm

        FORWARD         BitBlt,                 DIB_BitBlt, 'B'
        FORWARD         ColorInfo,              DIB_ColorInfo, 'C'
        FORWARD         EnumDFonts,             DIB_EnumDFonts, 'F'
        FORWARD_PDEV    EnumObj,                DIB_EnumObjExt, 'E'
        FORWARD         Output,                 DIB_Output, 'O'
        FORWARD         Pixel,                  DIB_Pixel, 'P'
        FORWARD_PDEV    RealizeObject,          DIB_RealizeObjectExt, 'R'
        FORWARD         StrBlt,                 DIB_StrBlt, 's'
        FORWARD         ScanLR,                 DIB_ScanLR, 'L'
        FORWARD         DeviceMode,             DIB_DeviceMode, 'M'
        FORWARD         ExtTextOut,             DIB_ExtTextOut, 'T'
        FORWARD         GetCharWidth,           DIB_GetCharWidth, 'W'
        FORWARD         DeviceBitmap,           DIB_DeviceBitmap, 'b'
        FORWARD         FastBorder,             DIB_FastBorder, 'f'
        FORWARD         SetAttribute,           DIB_SetAttribute, 'a'
        FORWARD         CreateDIBitmap,         DIB_CreateDIBitmap, 'D'
        FORWARD         DibToDevice,            DIB_DibToDevice, 'd'
        FORWARD_PDEV    GetPalette,             DIB_GetPaletteExt, 'g'
        FORWARD_PDEV    SetPaletteTranslate,    DIB_SetPaletteTranslateExt, 'x'
        FORWARD_PDEV    GetPaletteTranslate,    DIB_GetPaletteTranslateExt, 'X'
        FORWARD_PDEV    UpdateColors,           DIB_UpdateColorsExt, 'U'
        FORWARD         StretchBlt,             DIB_StretchBlt, 'S'
        FORWARD         StretchDIBits,          DIB_StretchDIBits, 'z'
        FORWARD         SelectBitmap,           DIB_SelectBitmap, 'm'
        FORWARD         BitmapBits,             DIB_BitmapBits, 'i'
        FORWARD         Inquire,                DIB_Inquire, 'I'
        FORWARD_PDEV    SetCursor,              DIB_SetCursorExt, 'K'
        FORWARD_PDEV    MoveCursor,             DIB_MoveCursorExt, 'k'

; DibBlt: the engine wants to know whether the screen has a palette (a word, not the PDEVICE).
        extrn   DIB_DibBltExt:far
        public  DibBlt
DibBlt  proc    far
        TRACEC  'l'
        mov     ax, DGROUP
        mov     es, ax
        pop     ecx
        push    word ptr es:[_wPalettized]
        push    ecx
        jmp     DIB_DibBltExt
DibBlt  endp

; CheckCursor comes from a timer; before Enable there is no PDEVICE to hand on.
        extrn   DIB_CheckCursorExt:far
        public  CheckCursor
CheckCursor proc far
        mov     ax, DGROUP                  ; (from a timer: not traced)
        mov     es, ax
        cmp     word ptr es:[_wEnabled], 0
        je      short cc_out
        pop     ecx
        push    dword ptr es:[_lpDriverPDevice]
        push    ecx
        jmp     DIB_CheckCursorExt
cc_out: ret
CheckCursor endp

; ---- Control: GDI's escapes. Ours are answered in C (control_escape in enable.c), the rest
; goes to the DIB engine with the caller's parameters still in place.
        extrn   _control_escape:near    ; enable.c
        extrn   DIB_Control:far
        public  Control
Control proc    far
        TRACEC  'c'
        push    bp
        mov     bp, sp
        push    ds
        mov     ax, DGROUP
        mov     ds, ax
        push    dword ptr [bp+6]        ; lpOutData
        push    dword ptr [bp+10]       ; lpInData
        push    word ptr [bp+14]        ; the escape's number
        call    _control_escape
        add     sp, 10
        pop     ds
        pop     bp
        cmp     ax, 8000h               ; R9X_NOT_OURS: not one of ours
        je      short ctl_forward
        retf    14
ctl_forward:
        jmp     DIB_Control
Control endp

; ---- ValidateMode: Display Properties asks whether a mode would work -----------------------
; The answer (0 for yes) goes back in all of EAX with DX zero: the callers are not all 16-bit.
        extrn   _validate_mode:near     ; enable.c
        public  ValidateMode
ValidateMode proc far
        push    bp
        mov     bp, sp
        push    ds
        mov     ax, DGROUP
        mov     ds, ax
        push    dword ptr [bp+6]        ; the mode description
        call    _validate_mode
        add     sp, 4
        movzx   eax, ax
        xor     dx, dx
        pop     ds
        pop     bp
        retf    4
ValidateMode endp

; ---- The VDD calls this after a full-screen DOS session, to get the screen mode back --------
; DS is not ours on entry.
        public  ResetHiResMode
ResetHiResMode proc far
        push    ds
        push    es
        pushad
        mov     ax, DGROUP
        mov     ds, ax
        call    _r9x_reset
        popad
        pop     es
        pop     ds
        ret
ResetHiResMode endp

; ---- Screen switches: int 2Fh, AX=4001h (Windows' screen goes to the background, e.g. a
; full-screen DOS session) and AX=4002h (it comes back). The VDD sends them; a display driver
; hooks the interrupt to stop drawing in between and to set its mode again afterwards.
        extrn   _r9x_switch:near        ; enable.c
        public  Int2FHook
Int2FHook proc  far
        cmp     ax, 4001h
        je      short i2f_ours
        cmp     ax, 4002h
        je      short i2f_ours
        ; Not ours: on to the previous handler with every register as it came. Its address is
        ; put on the stack under the caller's frame, and a far return jumps there.
        push    ds                      ; two words to be overwritten with the address
        push    ds
        push    bp
        mov     bp, sp
        push    ds
        push    ax
        mov     ax, DGROUP
        mov     ds, ax
        mov     ax, word ptr PrevInt2F
        mov     [bp+2], ax
        mov     ax, word ptr PrevInt2F+2
        mov     [bp+4], ax
        pop     ax
        pop     ds
        pop     bp
        retf
i2f_ours:
        push    ds
        push    es
        pushad
        mov     bx, DGROUP
        mov     ds, bx
        push    ax
        call    _r9x_switch
        add     sp, 2
        popad
        pop     es
        pop     ds
        push    bp                      ; carry clear in the caller's flags: the switch may go on
        mov     bp, sp
        and     word ptr [bp+6], 0FFFEh
        pop     bp
        iret
Int2FHook endp

; void hook_int2f(void) / void unhook_int2f(void): DOS get and set interrupt vector
        public  _hook_int2f
_hook_int2f proc near
        push    es
        push    bx
        cmp     dword ptr PrevInt2F, 0
        jne     short hk_out
        mov     ax, 352Fh
        int     21h
        mov     word ptr PrevInt2F, bx
        mov     word ptr PrevInt2F+2, es
        push    ds
        push    cs
        pop     ds
        mov     dx, offset Int2FHook
        mov     ax, 252Fh
        int     21h
        pop     ds
hk_out: pop     bx
        pop     es
        ret
_hook_int2f endp

        public  _unhook_int2f
_unhook_int2f proc near
        cmp     dword ptr PrevInt2F, 0
        je      short uh_out
        push    ds
        lds     dx, PrevInt2F
        mov     ax, 252Fh
        int     21h
        pop     ds
        mov     dword ptr PrevInt2F, 0
uh_out: ret
_unhook_int2f endp

; ---- Helpers for the C code: near, C calling convention, DS = DGROUP -----------------------

; WORD inpw_(WORD port)
        public  _inpw_
_inpw_  proc    near
        push    bp
        mov     bp, sp
        mov     dx, [bp+4]
        in      ax, dx
        pop     bp
        ret
_inpw_  endp

; void outpw_(WORD port, WORD value)
        public  _outpw_
_outpw_ proc    near
        push    bp
        mov     bp, sp
        mov     dx, [bp+4]
        mov     ax, [bp+6]
        out     dx, ax
        pop     bp
        ret
_outpw_ endp

; BYTE inp_(WORD port)
        public  _inp_
_inp_   proc    near
        push    bp
        mov     bp, sp
        mov     dx, [bp+4]
        in      al, dx
        xor     ah, ah
        pop     bp
        ret
_inp_   endp

; void outp_(WORD port, BYTE value)
        public  _outp_
_outp_  proc    near
        push    bp
        mov     bp, sp
        mov     dx, [bp+4]
        mov     al, [bp+6]
        out     dx, al
        pop     bp
        ret
_outp_  endp

; DWORD pci_read(WORD busdevfn, WORD reg): PCI configuration mechanism 1
        public  _pci_read
_pci_read proc  near
        push    bp
        mov     bp, sp
        movzx   eax, word ptr [bp+4]
        shl     eax, 8
        or      eax, 80000000h
        mov     al, [bp+6]
        and     al, 0FCh
        mov     dx, 0CF8h
        out     dx, eax
        mov     dx, 0CFCh
        in      eax, dx
        mov     edx, eax
        shr     edx, 16                 ; result in DX:AX
        pop     bp
        ret
_pci_read endp

; DWORD dpmi_map(DWORD phys, DWORD size): DPMI function 0800h, physical address mapping
        public  _dpmi_map
_dpmi_map proc  near
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    bx
        mov     cx, [bp+4]
        mov     bx, [bp+6]              ; BX:CX = physical address
        mov     di, [bp+8]
        mov     si, [bp+10]             ; SI:DI = size
        mov     ax, 0800h
        int     31h
        jc      short dm_fail
        mov     dx, bx                  ; BX:CX = linear address
        mov     ax, cx
        jmp     short dm_out
dm_fail:
        xor     ax, ax
        xor     dx, dx
dm_out: pop     bx
        pop     di
        pop     si
        pop     bp
        ret
_dpmi_map endp

; void vdd_init(void): int 2Fh function 1684h, device 000Ah (the VDD), and 1683h (our VM)
        public  _vdd_init
_vdd_init proc  near
        push    di
        push    es
        push    bx
        xor     di, di
        mov     es, di
        mov     ax, 1684h
        mov     bx, 000Ah
        int     2Fh
        mov     word ptr VddEntry, di
        mov     word ptr VddEntry+2, es
        mov     ax, 1683h
        int     2Fh
        mov     VmHandle, bx
        pop     bx
        pop     es
        pop     di
        ret
_vdd_init endp

; DWORD vdd_call(DWORD func, DWORD ecx, DWORD edx, WORD es, WORD di)
; Without a VDD the function code comes back unchanged, which is also the VDD's own answer
; to a function it doesn't know.
        public  _vdd_call
_vdd_call proc  near
        push    bp
        mov     bp, sp
        push    esi
        push    edi
        push    ebx
        push    es
        mov     eax, [bp+4]
        cmp     dword ptr VddEntry, 0
        je      short vc_out
        movzx   ebx, VmHandle
        mov     ecx, [bp+8]
        mov     edx, [bp+12]
        mov     es, [bp+16]
        movzx   edi, word ptr [bp+18]
        call    dword ptr VddEntry
vc_out: mov     edx, eax
        shr     edx, 16                 ; result in DX:AX
        pop     es
        pop     ebx
        pop     edi
        pop     esi
        pop     bp
        ret
_vdd_call endp

; void int2f(WORD ax)
        public  _int2f
_int2f  proc    near
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    bx
        push    es
        mov     ax, [bp+4]
        int     2Fh
        pop     es
        pop     bx
        pop     di
        pop     si
        pop     bp
        ret
_int2f  endp

; void bios_textmode(void): 80x25 text through the video BIOS
        public  _bios_textmode
_bios_textmode proc near
        push    si
        push    di
        push    bx
        push    bp
        mov     ax, 3
        int     10h
        pop     bp
        pop     bx
        pop     di
        pop     si
        ret
_bios_textmode endp

; WORD muldiv_(WORD a, WORD b, WORD c): a * b / c, rounded to nearest
        public  _muldiv_
_muldiv_ proc   near
        push    bp
        mov     bp, sp
        push    bx
        mov     ax, [bp+4]
        mul     word ptr [bp+6]
        mov     cx, [bp+8]
        mov     bx, cx
        shr     bx, 1
        add     ax, bx
        adc     dx, 0
        div     cx
        pop     bx
        pop     bp
        ret
_muldiv_ endp

; DWORD mul32(WORD a, WORD b)
        public  _mul32
_mul32  proc    near
        push    bp
        mov     bp, sp
        mov     ax, [bp+4]
        mul     word ptr [bp+6]
        pop     bp
        ret
_mul32  endp

; DWORD div32(DWORD a, WORD b): a / b (b not 0); 32-bit division without the C run-time
        public  _div32
_div32  proc    near
        push    bp
        mov     bp, sp
        mov     eax, [bp+4]
        movzx   ecx, word ptr [bp+8]
        xor     edx, edx
        div     ecx
        mov     edx, eax
        shr     edx, 16
        pop     bp
        ret
_div32  endp

; WORD dib_create(LPVOID lpInfo, LPVOID lpDevice, LPVOID lpBits, WORD flags)
; CreateDIBPDevice answers in EAX (the size of the screen in bytes, 0 for failure), which
; 16-bit C can't test: a 3 MB screen has a zero low word.
        extrn   CREATEDIBPDEVICE:far
        public  _dib_create
_dib_create proc near
        push    bp
        mov     bp, sp
        push    dword ptr [bp+4]        ; lpInfo
        push    dword ptr [bp+8]        ; lpDevice
        push    dword ptr [bp+12]       ; lpBits
        push    word ptr [bp+16]        ; flags
        call    CREATEDIBPDEVICE
        test    eax, eax
        setnz   al
        xor     ah, ah
        pop     bp
        ret
_dib_create endp

; DWORD sel_limit(WORD sel): the segment limit of a selector (LSL), 0 if it isn't valid
        public  _sel_limit
_sel_limit proc near
        push    bp
        mov     bp, sp
        movzx   edx, word ptr [bp+4]
        xor     eax, eax
        lsl     eax, edx
        mov     edx, eax
        shr     edx, 16
        pop     bp
        ret
_sel_limit endp

; void fb_fill(WORD sel, DWORD offset, DWORD count, BYTE value): bytes at sel:offset
        public  _fb_fill
_fb_fill proc   near
        push    bp
        mov     bp, sp
        push    es
        push    edi
        mov     es, [bp+4]
        mov     edi, [bp+6]
        mov     ecx, [bp+10]
        mov     al, [bp+14]
        cld
        rep stos byte ptr es:[edi]
        pop     edi
        pop     es
        pop     bp
        ret
_fb_fill endp

; void dbg_putc(BYTE c): QEMU's debug console port; nothing listens there on a real PC
        public  _dbg_putc
_dbg_putc proc  near
        push    bp
        mov     bp, sp
        mov     al, [bp+4]
        out     0E9h, al
        pop     bp
        ret
_dbg_putc endp

        end     LibEntry
