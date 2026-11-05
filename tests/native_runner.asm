; Native 32-bit differential-test runner. snips.bin (256 bytes per test) is placed at 0x1000000.
bits 32
global _start
section .text
snips: incbin "snips.bin"
NT equ %NT%
ITER equ 48
SCRATCH equ 0x2000000
_start:
  mov eax, 192
  mov ebx, SCRATCH
  mov ecx, 0x2000
  mov edx, 3
  mov esi, 0x32
  mov edi, -1
  xor ebp, ebp
  int 0x80
  mov dword [seed], 0x2545F491
  mov dword [tcnt], 0
.tloop:
  mov dword [icnt], 0
.iloop:
  ; fill scratch with random data
  xor ecx, ecx
.fill:
  call rand
  mov edx, ecx
  shl edx, 2
  mov [SCRATCH + edx], eax
  inc ecx
  cmp ecx, 1024
  jb .fill
  ; registers: eax ecx edx ebx (skip esp) ebp esi edi
  mov ecx, 0
.gen:
  cmp ecx, 4
  je .skip
  call biased
  mov [regs + ecx*4], eax
.skip:
  inc ecx
  cmp ecx, 8
  jb .gen
  ; target address
  mov eax, [tcnt]
  shl eax, 8
  add eax, 0x1000000
  mov [target], eax
  mov [savedesp], esp
  mov esp, 0x2001f00
  push dword 2
  popfd
  mov ebx, [regs+12]
  mov ebp, [regs+20]
  mov esi, [regs+24]
  mov edi, [regs+28]
  mov edx, [regs+8]
  mov ecx, [regs+4]
  mov eax, [regs]
  call [target]
  cld
  mov [outregs], eax
  mov [outregs+4], ecx
  mov [outregs+8], edx
  mov [outregs+12], ebx
  mov [outregs+16], esp
  mov [outregs+20], ebp
  mov [outregs+24], esi
  mov [outregs+28], edi
  mov esp, [savedesp]
  ; hash of the scratch page
  xor eax, eax
  xor ecx, ecx
.hash:
  rol eax, 5
  xor eax, [SCRATCH + ecx*4]
  inc ecx
  cmp ecx, 1024
  jb .hash
  mov [rec+40], eax
  mov eax, [tcnt]
  mov [rec], eax
  mov eax, [icnt]
  mov [rec+4], eax
  xor ecx, ecx
.cp:
  mov eax, [outregs + ecx*4]
  mov [rec + 8 + ecx*4], eax
  inc ecx
  cmp ecx, 8
  jb .cp
  mov eax, 4
  mov ebx, 1
  mov ecx, rec
  mov edx, 44
  int 0x80
  inc dword [icnt]
  cmp dword [icnt], ITER
  jb .iloop
  inc dword [tcnt]
  cmp dword [tcnt], NT
  jb .tloop
  mov eax, 1
  xor ebx, ebx
  int 0x80

rand:
  push edx
  mov eax, [seed]
  mov edx, eax
  shl edx, 13
  xor eax, edx
  mov edx, eax
  shr edx, 17
  xor eax, edx
  mov edx, eax
  shl edx, 5
  xor eax, edx
  mov [seed], eax
  pop edx
  ret

biased:
  call rand
  push ecx
  mov ecx, eax
  and ecx, 7
  jz .z
  dec ecx
  jz .one
  dec ecx
  jz .m1
  dec ecx
  jz .min
  dec ecx
  jz .max
  dec ecx
  jz .byte
  jmp .done
.z:    xor eax, eax
  jmp .done
.one:  mov eax, 1
  jmp .done
.m1:   mov eax, -1
  jmp .done
.min:  mov eax, 0x80000000
  jmp .done
.max:  mov eax, 0x7fffffff
  jmp .done
.byte: and eax, 0xff
.done:
  pop ecx
  ret

section .data
seed: dd 0
tcnt: dd 0
icnt: dd 0
target: dd 0
savedesp: dd 0
regs: times 8 dd 0
outregs: times 8 dd 0
rec: times 11 dd 0
