; VGA mode 13h display test for a DOS prompt. Build with:
;   nasm -f bin -o dos13.com tools/win9x/dos13.asm
; DOS13 holds each phase for ten seconds. DOS13 /K waits for Space instead.
; DOS13 /S also waits after the first component of the palette change.
; Escape returns to the original BIOS text mode during either hold.
bits 16
cpu 8086
org 100h

%macro rect 5                 ; x, y, width, height, palette index
    mov di, 320 * %2 + %1
    mov bx, %4
    mov al, %5
%%row:
    mov cx, %3
    rep stosb
    add di, 320 - %3
    dec bx
    jnz %%row
%endmacro

start:
    push cs
    pop ds
    cld
    xor cx, cx
    mov cl, [80h]             ; PSP command tail, excluding its carriage return
    mov si, 81h
    call skip_spaces
    jcxz args_ok
    cmp cx, 2
    jb usage
    lodsb
    cmp al, '/'
    jne usage
    lodsb
    or al, 20h
    cmp al, 'k'
    je .keyboard
    cmp al, 's'
    jne usage
    inc byte [keyboard]
.keyboard:
    inc byte [keyboard]
    sub cx, 2
    call skip_spaces
    or cx, cx
    jnz usage
args_ok:
    mov ah, 0fh
    int 10h
    and al, 7fh
    cmp al, 3
    jbe text_mode
    cmp al, 7
    jne wrong_start
text_mode:
    mov [old_mode], al
    mov ax, 0013h
    int 10h
    mov ah, 0fh
    int 10h
    and al, 7fh
    cmp al, 13h
    je draw
    mov si, mode_error
    jmp failed

usage:
    mov dx, help
    jmp dos_error
wrong_start:
    mov dx, text_required
dos_error:
    mov ah, 09h
    int 21h
    mov ax, 4c01h
    int 21h

draw:
    cld
    mov ax, 0a000h
    mov es, ax
    mov dx, 03c6h             ; enable all eight pixel-index bits
    mov al, 0ffh
    out dx, al
    mov si, palette_a
    xor al, al
    mov cx, 6 * 3
    call set_palette

    ; Color order from the top left is red, green, blue, yellow. The black
    ; frame/dividers and the white L at the upper left remain in both phases.
    xor di, di
    xor al, al
    mov cx, 320 * 200
    rep stosb
    rect   8,   8, 156,  78, 1
    rect 168,   8, 144,  78, 2
    rect   8,  90, 156, 102, 3
    rect 168,  90, 144, 102, 4
    rect  17,  17,   4,  16, 5
    rect  17,  29,  15,   4, 5
    rect 180, 116,  20,  16, 5
    call checksum
    mov [first_sum], dx
    mov si, frame_a
    call marker
    call hold
    jnc phase_b
    jmp cancelled

phase_b:
    mov si, palette_b
    mov dx, 03c8h
    mov al, 1                 ; only the four quadrant DAC entries change
    out dx, al
    inc dx
    lodsb
    out dx, al                ; leave green and blue pending for four BIOS ticks
    cmp byte [keyboard], 2
    jne .timed
    push si
    push dx
    mov si, split_msg
    call marker
    call hold
    pop dx
    pop si
    jc cancelled
.timed:
    call wait_for_dac_sample
    mov cx, 4 * 3 - 1
    call palette_data
    mov si, frame_b
    call marker
    call hold
    jc cancelled
    call checksum
    cmp dx, [first_sum]
    jne changed
    mov si, completed_msg
    call marker

    ; Phase C moves the white block across the gray quadrant. No DAC writes.
    rect 180, 116, 20, 16, 4
    rect 268, 158, 20, 16, 5
    mov si, frame_c
    call marker
    call hold
    jc cancelled
    cmp byte [es:116 * 320 + 180], 4
    jne bad_move
    cmp byte [es:158 * 320 + 268], 5
    jne bad_move
    mov si, moved_msg
    call marker
    call check_dac_read
    jc bad_dac
    mov si, read_ok
    jmp finish
cancelled:
    mov byte [status], 2
    mov si, cancelled_msg
    jmp finish
changed:
    mov si, changed_msg
    jmp failed
bad_move:
    mov si, move_error
    jmp failed
bad_dac:
    mov si, read_value
    call marker
    mov si, dac_read
    mov bx, hex_digits
    mov cl, 4
.component:
    lodsb
    mov ah, al
    shr al, cl
    xlat
    out 0e9h, al
    mov al, ah
    and al, 0fh
    xlat
    out 0e9h, al
    mov al, ' '
    out 0e9h, al
    cmp si, dac_read + 3
    jne .component
    mov si, read_error
failed:
    mov byte [status], 1
finish:
    call marker
    xor ah, ah
    mov al, [old_mode]
    int 10h
    mov ah, 0fh
    int 10h
    and al, 7fh
    cmp al, [old_mode]
    je restored
    mov si, restore_error
    call marker
    mov byte [status], 1
    jmp exit
