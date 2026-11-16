; moose-ripple mini-VDD for Windows 9x (ripplevd.vxd).
;
; Windows' virtual display device (the VDD) keeps DOS sessions from disturbing the screen by
; trapping the standard VGA ports. QEMU's card has two more, the Bochs mode-setting registers at
; 01CEh (index) and 01CFh (data), and its video BIOS writes them on every mode set. The VDD
; knows nothing of them, so a DOS session in a window, or a BIOS call inside the Windows VM,
; switched the real card out of the display driver's mode.
;
; This VxD is loaded by the VDD (registry value "minivdd") and owns those two ports:
;   - the VM that has the screen reaches the card: the Windows display driver (protected mode
;     in the Windows VM), or a full-screen DOS session;
;   - everything else gets a private copy of the registers and never touches the card:
;     DOS sessions that don't have the screen, and real-mode BIOS code in the Windows VM while
;     the display driver has the card in its mode.
;
; Built with MASM 6.11 and the Windows 98 DDK's vmm.inc and minivdd.inc (not in this repository):
;   ml -coff -DBLD_COFF -DMASM6 -DMINIVDD=1 -c -Cx ripplevd.asm ; link /VXD

        .386p
        .xlist
        include vmm.inc
        include minivdd.inc
        .list

Declare_Virtual_Device RIPPLEVD, 4, 0, RippleVD_Control, Undefined_Device_ID, VDD_Init_Order

VBE_INDEX       equ     01CEh
VBE_DATA        equ     01CFh
VBE_REGS        equ     11              ; registers 0 to 10
VBE_ID          equ     0
VBE_ENABLE      equ     4
VBE_VRAM_64K    equ     10

; What the display driver passes through VDD_REGISTER_DISPLAY_DRIVER_INFO, in ECX:
RVD_HIRES_ON    equ     1               ; the driver has set its mode
RVD_HIRES_OFF   equ     0
RVD_SIGNATURE   equ     31445652h       ; "RVD1", our answer in EAX

VxD_LOCKED_DATA_SEG
WindowsVM       dd      0               ; the system VM's handle
HiRes           dd      0               ; the display driver has the card in its mode
Disabled        dd      0               ; the display driver was disabled (Windows ends)
ModeChange      dd      0               ; the VDD is switching the screen to or from a DOS session
VIndex          dw      0               ; the private copy: index register
VRegs           dw      VBE_REGS dup (0);                   and the registers behind it
VramBlocks      dw      0               ; the card's memory in 64 KB blocks, read once
LoadedMsg       db      'rvd: moose-ripple mini-VDD loaded', 13, 10, 0
; what the VDD tells us, on QEMU's debug port
MsgRegOn        db      'rvd: display driver in its mode', 13, 10, 0
MsgRegOff       db      'rvd: display driver out of its mode', 13, 10, 0
MsgDisabling    db      'rvd: display driver disabling', 13, 10, 0
MsgToVGA        db      'rvd: screen to a VGA mode', 13, 10, 0
MsgToHiRes      db      'rvd: screen back to the display driver', 13, 10, 0
MsgSwitched     db      'rvd: mode switch done', 13, 10, 0
MsgSysExit      db      'rvd: Windows is ending', 13, 10, 0
MsgVMEnd        db      'rvd: a VM ends', 13, 10, 0
VxD_LOCKED_DATA_ENDS

VxD_LOCKED_CODE_SEG

Begin_Control_Dispatch RippleVD
        Control_Dispatch Sys_Dynamic_Device_Init, MiniVDD_Dynamic_Init
        Control_Dispatch Device_Init, MiniVDD_Dynamic_Init
        Control_Dispatch W32_DEVICEIOCONTROL, RippleVD_IoControl
        Control_Dispatch System_Exit, RippleVD_SysExit
        Control_Dispatch Sys_VM_Terminate, RippleVD_VMEnd
End_Control_Dispatch RippleVD

; Say: ESI = zero-terminated text for QEMU's debug port. Keeps every register and the flags.
BeginProc RippleVD_Say
        pushfd
        push    eax
        push    esi
say_next:
        lodsb
        test    al, al
        jz      short say_done
        out     0E9h, al
        jmp     short say_next
say_done:
        pop     esi
        pop     eax
        popfd
        ret
EndProc RippleVD_Say

SAY     macro   msg
        push    esi
        mov     esi, OFFSET32 msg
        call    RippleVD_Say
        pop     esi
        endm

