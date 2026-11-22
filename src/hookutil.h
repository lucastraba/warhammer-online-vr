// hookutil.h - tiny helpers for a 5-byte inline hook on 32-bit x86 (no Windows dependency, unit tested).
//
// PrologueLength() looks at the first bytes of a function and returns how many bytes (>= 5) of WHOLE instructions
// can be copied into a trampoline unchanged. It only understands the instructions that Microsoft DLLs, Wine, DXVK
// and common hooking tools put at the start of a function: endbr32, mov edi,edi, push/pop reg, push imm, mov reg,imm,
// the common ALU/mov ops with a ModRM operand (register or memory: [reg], [reg+disp], [esp+disp] via SIB, [abs32]),
// op r/m32,imm8/imm32, jmp rel32 and jmp [abs]. On 32-bit x86 a memory operand never depends on where the code runs,
// so those copy unchanged. Anything else returns 0 and the caller must NOT hook (it logs the bytes so the decoder
// can be extended).
#pragma once
#include <cstddef>
#include <cstdint>

namespace hk {

// Length of a ModRM byte plus its SIB byte and displacement (32-bit addressing).
inline size_t ModRmLength(const unsigned char* m)
{
    const unsigned mod = m[0] >> 6, rm = m[0] & 7;
    if (mod == 3) return 1;
    size_t l = 1;
    unsigned base = rm;
    if (rm == 4) { ++l; base = m[1] & 7; }                                 // SIB
    if (mod == 0) { if (rm == 5 || (rm == 4 && base == 5)) l += 4; }       // [disp32] / [index*s+disp32]
    else if (mod == 1) l += 1;
    else l += 4;
    return l;
}

// relOff: set to the offset of a rel32 operand that must be re-based when the bytes are copied (a jmp rel32 that
// somebody else - an overlay, a capture tool - already put there), or -1 when there is none.
inline size_t PrologueLength(const unsigned char* p, int* relOff)
{
    if (relOff) *relOff = -1;
    size_t n = 0;
    while (n < 5) {
        const unsigned char* q = p + n;
        const unsigned char b = q[0];
        size_t l = 0;
        if (b == 0xF3 && q[1] == 0x0F && q[2] == 0x1E && q[3] == 0xFB) l = 4;   // endbr32 (Wine and other CET builds)
        else if (b >= 0x50 && b <= 0x5F) l = 1;                              // push/pop r32
        else if (b == 0x90) l = 1;                                           // nop
        else if (b == 0x6A) l = 2;                                           // push imm8
        else if (b == 0x68) l = 5;                                           // push imm32
        else if (b >= 0xB8 && b <= 0xBF) l = 5;                              // mov r32, imm32
        else if (b == 0x8B || b == 0x89 || b == 0x33 || b == 0x31 || b == 0x85 || b == 0x3B || b == 0x39 ||
                 b == 0x2B || b == 0x29 || b == 0x03 || b == 0x01 || b == 0x8D) l = 1 + ModRmLength(q + 1);  // op r/m32,r32; lea
        else if (b == 0x83) l = 2 + ModRmLength(q + 1);                      // op r/m32, imm8 (sub esp,8 / and esp,-8)
        else if (b == 0x81 || b == 0xC7) l = 5 + ModRmLength(q + 1);         // op r/m32, imm32; mov r/m32, imm32
        else if (b == 0xE9) {                                                // jmp rel32 (already hooked by someone else)
            if (relOff && *relOff >= 0) return 0;
            if (relOff) *relOff = (int)n + 1;
            l = 5;
        }
        else if (b == 0xFF && q[1] == 0x25) l = 6;                           // jmp [abs32]  (absolute: position independent)
        else return 0;
        n += l;
    }
    return n;
}

// Write the trampoline for `len` bytes copied from `src` (running at address `srcAddr`) into `tramp` (running at
// `trampAddr`): the copied bytes, with a jmp rel32 operand re-based when relOff >= 0, then `jmp srcAddr+len`.
// Returns the total trampoline size (len + 5). Addresses are passed as integers so this is testable off-target.
inline size_t BuildTrampoline(const unsigned char* src, uint32_t srcAddr, unsigned char* tramp, uint32_t trampAddr, size_t len, int relOff)
{
    for (size_t i = 0; i < len; ++i) tramp[i] = src[i];
    if (relOff >= 0) {
        int32_t old = 0;
        for (int i = 0; i < 4; ++i) old |= (int32_t)((uint32_t)src[relOff + i] << (8 * i));
        const uint32_t dest = srcAddr + (uint32_t)relOff + 4u + (uint32_t)old;
        const int32_t fixd = (int32_t)(dest - (trampAddr + (uint32_t)relOff + 4u));
        for (int i = 0; i < 4; ++i) tramp[relOff + i] = (unsigned char)((uint32_t)fixd >> (8 * i));
    }
    tramp[len] = 0xE9;
    const int32_t back = (int32_t)((srcAddr + (uint32_t)len) - (trampAddr + (uint32_t)len + 5u));
    for (int i = 0; i < 4; ++i) tramp[len + 1 + i] = (unsigned char)((uint32_t)back >> (8 * i));
    return len + 5;
}

} // namespace hk