restored:
    mov si, text_back
    call marker
exit:
    mov ah, 4ch
    mov al, [status]
    int 21h

skip_spaces:
    jcxz .done
.next:
    cmp byte [si], ' '
    jne .done
    inc si
    loop .next
.done:
    ret

set_palette:                  ; AL first DAC entry, DS:SI RGB triples, CX byte count
    mov dx, 03c8h
    out dx, al
    inc dx
palette_data:
.next:
    lodsb
    out dx, al
    loop .next
    ret

wait_for_dac_sample:
    push es
    mov ax, 0040h
    mov es, ax
    mov bx, [es:006ch]
    mov cx, 4
.next_tick:
    mov ax, [es:006ch]
    cmp ax, bx
    je .next_tick
    mov bx, ax
    ; Count transitions so a timer rollover at midnight is also just one tick.
    loop .next_tick
    pop es
    ret

check_dac_read:
    mov dx, 03c7h
    mov al, 3
    out dx, al
    mov dx, 03c9h
    in al, dx
    mov [dac_read], al
    call wait_for_dac_sample  ; the application's read cursor now points to green
    in al, dx
    mov [dac_read + 1], al
    in al, dx
    mov [dac_read + 2], al
    mov si, dac_read
    mov di, palette_b + 6     ; palette_b starts at DAC index 1
    mov cx, 3
.component:
    lodsb
    cmp al, [di]
    jne .wrong
    inc di
    loop .component
    clc
    ret
.wrong:
    stc
    ret

checksum:                     ; read all 64,000 VGA indices without modifying them
    xor si, si
    xor dx, dx
    mov cx, 320 * 200
.next:
    rol dx, 1
    xor dl, [es:si]
    inc si
    loop .next
    ret

marker:                       ; fixed NUL-terminated status to the QEMU debug port
    lodsb
    or al, al
    jz .done
    out 0e9h, al
    jmp marker
.done:
    ret

hold:
    push es
    mov ax, 0040h
    mov es, ax
    mov bx, [es:006ch]        ; low word of the BIOS tick counter (18.2 Hz)
    mov cx, 182
.poll:
    mov ah, 01h
    int 16h
    jz .timer
    xor ah, ah
    int 16h
    cmp al, 27
    je .cancel
    cmp al, ' '
    jne .timer
    cmp byte [keyboard], 0
    jne .done
.timer:
    mov ax, [es:006ch]
    cmp ax, bx
    je .idle
    mov dx, ax
    sub dx, bx
    mov bx, ax
    ; Midnight resets the BIOS day counter to zero. Treat that transition
    ; as one tick, instead of ending the hold early.
    cmp dx, 8000h
    jb .elapsed
    mov dx, 1
.elapsed:
    cmp byte [keyboard], 0
    jne .idle
    sub cx, dx
    jbe .done
.idle:
    push bx
    push cx
    mov ax, 1680h             ; release this DOS VM's timeslice while idle
    int 2fh
    pop cx
    pop bx
    jmp .poll
.done:
    pop es
    clc
    ret
.cancel:
    pop es
    stc
    ret

palette_a:     db 0,0,0, 63,0,0, 0,63,0, 0,0,63, 63,63,0, 63,63,63
palette_b:     db 0,63,63, 63,0,63, 63,32,0, 32,32,32
keyboard:      db 0
old_mode:      db 3
status:        db 0
first_sum:     dw 0
dac_read:      db 0,0,0
frame_a:       db 'dos13: frame A',13,10,0
frame_b:       db 'dos13: palette B',13,10,0
split_msg:     db 'dos13: waiting after first red component',13,10,0
frame_c:       db 'dos13: pixels C',13,10,0
completed_msg: db 'dos13: frame indices unchanged',13,10,0
moved_msg:     db 'dos13: block moved',13,10,0
read_ok:       db 'dos13: DAC entry 3 readback OK (63,32,0)',13,10,0
cancelled_msg: db 'dos13: cancelled',13,10,0
text_back:     db 'dos13: text restored',13,10,0
mode_error:    db 'dos13: mode 13h unavailable',13,10,0
changed_msg:   db 'dos13: ERROR frame indices changed',13,10,0
move_error:    db 'dos13: ERROR block move readback failed',13,10,0
read_value:    db 'dos13: DAC entry 3 RGB (hex) ',0
hex_digits:    db '0123456789ABCDEF'
read_error:    db 13,10,'dos13: ERROR DAC entry 3 readback differs from (63,32,0)',13,10,0
restore_error: db 'dos13: ERROR text mode restore failed',13,10,0
help:          db 'Usage: DOS13 [/K | /S]   /K waits for Space at each frame.',13,10
               db '/S also waits after the first red component of the palette change.',13,10,'$'
text_required: db 'DOS13 must start in BIOS text mode.',13,10,'$'