BeginProc RippleVD_SysExit
        SAY     MsgSysExit
        clc
        ret
EndProc RippleVD_SysExit

BeginProc RippleVD_VMEnd
        SAY     MsgVMEnd
        clc
        ret
EndProc RippleVD_VMEnd

; ---- The trap for ports 01CEh and 01CFh -------------------------------------------------------
; VMM calls this for every access from ring 3 or virtual 8086 mode:
;   EAX = value written, EBX = VM handle, ECX = kind of access, EDX = port, EBP = client registers
; and takes EAX as the value read.
BeginProc RippleVD_VbeTrap
        cmp     ecx, Word_Output        ; bytes and words only; VMM splits strings and dwords
        jbe     short vt_simple         ; and calls back
        VMMJmp  Simulate_IO
vt_simple:
        cmp     ModeChange, 0
        jne     short vt_physical       ; the VDD's own mode switch: nothing is held back
        push    esi
        push    edi
        push    eax
        VxDCall VDD_Get_VM_Info         ; EDI = the VM that has the screen
        pop     eax
        cmp     ebx, edi
        pop     edi
        pop     esi
        jne     short vt_private        ; a VM without the screen
        cmp     ebx, WindowsVM
        jne     short vt_physical       ; a full-screen DOS session: the card is its own
        cmp     HiRes, 0
        je      short vt_physical       ; Windows before or after its display driver
        test    [ebp.Client_EFlags], VM_Mask
        jnz     short vt_private        ; BIOS code in the Windows VM
vt_physical:
        cmp     ecx, Word_Output
        je      short vp_outw
        cmp     ecx, Word_Input
        je      short vp_inw
        cmp     ecx, Byte_Output
        je      short vp_outb
        in      al, dx
        ret
vp_outb:
        out     dx, al
        ret
vp_inw: in      ax, dx
        ret
vp_outw:
        out     dx, ax
        ret

; The private copy. The card only decodes word accesses, so bytes read as FFh and are dropped.
vt_private:
        cmp     ecx, Word_Output
        je      short vv_outw
        cmp     ecx, Word_Input
        je      short vv_inw
        mov     al, 0FFh
        ret
vv_outw:
        cmp     edx, VBE_INDEX
        jne     short vv_outdata
        mov     VIndex, ax
        ret
vv_outdata:
        push    ecx
        movzx   ecx, VIndex
        cmp     ecx, VBE_REGS
        jae     short vv_outdone
        mov     VRegs[ecx*2], ax
vv_outdone:
        pop     ecx
        ret
vv_inw:
        cmp     edx, VBE_INDEX
        jne     short vv_indata
        mov     ax, VIndex
        ret
vv_indata:
        push    ecx
        movzx   ecx, VIndex
        mov     ax, 0B0C5h              ; the interface version the card reports
        cmp     ecx, VBE_ID
        je      short vv_indone
        mov     ax, VramBlocks
        cmp     ecx, VBE_VRAM_64K
        je      short vv_indone
        xor     ax, ax
        cmp     ecx, VBE_REGS
        jae     short vv_indone
        mov     ax, VRegs[ecx*2]
vv_indone:
        pop     ecx
        ret
EndProc RippleVD_VbeTrap

; ---- Called by the VDD (mini-VDD functions). EBX and EBP are kept, as the interface asks. ---

; The display driver's VDD_REGISTER_DISPLAY_DRIVER_INFO call: EBP = its registers.
BeginProc MiniVDD_RegisterDisplayDriver
        mov     eax, [ebp.Client_ECX]
        and     eax, 1
        mov     HiRes, eax
        test    eax, eax
        jz      short rd_off
        mov     Disabled, 0             ; the driver is back (Windows runs on)
        SAY     MsgRegOn
        jmp     short rd_said
rd_off: SAY     MsgRegOff
rd_said:
        mov     [ebp.Client_EAX], RVD_SIGNATURE
        ret
EndProc MiniVDD_RegisterDisplayDriver

; The display driver's Disable (Windows shuts down or leaves the screen to DOS for good).
BeginProc MiniVDD_DisplayDriverDisabling
        SAY     MsgDisabling
        mov     HiRes, 0
        mov     Disabled, 1
        call    RippleVD_MapVGA
        ret
EndProc MiniVDD_DisplayDriverDisabling

