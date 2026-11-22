#include "../src/hookutil.h"
#include <cstdio>
#include <cstring>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main()
{
    int rel;
    { const unsigned char p[] = { 0x8B,0xFF,0x55,0x8B,0xEC,0x83,0xE4,0xF8 };   // mov edi,edi; push ebp; mov ebp,esp; and esp,-8
      CHECK(hk::PrologueLength(p, &rel) == 5 && rel == -1); }
    { const unsigned char p[] = { 0x55,0x8B,0xEC,0x83,0xEC,0x20,0x53 };         // push ebp; mov ebp,esp; sub esp,32
      CHECK(hk::PrologueLength(p, &rel) == 6); }
    { const unsigned char p[] = { 0x55,0x8B,0xEC,0x81,0xEC,0x00,0x01,0,0 };     // ... sub esp,imm32
      CHECK(hk::PrologueLength(p, &rel) == 9); }
    { const unsigned char p[] = { 0xE9,0x10,0x00,0x00,0x00,0x90 };              // foreign jmp rel32
      CHECK(hk::PrologueLength(p, &rel) == 5 && rel == 1); }
    { const unsigned char p[] = { 0x55,0x8B,0x45,0x08,0x5D };                   // push ebp; mov eax,[ebp+8]; pop ebp
      CHECK(hk::PrologueLength(p, &rel) == 5); }
    { const unsigned char p[] = { 0x83,0xEC,0x20,0x8B,0x44,0x24,0x48,0x8B };    // DXVK d3d11: sub esp,32; mov eax,[esp+48h]
      CHECK(hk::PrologueLength(p, &rel) == 7 && rel == -1); }
    { const unsigned char p[] = { 0x8B,0x0D,0x78,0x56,0x34,0x12 };              // mov ecx,[0x12345678]
      CHECK(hk::PrologueLength(p, &rel) == 6); }
    { const unsigned char p[] = { 0x8B,0x84,0x24,0x00,0x01,0x00,0x00 };         // mov eax,[esp+100h]
      CHECK(hk::PrologueLength(p, &rel) == 7); }
    { const unsigned char p[] = { 0xC7,0x45,0xFC,0x00,0x00,0x00,0x00 };         // mov dword [ebp-4],0
      CHECK(hk::PrologueLength(p, &rel) == 7); }
    { const unsigned char p[] = { 0x8B,0x04,0x8D,0x78,0x56,0x34,0x12 };         // mov eax,[ecx*4+0x12345678] (SIB, no base)
      CHECK(hk::PrologueLength(p, &rel) == 7); }
    { const unsigned char p[] = { 0xEB,0x05,0x90,0x90,0x90,0x90 };              // short jmp -> unsupported
      CHECK(hk::PrologueLength(p, &rel) == 0); }
    { const unsigned char p[] = { 0xFF,0x25,0x78,0x56,0x34,0x12 };              // jmp [0x12345678]
      CHECK(hk::PrologueLength(p, &rel) == 6); }
    { const unsigned char p[] = { 0xF3,0x0F,0x1E,0xFB,0x55,0x89,0xE5,0x83,0xEC,0x28 }; // Wine d3d11: endbr32; push ebp; mov ebp,esp
      CHECK(hk::PrologueLength(p, &rel) == 5 && rel == -1); }
    { const unsigned char p[] = { 0xF3,0x0F,0x1F,0xFB,0x55 };                   // F3 0F not followed by 1E FB -> unsupported
      CHECK(hk::PrologueLength(p, &rel) == 0); }
    // trampoline for a plain prologue: copied bytes + jmp back
    { const unsigned char src[] = { 0x8B,0xFF,0x55,0x8B,0xEC,0xAA,0xBB };
      unsigned char t[16] = {};
      size_t n = hk::BuildTrampoline(src, 0x1000, t, 0x5000, 5, -1);
      CHECK(n == 10 && memcmp(t, src, 5) == 0 && t[5] == 0xE9);
      int32_t back; memcpy(&back, t + 6, 4);
      CHECK((uint32_t)(0x5000 + 5 + 5 + back) == 0x1005); }
    // trampoline for a foreign jmp rel32: the jump must still land on the same absolute destination
    { const unsigned char src[] = { 0xE9,0x00,0x01,0x00,0x00 };                 // at 0x1000 -> dest 0x1000+5+0x100 = 0x1105
      unsigned char t[16] = {};
      hk::BuildTrampoline(src, 0x1000, t, 0x9000, 5, 1);
      int32_t r; memcpy(&r, t + 1, 4);
      CHECK((uint32_t)(0x9000 + 5 + r) == 0x1105); }
    std::printf(fails ? "hookutil tests FAILED (%d)\n" : "hookutil tests passed\n", fails);
    return fails ? 1 : 0;
}