; The VGA window (A0000h-BFFFFh) of the card itself into Windows' VM. After the display driver
; is disabled (Windows ends), Windows sets text mode with the video BIOS; the VDD catches the
; BIOS's first write to the text window as a page fault and fills 64 KB of "text memory" from
; B8000h through its alias of physical memory: past the window, over the video BIOS at C0000h
; (QEMU under KVM doesn't write-protect it), and the BIOS dies (exception 6, a blue screen
; instead of a restart). Mapped, the window takes the BIOS's writes without faults.
BeginProc RippleVD_MapVGA
        pushad
        mov     eax, WindowsVM
        VMMCall _PhysIntoV86, <0A0h, eax, 0A0h, 20h, 0>
        popad
        ret
EndProc RippleVD_MapVGA

; Before a video BIOS mode set in a VM: Windows' own after its display driver is disabled
; gets the card's VGA window (see RippleVD_MapVGA).
BeginProc MiniVDD_PreInt10ModeSet
        cmp     Disabled, 0
        je      short pi_done
        cmp     ebx, WindowsVM
        jne     short pi_done
        call    RippleVD_MapVGA
pi_done:
        clc
        ret
EndProc MiniVDD_PreInt10ModeSet

; The VDD is about to give the screen to a DOS session (or to its own message screen) and will
; set a VGA mode: take the card out of the display driver's mode, and let everything through
; until the switch is done.
BeginProc MiniVDD_PreHiResToVGA
        SAY     MsgToVGA
        mov     ModeChange, 1
        push    eax
        push    edx
        mov     dx, VBE_INDEX
        mov     ax, VBE_ENABLE
        out     dx, ax
        inc     dx
        xor     ax, ax
        out     dx, ax
        pop     edx
        pop     eax
        ret
EndProc MiniVDD_PreHiResToVGA

; The screen comes back to Windows: the display driver is about to set its mode again.
BeginProc MiniVDD_PreVGAToHiRes
        SAY     MsgToHiRes
        mov     ModeChange, 1
        ret
EndProc MiniVDD_PreVGAToHiRes

BeginProc MiniVDD_PostModeSwitch       ; POST_HIRES_TO_VGA and POST_VGA_TO_HIRES
        SAY     MsgSwitched
        mov     ModeChange, 0
        ret
EndProc MiniVDD_PostModeSwitch

; ---- DeviceIoControl from Win32 programs (CreateFile on \\.\RIPPLEVD): ESI = the parameters.
; EAX = 0 for success, or a Win32 error code.
        extrn   _vgpu_ioctl:near                ; vgpu.c
BeginProc RippleVD_IoControl
        push    esi
        call    _vgpu_ioctl
        add     esp, 4
        ret
EndProc RippleVD_IoControl

; ---- What vgpu.c needs from VMM: near, C calling convention ----------------------------------

; void *vmm_map_phys(u32 phys, u32 bytes): a ring-0 address for device memory, 0 on failure
BeginProc _vmm_map_phys
        mov     eax, [esp+4]
        mov     ecx, [esp+8]
        VMMCall _MapPhysToLinear, <eax, ecx, 0>
        cmp     eax, -1
        jne     short mp_ok
        xor     eax, eax
mp_ok:  ret
EndProc _vmm_map_phys

; void *vmm_alloc_contig(u32 pages, u32 *phys): zeroed, fixed, physically contiguous pages
; below 4 GB; their physical address through phys. 0 on failure.
BeginProc _vmm_alloc_contig
        mov     ecx, [esp+4]
        mov     edx, [esp+8]
        VMMCall _PageAllocate, <ecx, PG_SYS, 0, 0, 0, 0FFFFFh, edx, PAGEZEROINIT+PAGEUSEALIGN+PAGECONTIG+PAGEFIXED>
        mov     eax, edx                ; EDX = the address (EAX is a handle we don't keep)
        ret
EndProc _vmm_alloc_contig

; void *vmm_alloc_pages(u32 pages, u32 *handle): zeroed system pages that are never paged out or
; moved (so their physical addresses stay); the handle for vmm_free_pages. 0 on failure.
BeginProc _vmm_alloc_pages
        mov     ecx, [esp+4]
        VMMCall _PageAllocate, <ecx, PG_SYS, 0, 0, 0, 0, 0, PAGEZEROINIT+PAGEFIXED>
        mov     ecx, [esp+8]
        mov     [ecx], eax
        mov     eax, edx
        ret
EndProc _vmm_alloc_pages

; void *vmm_alloc_pageable(u32 pages): zeroed system pages that may be paged out (memory only
; programs touch, through the same address in every process). 0 on failure. Never freed.
BeginProc _vmm_alloc_pageable
        mov     ecx, [esp+4]
        VMMCall _PageAllocate, <ecx, PG_SYS, 0, 0, 0, 0, 0, PAGEZEROINIT>
        mov     eax, edx
        ret
EndProc _vmm_alloc_pageable

; void vmm_free_pages(u32 handle)
BeginProc _vmm_free_pages
        mov     eax, [esp+4]
        VMMCall _PageFree, <eax, 0>
        ret
EndProc _vmm_free_pages

; int vmm_page_table(void *addr, u32 pages, u32 *entries): the page table entries of a range
; (the physical address of each page is in bits 12 and up). 0 on failure.
BeginProc _vmm_page_table
        mov     eax, [esp+4]
        shr     eax, 12
        mov     ecx, [esp+8]
        mov     edx, [esp+12]
        VMMCall _CopyPageTable, <eax, ecx, edx, 0>
        ret
EndProc _vmm_page_table

; u32 vmm_time_ms(void): milliseconds since Windows started
BeginProc _vmm_time_ms
        VMMCall Get_System_Time
        ret
EndProc _vmm_time_ms

; u32 vmm_mutex_create(void): a VMM mutex (no priority boost, no flags); 0 on failure
BeginProc _vmm_mutex_create
        VMMCall _CreateMutex, <0, 0>
        ret
EndProc _vmm_mutex_create

; void vmm_mutex_enter(u32 mutex): the thread owns it when this returns, and may block before
BeginProc _vmm_mutex_enter
        mov     eax, [esp+4]
        VMMCall _EnterMutex, <eax, BLOCK_SVC_IF_INTS_LOCKED+BLOCK_ENABLE_INTS>
        ret
EndProc _vmm_mutex_enter

; void vmm_mutex_leave(u32 mutex)
BeginProc _vmm_mutex_leave
        mov     eax, [esp+4]
        VMMCall _LeaveMutex, <eax>
        ret
EndProc _vmm_mutex_leave

VxD_LOCKED_CODE_ENDS

; ---- Initialisation ---------------------------------------------------------------------------
VxD_ICODE_SEG

        extrn   _vgpu_init:near                 ; vgpu.c

BeginProc MiniVDD_Dynamic_Init
        push    ebx
        VMMCall Get_Sys_VM_Handle
        mov     WindowsVM, ebx
        pop     ebx

        ; Is this the card? Its ID register reads B0Cxh.
        mov     dx, VBE_INDEX
        xor     ax, ax
        out     dx, ax
        inc     dx
        in      ax, dx
        and     ax, 0FFF0h
        cmp     ax, 0B0C0h
        jne     di_fail
        dec     dx
        mov     ax, VBE_VRAM_64K
        out     dx, ax
        inc     dx
        in      ax, dx
        mov     VramBlocks, ax

        VxDCall VDD_Get_Mini_Dispatch_Table     ; EDI = the VDD's table, ECX = its length
        cmp     ecx, NBR_MINI_VDD_FUNCTIONS
        jb      di_fail
        MiniVDDDispatch REGISTER_DISPLAY_DRIVER, RegisterDisplayDriver
        MiniVDDDispatch DISPLAY_DRIVER_DISABLING, DisplayDriverDisabling
        MiniVDDDispatch PRE_HIRES_TO_VGA, PreHiResToVGA
        MiniVDDDispatch POST_HIRES_TO_VGA, PostModeSwitch
        MiniVDDDispatch PRE_VGA_TO_HIRES, PreVGAToHiRes
        MiniVDDDispatch POST_VGA_TO_HIRES, PostModeSwitch
        MiniVDDDispatch PRE_INT_10_MODE_SET, PreInt10ModeSet

        mov     esi, OFFSET32 RippleVD_VbeTrap
        mov     edx, VBE_INDEX
        VMMCall Install_IO_Handler
        jc      short di_fail
        mov     esi, OFFSET32 RippleVD_VbeTrap
        mov     edx, VBE_DATA
        VMMCall Install_IO_Handler
        jc      short di_fail

        mov     esi, OFFSET32 LoadedMsg         ; a line on QEMU's debug port
di_msg: lodsb
        test    al, al
        jz      short di_ok
        out     0E9h, al
        jmp     short di_msg
di_ok:  call    _vgpu_init                      ; the virtio side (vgpu.c); it may find nothing
        clc
        ret
di_fail:
        stc
        ret
EndProc MiniVDD_Dynamic_Init

VxD_ICODE_ENDS

        end
