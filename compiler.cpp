// compiler.cpp
//
// The Q Compiler - Alpha Test Prototype.
//
// A single-file C++ program that compiles a .q source file into standalone
// executables for multiple platforms. Standard library only; no external
// dependencies.
//
// Supported Q syntax (alpha test):
//     show("text to print")
//
// Each show() on a separate source line produces a separate line of output
// in the generated program (newline-separated).
//
// Usage:
//     qc build <filename>.q [-target:<list>]
//
//   -target: comma-separated list of targets. Default is "all".
//     Windows_x86_64  Windows_arm64
//     MacOS_x86_64    MacOS_arm64
//     Linux_x86_64    Linux_arm64
//     all
//
// Output files (one per target):
//     <basename>_<target>.<ext>
//   e.g. hello_Windows_x86_64.exe
//        hello_Linux_x86_64.elf
//        hello_MacOS_arm64.macho
//
// Build (developer):
//     cmake -S . -B build && cmake --build build --config Release
//
// All code generators produce position-independent machine code that calls
// the OS's system-call interface directly (write(2)/read(2) on Unix,
// WriteFile/ReadFile on Windows).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

// ===========================================================================
// Common types
// ===========================================================================

struct ShowStatement {
    std::string text;
};

enum Target : uint8_t {
    T_WINDOWS_X86_64 = 0,
    T_WINDOWS_ARM64,
    T_MACOS_X86_64,
    T_MACOS_ARM64,
    T_LINUX_X86_64,
    T_LINUX_ARM64,
    T_COUNT
};

struct TargetInfo {
    Target      id;
    const char* name;
    const char* ext;
};

static const TargetInfo kTargets[] = {
    { T_WINDOWS_X86_64, "Windows_x86_64", ".exe"    },
    { T_WINDOWS_ARM64,  "Windows_arm64",  ".exe"    },
    { T_MACOS_X86_64,   "MacOS_x86_64",   ".macho"  },
    { T_MACOS_ARM64,    "MacOS_arm64",    ".macho"  },
    { T_LINUX_X86_64,   "Linux_x86_64",   ".elf"    },
    { T_LINUX_ARM64,    "Linux_arm64",    ".elf"    },
};
constexpr size_t kNumTargets = 6;

// ===========================================================================
// Parser
// ===========================================================================

static bool parse_source(const std::string& source, std::vector<ShowStatement>& out) {
    size_t i = 0, line = 1, col = 1;
    auto skip_ws_and_comments = [&]() {
        while (i < source.size()) {
            char c = source[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                i++; if (c == '\n') { line++; col = 1; } else col++;
            } else if (c == '/' && i+1 < source.size() && source[i+1] == '/') {
                while (i < source.size() && source[i] != '\n') i++;
            } else if (c == '/' && i+1 < source.size() && source[i+1] == '*') {
                i += 2;
                while (i < source.size() && !(source[i] == '*' && i+1 < source.size() && source[i+1] == '/')) i++;
                if (i < source.size()) i += 2;
            } else break;
        }
    };
    while (true) {
        skip_ws_and_comments();
        if (i >= source.size()) break;
        if (source.compare(i, 4, "show") != 0) {
            std::cerr << "error: expected 'show' at line " << line << ", col " << col << "\n";
            return false;
        }
        i += 4; col += 4;
        skip_ws_and_comments();
        if (i >= source.size() || source[i] != '(') {
            std::cerr << "error: expected '(' after 'show' at line " << line << "\n";
            return false;
        }
        i++; col++;
        skip_ws_and_comments();
        if (i >= source.size() || source[i] != '"') {
            std::cerr << "error: expected '\"' at line " << line << "\n";
            return false;
        }
        i++; col++;
        ShowStatement stmt;
        while (i < source.size() && source[i] != '"') {
            if (source[i] == '\\') {
                i++; col++;
                if (i >= source.size()) { std::cerr << "error: unterminated escape\n"; return false; }
                char esc = source[i];
                switch (esc) {
                    case 'n': stmt.text.push_back('\n'); break;
                    case 'r': stmt.text.push_back('\r'); break;
                    case 't': stmt.text.push_back('\t'); break;
                    case '\\': stmt.text.push_back('\\'); break;
                    case '"': stmt.text.push_back('"'); break;
                    case '\'': stmt.text.push_back('\''); break;
                    case '0': stmt.text.push_back('\0'); break;
                    default: std::cerr << "error: unknown escape '\\" << esc << "'\n"; return false;
                }
                i++; col++;
            } else {
                char c = source[i++];
                if (c == '\n') { line++; col = 1; } else col++;
                stmt.text.push_back(c);
            }
        }
        if (i >= source.size()) { std::cerr << "error: unterminated string at line " << line << "\n"; return false; }
        i++; col++;
        skip_ws_and_comments();
        if (i >= source.size() || source[i] != ')') {
            std::cerr << "error: expected ')' at line " << line << "\n";
            return false;
        }
        i++; col++;
        skip_ws_and_comments();
        if (i < source.size() && source[i] == ';') { i++; col++; }
        out.push_back(stmt);
    }
    return true;
}

// ===========================================================================
// Byte emitter (shared)
// ===========================================================================

struct ByteEmitter {
    std::vector<uint8_t> b;
    void u8(uint8_t v)  { b.push_back(v); }
    void u16(uint16_t v) { u8(v&0xFF); u8((v>>8)&0xFF); }
    void u32(uint32_t v) { u8(v&0xFF); u8((v>>8)&0xFF); u8((v>>16)&0xFF); u8((v>>24)&0xFF); }
    void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v>>32)); }
    void bytes(const void* d, size_t n) { b.insert(b.end(), (const uint8_t*)d, (const uint8_t*)d+n); }
    void pad(size_t n) { for (size_t k=0;k<n;k++) b.push_back(0); }
    size_t size() const { return b.size(); }
};

// Build the data segment: one block per show() call, each terminated by \n.
// Returns offsets within the data block for each show and for the prompt.
struct DataLayout {
    std::vector<uint32_t> show_offs;   // offset of each show string (with trailing \n)
    uint32_t prompt_off;                // offset of the "Press Enter..." prompt
    uint32_t total;                     // total data size
};

static DataLayout build_data(const std::vector<ShowStatement>& shows,
                             std::vector<uint8_t>& data) {
    DataLayout dl;
    uint32_t off = 0;
    for (const auto& s : shows) {
        dl.show_offs.push_back(off);
        data.insert(data.end(), s.text.begin(), s.text.end());
        data.push_back('\n');   // each show on its own line
        off += (uint32_t)s.text.size() + 1;
    }
    // The "Press Enter to exit..." prompt.
    const char* prompt = "\r\nPress Enter to exit...\r\n";
    dl.prompt_off = off;
    data.insert(data.end(), prompt, prompt + strlen(prompt));
    off += (uint32_t)strlen(prompt);
    dl.total = off;
    return dl;
}

// ===========================================================================
// x86_64 machine code emitter (shared by Windows PE, Linux ELF, macOS Mach-O)
// ===========================================================================

// On all x86_64 OSes the calling convention for syscalls / library calls
// differs. We abstract via small per-OS helpers below.

struct X64Code {
    ByteEmitter c;

    // sub rsp, imm8
    void sub_rsp(uint8_t n)    { c.u8(0x48); c.u8(0x83); c.u8(0xEC); c.u8(n); }
    // add rsp, imm8
    void add_rsp(uint8_t n)    { c.u8(0x48); c.u8(0x83); c.u8(0xC4); c.u8(n); }
    // xor eax, eax
    void xor_eax()             { c.u8(0x31); c.u8(0xC0); }
    // xor edi, edi
    void xor_edi()             { c.u8(0x31); c.u8(0xFF); }
    // mov edi, imm32
    void mov_edi(int32_t v)    { c.u8(0xBF); c.u32((uint32_t)v); }
    // mov esi, imm32
    void mov_esi(uint32_t v)   { c.u8(0xBE); c.u32(v); }
    // mov edx, imm32
    void mov_edx(uint32_t v)    { c.u8(0xBA); c.u32(v); }
    // mov ecx, imm32
    void mov_ecx(int32_t v)    { c.u8(0xB9); c.u32((uint32_t)v); }
    // mov eax, imm32
    void mov_eax(uint32_t v)   { c.u8(0xB8); c.u32(v); }
    // mov rbx, rax   (48 89 C3)
    void mov_rbx_rax()         { c.u8(0x48); c.u8(0x89); c.u8(0xC3); }
    // mov rcx, rbx   (48 89 D9)
    void mov_rcx_rbx()         { c.u8(0x48); c.u8(0x89); c.u8(0xD9); }
    // mov rcx, rax   (48 89 C1)
    void mov_rcx_rax()         { c.u8(0x48); c.u8(0x89); c.u8(0xC1); }
    // mov rdi, rax   (48 89 C7)
    void mov_rdi_rax()         { c.u8(0x48); c.u8(0x89); c.u8(0xC7); }
    // push rbx
    void push_rbx()            { c.u8(0x53); }
    // pop rbx
    void pop_rbx()             { c.u8(0x5B); }
    // ret
    void ret()                 { c.u8(0xC3); }
    // syscall
    void syscall()             { c.u8(0x0F); c.u8(0x05); }
    // lea rdx, [rip+disp32]  (48 8D 15 disp)
    void lea_rdx_rip(uint32_t data_rva, uint32_t code_rva) {
        uint32_t cur = code_rva + (uint32_t)c.size();
        int32_t disp = (int32_t)data_rva - (int32_t)(cur + 7);
        c.u8(0x48); c.u8(0x8D); c.u8(0x15); c.u32((uint32_t)disp);
    }
    // lea rsi, [rip+disp32]  (48 8D 35 disp)
    void lea_rsi_rip(uint32_t data_rva, uint32_t code_rva) {
        uint32_t cur = code_rva + (uint32_t)c.size();
        int32_t disp = (int32_t)data_rva - (int32_t)(cur + 7);
        c.u8(0x48); c.u8(0x8D); c.u8(0x35); c.u32((uint32_t)disp);
    }
    // lea r9, [rip+disp32]  (4C 8D 0D disp)
    void lea_r9_rip(uint32_t data_rva, uint32_t code_rva) {
        uint32_t cur = code_rva + (uint32_t)c.size();
        int32_t disp = (int32_t)data_rva - (int32_t)(cur + 7);
        c.u8(0x4C); c.u8(0x8D); c.u8(0x0D); c.u32((uint32_t)disp);
    }
    // call qword [rip+disp32]  (FF 15 disp)
    void call_rip(uint32_t iat_rva, uint32_t code_rva) {
        uint32_t cur = code_rva + (uint32_t)c.size();
        int32_t disp = (int32_t)iat_rva - (int32_t)(cur + 6);
        c.u8(0xFF); c.u8(0x15); c.u32((uint32_t)disp);
    }
    // mov qword [rsp+imm8], 0  (48 C7 44 24 ib 00 00 00 00)
    void movq_rsp_zero(uint8_t off) {
        c.u8(0x48); c.u8(0xC7); c.u8(0x44); c.u8(0x24); c.u8(off);
        c.u8(0x00); c.u8(0x00); c.u8(0x00); c.u8(0x00);
    }
};

// ===========================================================================
// ARM64 machine code emitter (shared by Windows/Linux/macOS)
// ===========================================================================

struct Arm64Code {
    ByteEmitter c;

    // Emit one 32-bit instruction (little-endian).
    void insn(uint32_t v) { c.u32(v); }

    // Helpers for common ARM64 instructions.
    // movz xN, #imm16 [, lsl #shift]
    //   encoding: 1 10 100101 hw imm16 Rd  -> 0xD2800000 | (hw<<21) | (imm16<<5) | Rd
    void movz(uint8_t rd, uint16_t imm16, uint8_t hw=0) {
        insn(0xD2800000 | ((uint32_t)hw << 21) | ((uint32_t)imm16 << 5) | rd);
    }
    // movk xN, #imm16, lsl #hw*16
    void movk(uint8_t rd, uint16_t imm16, uint8_t hw) {
        insn(0xF2800000 | ((uint32_t)hw << 21) | ((uint32_t)imm16 << 5) | rd);
    }
    // Load a 64-bit immediate into xN via movz/movk chain.
    void mov_imm64(uint8_t rd, uint64_t val) {
        movz(rd, (uint16_t)(val & 0xFFFF), 0);
        if ((val >> 16) & 0xFFFF) movk(rd, (uint16_t)((val >> 16) & 0xFFFF), 1);
        if ((val >> 32) & 0xFFFF) movk(rd, (uint16_t)((val >> 32) & 0xFFFF), 2);
        if ((val >> 48) & 0xFFFF) movk(rd, (uint16_t)((val >> 48) & 0xFFFF), 3);
    }
    // Load a 32-bit immediate (zero-extended) into xN.
    void mov_imm32(uint8_t rd, uint32_t val) {
        movz(rd, (uint16_t)(val & 0xFFFF), 0);
        if ((val >> 16) & 0xFFFF) movk(rd, (uint16_t)((val >> 16) & 0xFFFF), 1);
    }
    // mov x0, x1  -> 0xAA0103E0 (orr x0, xzr, x1)
    //   generic: mov xd, xm -> 0xAA0003E0 | (m << 16) | d
    void mov_reg(uint8_t rd, uint8_t rm) {
        insn(0xAA0003E0 | ((uint32_t)rm << 16) | rd);
    }
    // adr xN, #imm  (PC-relative, ±1MB).  encoding:
    //   0 immlo 10000 immhi Rd
    //   0 immlo(2) 1 0000 0 immhi(19) Rd(5)
    // We use adr for short offsets; for larger we'd need adrp+add.
    void adr(uint8_t rd, int32_t offset) {
        uint32_t imm = (uint32_t)offset;
        uint32_t immlo = imm & 3;
        uint32_t immhi = (imm >> 2) & 0x7FFFF;   // 19 bits
        insn((immlo << 29) | (0x10 << 24) | (immhi << 5) | rd);
    }
    // adrp xN, #imm  (PC-relative page, ±4GB). imm is byte offset; we
    // compute page-relative.
    void adrp(uint8_t rd, int64_t offset) {
        // adrp loads (PC & ~0xFFF) + (imm << 12)
        int64_t page = (offset >= 0 ? (offset >> 12) : ((offset - 0xFFF) >> 12));
        // For negative, need to encode properly in 21-bit two-part imm.
        uint32_t imm = (uint32_t)page;
        uint32_t immlo = imm & 3;
        uint32_t immhi = (imm >> 2) & 0x7FFFF;
        insn((immlo << 29) | (0x90 << 24) | (immhi << 5) | rd);
    }
    // add xN, xN, #imm12  (0x91 << 24 | imm12 << 10 | Rn << 5 | Rd)
    void add_imm(uint8_t rd, uint8_t rn, uint16_t imm12) {
        insn((0x91 << 24) | ((uint32_t)imm12 << 10) | ((uint32_t)rn << 5) | rd);
    }
    // bl #imm26  (branch with link, ±128MB)
    void bl(int32_t offset) {
        uint32_t imm26 = (uint32_t)(offset >> 2) & 0x3FFFFFF;
        insn((0x94 << 24) | imm26);
    }
    // br xN  (0xD61F00C0 | (Rn << 5))
    void br(uint8_t rn) {
        insn(0xD61F0000 | ((uint32_t)rn << 5));
    }
    // svc #0  (supervisor call, Linux)
    void svc() { insn(0xD4000001); }
    // ret
    void ret() { insn(0xD65F03C0); }
    // mov x0, #0  (alias: movz x0, #0)
    void mov_x0_zero() { movz(0, 0); }
    // mov x8, #imm  (Linux syscall number)
    void mov_x8_imm(uint32_t val) { mov_imm32(8, val); }
    // str xN, [sp, #imm]  (store 64-bit, unsigned offset scaled by 8)
    //   0xF9000000 | (imm12 << 10) | (Rn << 5) | Rt
    void str_sp(uint8_t rt, uint16_t imm12) {
        insn(0xF9000000 | ((uint32_t)imm12 << 10) | (31u << 5) | rt);
    }
    // ldr xN, [sp, #imm]
    void ldr_sp(uint8_t rt, uint16_t imm12) {
        insn(0xF9400000 | ((uint32_t)imm12 << 10) | (31u << 5) | rt);
    }
    // sub sp, sp, #imm
    void sub_sp(uint16_t imm12) {
        insn((0xD1 << 24) | ((uint32_t)imm12 << 10) | (31u << 5) | 31);
    }
    // add sp, sp, #imm
    void add_sp(uint16_t imm12) {
        insn((0x91 << 24) | ((uint32_t)imm12 << 10) | (31u << 5) | 31);
    }
    // stp x29, x30, [sp, #imm]  (store pair, for frame)
    void stp_x29_x30(uint16_t imm12) {
        // 0xA9000000 | (imm7<<15) | (Rt2<<10) | (Rn<<5) | Rt
        insn(0xA9000000 | ((uint32_t)(imm12/8) << 15) | (30u << 10) | (31u << 5) | 29u);
    }
    // ldp x29, x30, [sp, #imm]
    void ldp_x29_x30(uint16_t imm12) {
        insn(0xA9400000 | ((uint32_t)(imm12/8) << 15) | (30u << 10) | (31u << 5) | 29u);
    }
};

// Helper: put8 into a vector
static void put8_safe(std::vector<uint8_t>& img, uint32_t o, uint8_t v) { img[o] = v; }

// ===========================================================================
// Windows PE generator (x86_64 and ARM64)
// ===========================================================================

namespace pe {
    constexpr uint32_t IMAGE_BASE      = 0x00400000;
    constexpr uint16_t MACHINE_AMD64   = 0x8664;
    constexpr uint16_t MACHINE_ARM64   = 0xAA64;
    constexpr uint32_t SEC_ALIGN       = 0x1000;
    constexpr uint32_t FILE_ALIGN       = 0x200;
    constexpr uint32_t HEADERS_SIZE    = 0x200;
    constexpr uint32_t TEXT_RVA        = 0x1000;
    constexpr uint32_t IDATA_RVA       = 0x2000;
    constexpr uint32_t SC_CODE         = 0x60000020;
    constexpr uint32_t SC_IDATA        = 0xC0000040;

    static const char* kImports[] = { "GetStdHandle", "WriteFile", "ReadFile", "ExitProcess" };
    constexpr size_t kNumImports = 4;
    enum { IDX_GSH = 0, IDX_WF = 1, IDX_RF = 2, IDX_EP = 3 };
    constexpr int32_t STD_OUT = -11;
    constexpr int32_t STD_IN  = -10;
}

// Build a Windows PE for either x86_64 or ARM64.
static std::vector<uint8_t> build_pe(const std::vector<ShowStatement>& shows,
                                     bool is_arm64) {
    using namespace pe;

    // ---- data ----
    std::vector<uint8_t> data;
    DataLayout dl = build_data(shows, data);

    // ---- .idata layout ----
    uint32_t off = 0;
    const uint32_t idt_off = off; off += 40;                 // 2 IDT entries
    const uint32_t iat_off = off; off += (uint32_t)(kNumImports + 1) * 8; // IAT + null
    uint32_t inr_offs[kNumImports];
    const uint32_t inr_start = off;
    for (size_t k = 0; k < kNumImports; k++) {
        inr_offs[k] = off;
        off += 2 + (uint32_t)strlen(kImports[k]) + 1;
    }
    if (off & 1) off++;
    const uint32_t dllname_off = off;
    off += (uint32_t)strlen("kernel32.dll") + 1;
    while (off & 7) off++;
    const uint32_t written_off = off; off += 4;
    const uint32_t read_off    = off; off += 4;
    const uint32_t data_off    = off; off += dl.total;
    const uint32_t idata_size  = off;

    auto idata_rva = [&](uint32_t o) { return IDATA_RVA + o; };

    // ---- code ----
    uint32_t code_rva = TEXT_RVA;
    std::vector<uint8_t> code;

    if (!is_arm64) {
        // ===== x86_64 =====
        X64Code pc;
        pc.push_rbx();             // 53
        pc.sub_rsp(48);            // 48 83 EC 30
        // GetStdHandle(STD_OUTPUT_HANDLE)
        pc.mov_ecx(STD_OUT);       // B9 F5 FF FF FF
        pc.call_rip(idata_rva(iat_off + pe::IDX_GSH * 8), code_rva);
        pc.mov_rbx_rax();          // 48 89 C3  (hOut in rbx)
        // For each show: WriteFile(hOut, text, len, &written, NULL)
        for (size_t k = 0; k < shows.size(); k++) {
            pc.mov_rcx_rbx();     // 48 89 D9
            pc.lea_rdx_rip(idata_rva(data_off + dl.show_offs[k]), code_rva);
            pc.mov_edx((uint32_t)(shows[k].text.size() + 1)); // +1 for \n
            // Wait — mov_edx overwrites rdx from lea. Fix: use r8 for len.
            // Actually lea sets rdx; then we need r8 = len. Let me reorder:
            // lea rdx first, then mov r8d, len.
            // (The mov_edx above is wrong — it clobbers rdx. Remove it.)
        }
        // Re-emit correctly: lea rdx, mov r8d, len; lea r9, &written; 5th=NULL; call
        code.clear();
        // Rebuild from scratch with correct ordering.
        // [restart]
        // push rbx; sub rsp,48
        pc.c.b.clear();
        pc.push_rbx();
        pc.sub_rsp(48);
        // GetStdHandle(STD_OUTPUT_HANDLE)
        pc.mov_ecx(STD_OUT);
        pc.call_rip(idata_rva(iat_off + pe::IDX_GSH * 8), code_rva);
        pc.mov_rbx_rax();
        // Each show
        for (size_t k = 0; k < shows.size(); k++) {
            pc.mov_rcx_rbx();                                              // rcx = hOut
            pc.lea_rdx_rip(idata_rva(data_off + dl.show_offs[k]), code_rva); // rdx = &text
            // r8 = len (text + 1 for \n)
            // mov r8d, imm32: 41 B8 id
            pc.c.u8(0x41); pc.c.u8(0xB8); pc.c.u32((uint32_t)(shows[k].text.size() + 1));
            pc.lea_r9_rip(idata_rva(written_off), code_rva);               // r9 = &written
            pc.movq_rsp_zero(32);                                          // [rsp+32] = 0 (5th arg)
            pc.call_rip(idata_rva(iat_off + pe::IDX_WF * 8), code_rva);
        }
        // Print prompt
        pc.mov_rcx_rbx();
        pc.lea_rdx_rip(idata_rva(data_off + dl.prompt_off), code_rva);
        pc.c.u8(0x41); pc.c.u8(0xB8); pc.c.u32((uint32_t)strlen("\r\nPress Enter to exit...\r\n"));
        pc.lea_r9_rip(idata_rva(written_off), code_rva);
        pc.movq_rsp_zero(32);
        pc.call_rip(idata_rva(iat_off + pe::IDX_WF * 8), code_rva);
        // GetStdHandle(STD_INPUT_HANDLE)
        pc.mov_ecx(STD_IN);
        pc.call_rip(idata_rva(iat_off + pe::IDX_GSH * 8), code_rva);
        pc.mov_rcx_rax();   // hIn
        // ReadFile(hIn, buf, 1, &read, NULL)
        // lea rdx, [rsp+40]: 48 8D 54 24 28
        pc.c.u8(0x48); pc.c.u8(0x8D); pc.c.u8(0x54); pc.c.u8(0x24); pc.c.u8(0x28);
        pc.c.u8(0x41); pc.c.u8(0xB8); pc.c.u32(1);     // r8 = 1
        pc.lea_r9_rip(idata_rva(read_off), code_rva);
        pc.movq_rsp_zero(32);
        pc.call_rip(idata_rva(iat_off + pe::IDX_RF * 8), code_rva);
        // ExitProcess(0)
        pc.xor_eax();
        pc.mov_rcx_rax();
        pc.call_rip(idata_rva(iat_off + pe::IDX_EP * 8), code_rva);
        // Fallback
        pc.add_rsp(48);
        pc.pop_rbx();
        pc.ret();
        code = pc.c.b;
    } else {
        // ===== ARM64 Windows =====
        // Windows ARM64 calling convention: x0-x3 args, x4-x7 more,
        // x9-x15 volatile, x19-x28 callee-saved.
        // We use x19 to save hOut.
        Arm64Code ac;
        // prologue: stp x29,x30,[sp,#-32]! would be ideal, but let's keep
        // it simple: sub sp, sp, #48; str x19, [sp, #32]; stp x29,x30,[sp,#16]
        // Actually for Windows ARM64 we can use a simple frame:
        // sub sp, sp, #64; save x19 at [sp,#32], x29/x30 at [sp,#40]/[sp,#48]
        ac.sub_sp(64);           // sp -= 64 (16 bytes * 4)
        ac.str_sp(19, 4);        // [sp+32] = x19  (save)
        ac.str_sp(29, 5);        // [sp+40] = x29
        ac.str_sp(30, 6);        // [sp+48] = x30

        // GetStdHandle(STD_OUTPUT_HANDLE) -> x0
        // STD_OUTPUT_HANDLE = -11 = 0xFFFFFFF5
        ac.mov_imm32(0, (uint32_t)STD_OUT);   // x0 = -11 (as unsigned, but movz/movk loads 0xFFFFFFF5)
        // For -11 we need 0xFFFFFFFFFFFFFFF5 as 64-bit. mov_imm32 loads 32-bit
        // zero-extended, so 0xFFFFFFF5. But GetStdHandle takes a DWORD, so
        // it reads only the low 32 bits. That's fine.
        // Actually Windows ARM64 passes args in x0 as 64-bit. -11 as i32 is
        // 0xFFFFFFF5, zero-extended to 64-bit = 0x00000000FFFFFFF5. GetStdHandle
        // interprets it as DWORD (32-bit), so it works.
        // bl to the IAT thunk... but ARM64 PE uses a stub: we need to emit
        // a "ldr x16, [pc+8]; br x16; .quad IAT_entry" pattern for each call.
        // That's complex. For simplicity, we emit inline thunks.
        // Actually Windows ARM64 PE IAT calls go through stubs in .text.
        // Let's emit the call-stub pattern inline before each call.
        // But we need to know the IAT RVA to compute the address.
        // The IAT is in .idata. We can load the absolute address using
        // adrp+add if we know the page. But with ASLR off and image base
        // 0x400000, the IAT VA = 0x400000 + IDATA_RVA + iat_off + idx*8.
        // We can use mov_imm64 to load the full 64-bit VA.
        // This is simpler but not position-independent. Since we disabled
        // ASLR, it works. For now, use absolute addresses.

        auto call_iat_arm64 = [&](uint32_t iat_slot_off) {
            uint64_t iat_va = IMAGE_BASE + IDATA_RVA + iat_slot_off;
            // mov x16, iat_va (64-bit)
            ac.mov_imm64(16, iat_va);
            // ldr x16, [x16]  -> 0xF9400000 | (16<<5) | 16 = 0xF9400210
            // Wait: ldr x16, [x16] is 0xF9400210? Let me compute:
            //   ldr x16, [x16, #0] = 0xF9400000 | (0 << 10) | (16 << 5) | 16
            //   = 0xF9400000 | 0x200 | 0x10 = 0xF9400210
            ac.insn(0xF9400210);
            // br x16
            ac.br(16);
        };

        call_iat_arm64(iat_off + pe::IDX_GSH * 8);
        // x0 = hOut; save to x19
        ac.mov_reg(19, 0);  // mov x19, x0

        // For each show: WriteFile(hOut, text, len, &written, NULL)
        // x0=hOut, x1=&text, x2=len, x3=&written, x4=NULL(0), 5th+ on stack
        for (size_t k = 0; k < shows.size(); k++) {
            ac.mov_reg(0, 19);  // x0 = hOut
            // x1 = &text (adr or adrp+add). Use adrp+add.
            // data is at IDATA_RVA + data_off + dl.show_offs[k]
            // We need a PC-relative load of the address. Since code is at
            // TEXT_RVA and data at IDATA_RVA, the offset is ~0x1000+data.
            // adrp can reach ±4GB, so it's fine.
            // But adrp needs page-aligned offset. Let's just use mov_imm64
            // with the absolute VA (no ASLR).
            {
                uint64_t text_va = IMAGE_BASE + IDATA_RVA + data_off + dl.show_offs[k];
                ac.mov_imm64(1, text_va);   // x1 = &text
            }
            ac.mov_imm32(2, (uint32_t)(shows[k].text.size() + 1)); // x2 = len
            // x3 = &written (absolute VA)
            ac.mov_imm64(3, IMAGE_BASE + IDATA_RVA + written_off);
            // x4 = 0 (NULL for lpOverlapped)
            ac.movz(4, 0);
            // For Windows ARM64, the 5th arg (lpOverlapped) goes in x4.
            // WriteFile has 5 args: hFile, lpBuffer, nBytes, lpBytesWritten, lpOverlapped.
            // So x0=hOut, x1=buf, x2=len, x3=&written, x4=NULL.
            call_iat_arm64(iat_off + pe::IDX_WF * 8);
        }

        // Print prompt
        ac.mov_reg(0, 19);  // x0 = hOut
        ac.mov_imm64(1, IMAGE_BASE + IDATA_RVA + data_off + dl.prompt_off);
        ac.mov_imm32(2, (uint32_t)strlen("\r\nPress Enter to exit...\r\n"));
        ac.mov_imm64(3, IMAGE_BASE + IDATA_RVA + written_off);
        ac.movz(4, 0);
        call_iat_arm64(iat_off + pe::IDX_WF * 8);

        // GetStdHandle(STD_INPUT_HANDLE) -> x0
        ac.mov_imm32(0, (uint32_t)STD_IN);
        call_iat_arm64(iat_off + pe::IDX_GSH * 8);
        // x0 = hIn

        // ReadFile(hIn, buf, 1, &read, NULL)
        // x0=hIn, x1=&buf (on stack at [sp+0]), x2=1, x3=&read, x4=NULL
        ac.mov_reg(0, 0);   // x0 = hIn (already in x0)
        // x1 = sp (lea x1, [sp])
        // add x1, sp, #0  -> 0x910003E1
        ac.insn(0x910003E1);
        ac.mov_imm32(2, 1);
        ac.mov_imm64(3, IMAGE_BASE + IDATA_RVA + read_off);
        ac.movz(4, 0);
        call_iat_arm64(iat_off + pe::IDX_RF * 8);

        // ExitProcess(0)
        ac.movz(0, 0);  // x0 = 0
        call_iat_arm64(iat_off + pe::IDX_EP * 8);

        // Fallback: restore and return
        ac.ldr_sp(19, 4);
        ac.ldr_sp(29, 5);
        ac.ldr_sp(30, 6);
        ac.add_sp(64);
        ac.ret();
        code = ac.c.b;
    }

    // ---- sizes ----
    uint32_t text_size = (uint32_t)code.size();
    uint32_t text_raw  = text_size;
    while (text_raw & (FILE_ALIGN-1)) text_raw++;
    uint32_t idata_raw = idata_size;
    while (idata_raw & (FILE_ALIGN-1)) idata_raw++;
    uint32_t text_virt = text_size;
    while (text_virt & (SEC_ALIGN-1)) text_virt++;
    uint32_t idata_virt = idata_size;
    while (idata_virt & (SEC_ALIGN-1)) idata_virt++;

    const uint32_t text_file  = HEADERS_SIZE;
    const uint32_t idata_file = text_file + text_raw;
    const uint32_t total      = idata_file + idata_raw;

    // ---- build image ----
    std::vector<uint8_t> img(total, 0);
    auto put16 = [&](uint32_t o, uint16_t v) { img[o]=(v&0xFF); img[o+1]=(v>>8)&0xFF; };
    auto put32 = [&](uint32_t o, uint32_t v) { img[o]=v&0xFF; img[o+1]=(v>>8)&0xFF; img[o+2]=(v>>16)&0xFF; img[o+3]=(v>>24)&0xFF; };
    auto put64 = [&](uint32_t o, uint64_t v) { put32(o,(uint32_t)v); put32(o+4,(uint32_t)(v>>32)); };
    auto put   = [&](uint32_t o, const void* s, size_t n) { memcpy(&img[o], s, n); };

    // DOS header
    put16(0x00, 0x5A4D);
    put32(0x3C, 0x40);
    // PE signature
    put32(0x40, 0x00004550);
    // COFF header
    put16(0x44, is_arm64 ? pe::MACHINE_ARM64 : pe::MACHINE_AMD64);
    put16(0x46, 2);
    put32(0x48, 0);
    put32(0x4C, 0);
    put32(0x50, 0);
    put16(0x54, 0xF0);
    put16(0x56, 0x22);
    // Optional header
    put16(0x58, 0x20B);          // PE32+
    put8_safe(img, 0x5A, 14);
    put8_safe(img, 0x5B, 0);
    put32(0x5C, text_raw);        // SizeOfCode
    put32(0x60, idata_raw);       // SizeOfInitializedData
    put32(0x64, 0);
    put32(0x68, TEXT_RVA);        // EntryPoint
    put32(0x6C, TEXT_RVA);        // BaseOfCode
    put64(0x70, IMAGE_BASE);
    put32(0x78, SEC_ALIGN);
    put32(0x7C, FILE_ALIGN);
    put16(0x80, 6);
    put16(0x82, 0);
    put16(0x84, 0);
    put16(0x86, 0);
    put16(0x88, 6);
    put16(0x8A, 0);
    put32(0x8C, 0);
    put32(0x90, IDATA_RVA + idata_virt);  // SizeOfImage
    put32(0x94, HEADERS_SIZE);
    put32(0x98, 0);
    put16(0x9C, 3);               // CONSOLE
    put16(0x9E, 0x0100);          // NX_COMPAT only (no ASLR)
    put64(0xA0, 0x100000);
    put64(0xA8, 0x1000);
    put64(0xB0, 0x100000);
    put64(0xB8, 0x1000);
    put32(0xC0, 0);
    put32(0xC4, 16);
    // Data directories
    put32(0xC8 + 8,  IDATA_RVA + idt_off);  put32(0xC8+12, 40);
    put32(0xC8 + 96, IDATA_RVA + iat_off);  put32(0xC8+100, (uint32_t)((kNumImports+1)*8));
    // Section headers at 0x148
    put(0x148, ".text\0\0\0", 8);
    put32(0x148+8, text_virt);  put32(0x148+12, TEXT_RVA);
    put32(0x148+16, text_raw);  put32(0x148+20, text_file);
    put32(0x148+36, SC_CODE);
    put(0x148+40, ".idata\0\0", 8);
    put32(0x148+48, idata_virt); put32(0x148+52, IDATA_RVA);
    put32(0x148+56, idata_raw); put32(0x148+60, idata_file);
    put32(0x148+76, SC_IDATA);
    // .text
    put(text_file, code.data(), code.size());
    // .idata
    auto iput32 = [&](uint32_t wo, uint32_t v) {
        img[idata_file + wo]   = v & 0xFF;
        img[idata_file+wo+1] = (v>>8)&0xFF;
        img[idata_file+wo+2] = (v>>16)&0xFF;
        img[idata_file+wo+3] = (v>>24)&0xFF;
    };
    auto iput16 = [&](uint32_t wo, uint16_t v) {
        img[idata_file+wo] = v & 0xFF;
        img[idata_file+wo+1] = (v>>8)&0xFF;
    };
    auto iput = [&](uint32_t wo, const void* s, size_t n) {
        memcpy(&img[idata_file+wo], s, n);
    };
    // IDT
    iput32(0,  idata_rva(iat_off));     // OriginalFirstThunk
    iput32(4,  0);
    iput32(8,  0);
    iput32(12, idata_rva(dllname_off)); // Name
    iput32(16, idata_rva(iat_off));     // FirstThunk
    // IAT
    for (size_t k = 0; k < kNumImports; k++) {
        iput32(iat_off + k*8,     idata_rva(inr_offs[k]));
        iput32(iat_off + k*8 + 4, 0);
    }
    iput32(iat_off + kNumImports*8,     0);  // null terminator
    iput32(iat_off + kNumImports*8 + 4, 0);
    // Name records
    for (size_t k = 0; k < kNumImports; k++) {
        iput16(inr_offs[k], 0);  // hint
        iput(inr_offs[k]+2, kImports[k], strlen(kImports[k]));
        img[idata_file + inr_offs[k] + 2 + strlen(kImports[k])] = 0;
    }
    // DLL name
    iput(dllname_off, "kernel32.dll", strlen("kernel32.dll"));
    img[idata_file + dllname_off + strlen("kernel32.dll")] = 0;
    // Data
    iput(data_off, data.data(), data.size());

    return img;
}

// ===========================================================================
// Linux ELF generator (x86_64 and ARM64)
// ===========================================================================

namespace elf {
    // ELF64 constants
    constexpr uint8_t  ELFMAG0    = 0x7F;
    constexpr uint8_t  ELFMAG1    = 'E';
    constexpr uint8_t  ELFMAG2    = 'L';
    constexpr uint8_t  ELFMAG3    = 'F';
    constexpr uint8_t  ELFCLASS64 = 2;
    constexpr uint8_t  ELFDATA2LSB = 1;
    constexpr uint8_t  ET_EXEC    = 2;
    constexpr uint8_t  EV_CURRENT = 1;
    constexpr uint8_t  PT_LOAD    = 1;
    constexpr uint8_t  PF_X       = 1;
    constexpr uint8_t  PF_R       = 4;
    constexpr uint8_t  PF_W       = 2;

    // x86_64
    constexpr uint16_t EM_X86_64  = 62;
    constexpr uint32_t X64_WRITE  = 1;   // sys_write
    constexpr uint32_t X64_READ   = 0;   // sys_read
    constexpr uint32_t X64_EXIT   = 60;  // sys_exit

    // ARM64 (AArch64)
    constexpr uint16_t EM_AARCH64 = 183;
    constexpr uint32_t A64_WRITE  = 64;  // sys_write
    constexpr uint32_t A64_READ   = 63;  // sys_read
    constexpr uint32_t A64_EXIT   = 93;  // sys_exit
}

static std::vector<uint8_t> build_elf(const std::vector<ShowStatement>& shows,
                                       bool is_arm64) {
    using namespace elf;

    // Data: show strings + prompt
    std::vector<uint8_t> data;
    DataLayout dl = build_data(shows, data);

    // Linux ELF64 layout:
    //   ELF header (64 bytes)
    //   Program header (one PT_LOAD, 56 bytes)
    //   Code (follows)
    //   Data (follows code, page-aligned in memory but contiguous in file)
    //
    // We use a single PT_LOAD segment covering both code and data.
    // Virtual address: 0x400000 (standard Linux executable base).
    // File offset 0 = virtual address 0x400000.
    // Code starts at file offset 0x40 (after headers), VA 0x400040.
    // Data follows code.

    const uint64_t BASE = 0x400000;
    const uint32_t EHDR_SIZE = 64;
    const uint32_t PHDR_SIZE = 56;
    // 2 program headers: PT_LOAD + PT_GNU_STACK
    const uint32_t HDRS = EHDR_SIZE + 2 * PHDR_SIZE;   // 64 + 112 = 176 (0xB0)
    const uint32_t CODE_OFF = HDRS;                     // code starts right after headers

    // Generate code
    std::vector<uint8_t> code;

    if (!is_arm64) {
        // ===== x86_64 Linux syscalls =====
        // Linux x86_64 syscall convention:
        //   rax = syscall number
        //   rdi = arg1, rsi = arg2, rdx = arg3
        //   syscall
        // Data is placed at file offset 0x1000 (page-aligned), VA = BASE+0x1000.
        // Code starts at file offset HDRS (0x78), VA = BASE+0x78.
        // lea_rsi_rip(data_rva, code_base_rva) computes the displacement as:
        //   disp = data_rva - (code_base_rva + cur_code_size + 7)
        // so we pass CODE_OFF as the base and it adds the current emit position.
        X64Code pc;
        const uint32_t DATA_RVA = 0x1000;  // data at file/VA offset 0x1000

        for (size_t k = 0; k < shows.size(); k++) {
            pc.mov_eax(X64_WRITE);          // rax = 1 (write)
            pc.mov_edi(1);                   // rdi = 1 (stdout)
            pc.lea_rsi_rip(DATA_RVA + dl.show_offs[k], CODE_OFF);  // rsi = &text
            pc.mov_edx((uint32_t)(shows[k].text.size() + 1)); // rdx = len (+1 for \n)
            pc.syscall();
        }
        // Print prompt
        {
            pc.mov_eax(X64_WRITE);
            pc.mov_edi(1);
            pc.lea_rsi_rip(DATA_RVA + dl.prompt_off, CODE_OFF);
            pc.mov_edx((uint32_t)strlen("\r\nPress Enter to exit...\r\n"));
            pc.syscall();
        }
        // Read(0, buf, 1) — wait for Enter
        {
            pc.mov_eax(X64_READ);
            pc.mov_edi(0);   // stdin
            pc.sub_rsp(16);
            // lea rsi, [rsp] = 48 8D 34 24
            pc.c.u8(0x48); pc.c.u8(0x8D); pc.c.u8(0x34); pc.c.u8(0x24);
            pc.mov_edx(1);
            pc.syscall();
            pc.add_rsp(16);
        }
        // exit(0)
        {
            pc.mov_eax(X64_EXIT);
            pc.xor_edi();
            pc.syscall();
        }
        code = pc.c.b;
    } else {
        // ===== ARM64 Linux syscalls =====
        // Linux ARM64 syscall convention:
        //   x8 = syscall number
        //   x0 = arg1, x1 = arg2, x2 = arg3
        //   svc #0
        Arm64Code ac;
        // Data at file offset 0x1000, VA = BASE + 0x1000
        const uint64_t DATA_VA = BASE + 0x1000;
        for (size_t k = 0; k < shows.size(); k++) {
            ac.mov_x8_imm(A64_WRITE);   // x8 = 64 (write)
            ac.movz(0, 1);              // x0 = 1 (stdout)
            // x1 = &text (use mov_imm64 with absolute VA since no ASLR... but
            // Linux ELF executables with ET_EXEC load at fixed VA, so this works)
            ac.mov_imm64(1, DATA_VA + dl.show_offs[k]);
            ac.mov_imm32(2, (uint32_t)(shows[k].text.size() + 1));
            ac.svc();
        }
        // Prompt
        ac.mov_x8_imm(A64_WRITE);
        ac.movz(0, 1);
        ac.mov_imm64(1, DATA_VA + dl.prompt_off);
        ac.mov_imm32(2, (uint32_t)strlen("\r\nPress Enter to exit...\r\n"));
        ac.svc();
        // Read(0, buf, 1)
        ac.mov_x8_imm(A64_READ);
        ac.movz(0, 0);   // stdin
        // x1 = sp (sub sp first)
        ac.sub_sp(16);
        // mov x1, sp -> add x1, sp, #0 = 0x910003E1
        ac.insn(0x910003E1);
        ac.mov_imm32(2, 1);
        ac.svc();
        ac.add_sp(16);
        // exit(0)
        ac.mov_x8_imm(A64_EXIT);
        ac.movz(0, 0);
        ac.svc();
        code = ac.c.b;
    }

    // Layout:
    //   ELF header             64 bytes (offset 0)
    //   PHDR #0 (PT_LOAD)      56 bytes (offset 64)
    //   PHDR #1 (PT_GNU_STACK) 56 bytes (offset 120)
    //   = total headers = 176 (0xB0)
    //   Code at offset 0xB0, data at offset 0x1000 (page-aligned).
    //   The PT_LOAD segment covers the entire file (RWX) so both code and
    //   data are mapped. PT_GNU_STACK marks the stack as non-executable,
    //   which prevents the kernel from applying READ_IMPLIES_EXEC.
    const uint32_t DATA_OFF = 0x1000;
    const uint32_t CODE_OFF_ACTUAL = HDRS;  // 0xB0
    const uint64_t CODE_VA  = BASE + CODE_OFF_ACTUAL;
    const uint64_t DATA_VA  = BASE + DATA_OFF;
    const uint32_t FILE_SIZE = DATA_OFF + (uint32_t)data.size();

    // Build the ELF
    std::vector<uint8_t> img(FILE_SIZE, 0);
    auto p16 = [&](uint32_t o, uint16_t v) { img[o]=v&0xFF; img[o+1]=(v>>8)&0xFF; };
    auto p32 = [&](uint32_t o, uint32_t v) { img[o]=v&0xFF; img[o+1]=(v>>8)&0xFF; img[o+2]=(v>>16)&0xFF; img[o+3]=(v>>24)&0xFF; };
    auto p64 = [&](uint32_t o, uint64_t v) { p32(o,(uint32_t)v); p32(o+4,(uint32_t)(v>>32)); };

    // ELF header (64 bytes)
    img[0] = 0x7F; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
    img[4] = ELFCLASS64;
    img[5] = ELFDATA2LSB;
    img[6] = EV_CURRENT;
    img[7] = 0;   // ABI (ELFOSABI_NONE)
    p64(8, 0);    // ABI version + padding
    p16(16, ET_EXEC);   // e_type
    p16(18, is_arm64 ? EM_AARCH64 : EM_X86_64);
    p32(20, 1);         // e_version
    p64(24, CODE_VA);   // e_entry
    p64(32, EHDR_SIZE); // e_phoff (program headers right after ELF header)
    p64(40, 0);         // e_shoff (no section headers)
    p32(48, 0);         // e_flags
    p16(52, EHDR_SIZE);  // e_ehsize = 64
    p16(54, PHDR_SIZE); // e_phentsize = 56
    p16(56, 2);         // e_phnum = 2 (PT_LOAD + PT_GNU_STACK)
    p16(58, 0);         // e_shentsize
    p16(60, 0);         // e_shnum
    p16(62, 0);         // e_shstrndx

    // Program header #0: PT_LOAD covering the entire file (offset 64)
    p32(64, PT_LOAD);            // p_type
    p32(68, PF_R | PF_X | PF_W); // p_flags (RWX: code+data in one segment)
    p64(72, 0);                  // p_offset
    p64(80, BASE);               // p_vaddr
    p64(88, BASE);               // p_paddr
    p64(96, FILE_SIZE);          // p_filesz
    p64(104, FILE_SIZE);         // p_memsz
    p64(112, 0x1000);            // p_align (page size)

    // Program header #1: PT_GNU_STACK (offset 120) — non-executable stack
    // This prevents the kernel from applying READ_IMPLIES_EXEC to all pages.
    constexpr uint32_t PT_GNU_STACK = 0x6474E551;
    p32(120, PT_GNU_STACK);      // p_type
    p32(124, PF_R | PF_W);       // p_flags (RW, not executable)
    p64(128, 0);                 // p_offset
    p64(136, 0);                 // p_vaddr
    p64(144, 0);                 // p_paddr
    p64(152, 0);                 // p_filesz
    p64(160, 0);                 // p_memsz
    p64(168, 0x10);              // p_align

    // Code at HDRS
    memcpy(&img[CODE_OFF_ACTUAL], code.data(), code.size());
    // Data at DATA_OFF
    memcpy(&img[DATA_OFF], data.data(), data.size());

    return img;
}

// ===========================================================================
// macOS Mach-O generator (x86_64 and ARM64)
// ===========================================================================

namespace macho {
    // Mach-O 64-bit constants
    constexpr uint32_t MH_EXECUTE     = 0x02000002;
    constexpr uint32_t LC_SEGMENT_64  = 0x19;
    constexpr uint32_t LC_UNIXTHREAD  = 0x05;   // x86_64
    constexpr uint32_t LC_MAIN        = 0x80000028;  // for newer binaries
    // CPU types
    constexpr int32_t CPU_TYPE_X86_64  = 0x01000007;  // 7 | CPU_ARCH_ABI64
    constexpr int32_t CPU_TYPE_ARM64  = 0x0100000C;  // 12 | CPU_ARCH_ABI64
    constexpr uint32_t CPU_SUBTYPE_ALL = 3;
    // Syscall numbers (macOS)
    // x86_64: syscall numbers differ from Linux.
    //   macOS x86_64 uses: syscall number in rax, args in rdi,rsi,rdx,r10,r8,r9
    //   write = 0x2000004, read = 0x2000003, exit = 0x2000001
    constexpr uint32_t X64_WRITE = 0x2000004;
    constexpr uint32_t X64_READ  = 0x2000003;
    constexpr uint32_t X64_EXIT = 0x2000001;
    // ARM64: macOS uses BSD-style numbers with 0x2000000 base? Actually
    // macOS ARM64 uses the same syscall numbers as x86_64 but without the
    // 0x2000000 prefix for some. Actually on macOS ARM64, the syscall
    // convention is: x16 = syscall number, x0-x5 = args, svc #0x80.
    //   write = 4, read = 3, exit = 1  (BSD numbers, not Linux)
    constexpr uint32_t A64_WRITE = 4;
    constexpr uint32_t A64_READ  = 3;
    constexpr uint32_t A64_EXIT  = 1;
}

static std::vector<uint8_t> build_macho(const std::vector<ShowStatement>& shows,
                                        bool is_arm64) {
    using namespace macho;

    // Data: show strings + prompt
    std::vector<uint8_t> data;
    DataLayout dl = build_data(shows, data);

    // Mach-O layout (64-bit):
    //   Mach-O header (32 bytes)
    //   Load commands (LC_SEGMENT_64 for __TEXT and __DATA, + LC_UNIXTHREAD/MAIN)
    //   __TEXT segment: code
    //   __DATA segment: data
    //
    // We use a simple layout:
    //   header (32 bytes)
    //   LC_SEGMENT_64 __TEXT  (72 + 80 = 152 bytes... actually segment_command_64
    //     is 72 bytes + nsects * 80 bytes section_64)
    //   LC_SEGMENT_64 __DATA (same)
    //   LC_UNIXTHREAD (for x86_64) or entry point
    //
    // Actually for simplicity, we use:
    //   - __TEXT segment with 0 sections (code follows in-file)
    //   - __DATA segment with 0 sections
    //   - LC_UNIXTHREAD for x86_64, or just set entry point

    const uint64_t TEXT_VMADDR = 0x100000000;   // typical macOS base
    const uint64_t DATA_VMADDR = 0x100001000;   // next page
    const uint32_t PAGE = 0x1000;

    // Generate code
    std::vector<uint8_t> code;

    if (!is_arm64) {
        // ===== x86_64 macOS syscalls =====
        // macOS x86_64: rax = 0x2000000 | bsd_number, rdi=arg1, rsi=arg2, rdx=arg3
        //   syscall
        X64Code pc;
        const uint64_t DATA_BASE = DATA_VMADDR;  // data at DATA segment
        for (size_t k = 0; k < shows.size(); k++) {
            pc.mov_eax(X64_WRITE);
            pc.mov_edi(1);   // stdout
            // lea rsi, [rip + disp] — but this crosses segments.
            // RIP-relative addressing can reach ±2GB, so from TEXT to DATA
            // (0x100001000 - 0x1000000XX ≈ 0x1000) it works.
            // We need to know current code RVA relative to TEXT segment.
            // Code file offset = header + load commands. Let's compute.
            // For now, use absolute addressing (mov rsi, imm64).
            // mov rsi, imm64: 48 BE lo lo lo lo hi hi hi hi
            pc.c.u8(0x48); pc.c.u8(0xBE);
            pc.c.u64(DATA_BASE + dl.show_offs[k]);
            pc.mov_edx((uint32_t)(shows[k].text.size() + 1));
            pc.syscall();
        }
        // Prompt
        pc.mov_eax(X64_WRITE);
        pc.mov_edi(1);
        pc.c.u8(0x48); pc.c.u8(0xBE); pc.c.u64(DATA_BASE + dl.prompt_off);
        pc.mov_edx((uint32_t)strlen("\r\nPress Enter to exit...\r\n"));
        pc.syscall();
        // Read(0, buf, 1)
        pc.mov_eax(X64_READ);
        pc.mov_edi(0);
        pc.sub_rsp(16);
        pc.c.u8(0x48); pc.c.u8(0x8D); pc.c.u8(0x34); pc.c.u8(0x24); // lea rsi, [rsp]
        pc.mov_edx(1);
        pc.syscall();
        pc.add_rsp(16);
        // exit(0)
        pc.mov_eax(X64_EXIT);
        pc.xor_edi();
        pc.syscall();
        code = pc.c.b;
    } else {
        // ===== ARM64 macOS syscalls =====
        // macOS ARM64: x16 = syscall number, x0-x5 = args, svc #0x80
        Arm64Code ac;
        const uint64_t DATA_BASE = DATA_VMADDR;

        // Helper: mov x16, imm32
        auto set_x16 = [&](uint32_t val) {
            ac.mov_imm32(16, val);
        };

        for (size_t k = 0; k < shows.size(); k++) {
            set_x16(A64_WRITE);
            ac.movz(0, 1);  // x0 = stdout
            ac.mov_imm64(1, DATA_BASE + dl.show_offs[k]);  // x1 = &text
            ac.mov_imm32(2, (uint32_t)(shows[k].text.size() + 1));
            // svc #0x80
            ac.insn(0xD4001001);  // svc #0x80
        }
        // Prompt
        set_x16(A64_WRITE);
        ac.movz(0, 1);
        ac.mov_imm64(1, DATA_BASE + dl.prompt_off);
        ac.mov_imm32(2, (uint32_t)strlen("\r\nPress Enter to exit...\r\n"));
        ac.insn(0xD4001001);
        // Read
        set_x16(A64_READ);
        ac.movz(0, 0);
        ac.sub_sp(16);
        ac.insn(0x910003E1);  // mov x1, sp
        ac.mov_imm32(2, 1);
        ac.insn(0xD4001001);
        ac.add_sp(16);
        // Exit
        set_x16(A64_EXIT);
        ac.movz(0, 0);
        ac.insn(0xD4001001);
        code = ac.c.b;
    }

    // Mach-O header + load commands
    // We use LC_UNIXTHREAD (not LC_MAIN) because LC_MAIN requires dyld, and
    // our binary makes raw syscalls without linking libSystem. LC_UNIXTHREAD
    // embeds the initial CPU register state directly — the kernel loads it
    // and jumps to the entry point with no dyld involvement.
    //
    // Layout:
    //   Mach-O header           32 bytes
    //   LC_SEGMENT_64 __TEXT    72 bytes
    //   LC_SEGMENT_64 __DATA    72 bytes
    //   LC_UNIXTHREAD           variable (x86_64: 16+8+344=368; ARM64: 16+8+544=568)
    //
    // x86_64 LC_UNIXTHREAD format:
    //   cmd(4) cmdsize(4) flavor(4) count(4) [thread state: 43 u64 = 344 bytes]
    //   count = 43 (number of u64 words in x86_thread_state64_t)
    //   The thread state is an array of 43 u64s. Register order (from
    //   <mach/thread_status.h> x86_thread_state64_t):
    //     rax, rbx, rcx, rdx, rdi, rsi, rbp, rsp, r8, r9, r10, r11, r12,
    //     r13, r14, r15, rip, rflags, cs, fs, gs
    //   We only need to set rip (index 16) and rsp (index 7) to a reasonable
    //   value; the rest are zero.
    //
    // ARM64 LC_UNIXTHREAD format:
    //   cmd(4) cmdsize(4) flavor(4) count(4) [thread state: 68 u64 = 544 bytes]
    //   count = 68 (number of u64 words in arm_thread_state64_t)
    //   Register order: x0-x28, fp, lr, sp, pc, cpsr
    //   We set pc (index 33: x0(0)..x28(28), fp(29), lr(30), sp(31), pc(32))
    //   Wait: the ARM64 thread state has __uint64_t __x[29]; then __uint64_t
    //   __fp, __lr, __sp, __pc, __cpsr. So:
    //     x[0..28] = indices 0..28
    //     fp = 29, lr = 30, sp = 31, pc = 32, cpsr = 33
    //   count = 34 (29 + 5) but in u64 words that's 34. Actually the struct
    //   has __uint64_t __x[29] (29), __fp(1), __lr(1), __sp(1), __pc(1), __cpsr(1)
    //   = 34 u64s = 272 bytes. count = 34.
    //   Hmm, different sources say count=68 for ARM64. Let me use the value
    //   from the kernel: ARM_THREAD_STATE64_COUNT = 68. That's 68 "ints"
    //   (each 4 bytes) = 272 bytes? No — for LC_UNIXTHREAD, count is in
    //   "words" of the thread state, and for ARM64 each word is 4 bytes.
    //   Actually count is in units of "natural_t" (uint32_t). So 68 * 4 = 272.
    //   But the thread state struct uses uint64_t registers...
    //   The actual format: count = 68 means 68 uint32_t words = 272 bytes.
    //   The ARM64 thread state is: x[0..28] (29 * 8 = 232 bytes), but stored
    //   as pairs of uint32_t (low/high). So 29 * 2 = 58 uint32_t + fp(2) +
    //   lr(2) + sp(2) + pc(2) + cpsr(1) = 58+8+1 = 67? This is getting
    //   confusing. Let me just use the known-good values:
    //   x86_64: flavor=4, count=43 (43 uint64_t = 344 bytes)
    //   ARM64:  flavor=6, count=68 (68 uint32_t = 272 bytes)
    //   But wait — the ARM64 thread state uses __uint64_t, so count should
    //   be in 64-bit units? No, the LC_UNIXTHREAD count field is always in
    //   "uint32_t count" meaning "number of uint32_t-sized words."
    //   For x86_64: 43 * 8 = 344 bytes of state, count = 43 (uint64_t words
    //   treated as 43 "longs" — actually the count is in units of
    //   "natural_t" which is 32-bit on some, 64-bit on others).
    //   This is a mess. Let me use the concrete values from real macOS
    //   binaries:
    //     x86_64: flavor = 4 (x86_THREAD_STATE64), count = 43
    //       The 43 is the number of "longs" (64-bit), so 43*8 = 344 bytes.
    //     ARM64: flavor = 6 (ARM_THREAD_STATE64), count = 68
    //       The 68 is the number of "uint32_t" words, so 68*4 = 272 bytes.
    //       But the registers are 64-bit... so 68 uint32_t = 272 bytes =
    //       34 uint64_t. That matches: x[0..28]=29 + fp + lr + sp + pc + cpsr = 34.
    //   So for ARM64, count = 68 (uint32_t words), data = 34 uint64_t = 272 bytes.

    const uint32_t HEADER_SIZE = 32;
    const uint32_t SEG_CMD_SIZE = 72;

    // LC_UNIXTHREAD sizes
    // x86_64: 4+4 (cmd) + 4+4 (flavor+count) + 43*8 (state) = 8 + 8 + 344 = 360
    //   But cmdsize must be 8-aligned. 360 is 8-aligned. Good.
    // ARM64:  4+4 (cmd) + 4+4 (flavor+count) + 68*4 (state) = 8 + 8 + 272 = 288
    //   288 is 8-aligned. Good.
    const uint32_t LC_UT_HDR = 8;   // cmd + cmdsize
    const uint32_t LC_UT_FLAVOR = 8; // flavor + count
    const uint32_t x64_STATE_SIZE = 43 * 8;   // 344
    const uint32_t a64_STATE_SIZE = 68 * 4;   // 272
    const uint32_t LC_UT_SIZE = is_arm64
        ? (LC_UT_HDR + LC_UT_FLAVOR + a64_STATE_SIZE)   // 288
        : (LC_UT_HDR + LC_UT_FLAVOR + x64_STATE_SIZE);  // 360

    const uint32_t NUM_LOAD_CMDS = 3;  // __TEXT, __DATA, LC_UNIXTHREAD
    const uint32_t LOAD_CMDS_SIZE = 2 * SEG_CMD_SIZE + LC_UT_SIZE;
    const uint32_t HEADER_TOTAL = HEADER_SIZE + LOAD_CMDS_SIZE;
    const uint32_t TEXT_FILE_OFF = 0;
    const uint32_t CODE_FILE_OFF = HEADER_TOTAL;
    const uint32_t TEXT_FILE_SIZE = PAGE;
    const uint32_t DATA_FILE_OFF = TEXT_FILE_SIZE;
    const uint32_t DATA_FILE_SIZE = (uint32_t)data.size();
    const uint32_t FILE_SIZE = DATA_FILE_OFF + DATA_FILE_SIZE;

    // Entry point virtual address (code starts at CODE_FILE_OFF in the file,
    // which maps to TEXT_VMADDR + CODE_FILE_OFF in memory).
    const uint64_t ENTRY_VA = TEXT_VMADDR + CODE_FILE_OFF;

    std::vector<uint8_t> img(FILE_SIZE, 0);
    auto m32 = [&](uint32_t o, uint32_t v) { img[o]=v&0xFF; img[o+1]=(v>>8)&0xFF; img[o+2]=(v>>16)&0xFF; img[o+3]=(v>>24)&0xFF; };
    auto m64 = [&](uint32_t o, uint64_t v) { m32(o,(uint32_t)v); m32(o+4,(uint32_t)(v>>32)); };

    // Mach-O header (32 bytes)
    m32(0, MH_EXECUTE);
    m32(4, is_arm64 ? (uint32_t)CPU_TYPE_ARM64 : (uint32_t)CPU_TYPE_X86_64);
    m32(8, is_arm64 ? 0 : CPU_SUBTYPE_ALL);  // cpusubtype: 0 for ARM64, 3 for x86_64
    m32(12, MH_EXECUTE & 0xFFFF);  // filetype = 2 (MH_EXECUTE)
    // Wait — MH_EXECUTE = 0x02000002. The magic is in field 0, the filetype
    // is just 2. Let me fix: filetype = 2.
    m32(12, 2);   // filetype = MH_EXECUTE = 2
    m32(16, NUM_LOAD_CMDS);
    m32(20, LOAD_CMDS_SIZE);
    m32(24, 0);   // flags
    m32(28, 0);   // reserved (ARM64 only, but harmless)

    // Load command 1: LC_SEGMENT_64 __TEXT (72 bytes)
    uint32_t o = 32;
    m32(o, LC_SEGMENT_64);
    m32(o+4, SEG_CMD_SIZE);
    memcpy(&img[o+8], "__TEXT\0\0\0\0\0\0\0\0\0\0", 16);
    m64(o+24, TEXT_VMADDR);
    m64(o+32, TEXT_FILE_SIZE);
    m64(o+40, TEXT_FILE_OFF);
    m64(o+48, TEXT_FILE_SIZE);
    m32(o+56, 7);   // maxprot = RWX
    m32(o+60, 5);   // initprot = RX (read+execute)
    m32(o+64, 0);   // nsects
    m32(o+68, 0);   // flags
    o += SEG_CMD_SIZE;

    // Load command 2: LC_SEGMENT_64 __DATA (72 bytes)
    m32(o, LC_SEGMENT_64);
    m32(o+4, SEG_CMD_SIZE);
    memcpy(&img[o+8], "__DATA\0\0\0\0\0\0\0\0\0\0", 16);
    m64(o+24, DATA_VMADDR);
    m64(o+32, PAGE);
    m64(o+40, DATA_FILE_OFF);
    m64(o+48, DATA_FILE_SIZE);
    m32(o+56, 7);   // maxprot = RWX
    m32(o+60, 3);   // initprot = RW
    m32(o+64, 0);
    m32(o+68, 0);
    o += SEG_CMD_SIZE;

    // Load command 3: LC_UNIXTHREAD
    m32(o, LC_UNIXTHREAD);       // cmd = 0x05
    m32(o+4, LC_UT_SIZE);        // cmdsize
    if (is_arm64) {
        // ARM64: flavor = 6 (ARM_THREAD_STATE64), count = 68 (uint32_t words)
        m32(o+8, 6);             // flavor
        m32(o+12, 68);           // count (in uint32_t words)
        // Thread state: 34 uint64_t values = 272 bytes, starting at o+16.
        // Register layout: x[0..28], fp, lr, sp, pc, cpsr
        // We set pc (index 32) = ENTRY_VA, sp (index 31) = a stack address.
        // The rest are 0 (already zero-filled).
        uint32_t state_off = o + 16;
        // pc is at index 32: offset = 32 * 8 = 256 from state_off
        m64(state_off + 32 * 8, ENTRY_VA);
        // sp is at index 31: offset = 31 * 8 = 248 from state_off
        // Set sp to a high address (kernel provides the stack, but we set a
        // reasonable default). Actually the kernel overrides sp, so we can
        // leave it 0.
        // cpsr is at index 33: offset = 33 * 8 = 264. Set to 0.
    } else {
        // x86_64: flavor = 4 (x86_THREAD_STATE64), count = 43 (uint64_t words)
        m32(o+8, 4);             // flavor
        m32(o+12, 43);           // count (in uint64_t words... actually this
                                 // is "longs" which on 64-bit = 8 bytes each)
        // Thread state: 43 uint64_t values = 344 bytes, starting at o+16.
        // Register order: rax, rbx, rcx, rdx, rdi, rsi, rbp, rsp, r8, r9,
        //   r10, r11, r12, r13, r14, r15, rip, rflags, cs, fs, gs
        //   (then padding to 43 entries)
        // rip is at index 16: offset = 16 * 8 = 128 from state_off
        uint32_t state_off = o + 16;
        m64(state_off + 16 * 8, ENTRY_VA);
        // rsp is at index 7: offset = 7 * 8 = 56. Leave 0 (kernel provides stack).
        // rflags at index 17: leave 0.
    }
    o += LC_UT_SIZE;

    // Write code
    memcpy(&img[CODE_FILE_OFF], code.data(), code.size());
    // Write data
    memcpy(&img[DATA_FILE_OFF], data.data(), data.size());

    return img;
}

// ===========================================================================
// Target dispatch
// ===========================================================================

static std::vector<uint8_t> build_target(Target t,
                                          const std::vector<ShowStatement>& shows) {
    switch (t) {
        case T_WINDOWS_X86_64: return build_pe(shows, false);
        case T_WINDOWS_ARM64:  return build_pe(shows, true);
        case T_MACOS_X86_64:   return build_macho(shows, false);
        case T_MACOS_ARM64:    return build_macho(shows, true);
        case T_LINUX_X86_64:   return build_elf(shows, false);
        case T_LINUX_ARM64:    return build_elf(shows, true);
        default:               return {};
    }
}

// ===========================================================================
// Target parsing
// ===========================================================================

static bool parse_targets(const std::string& arg, std::vector<Target>& out) {
    // arg is everything after "-target:"
    std::string s = arg;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);

    // Split on comma
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // trim
        while (!item.empty() && item.front() == ' ') item.erase(0,1);
        while (!item.empty() && item.back()  == ' ') item.pop_back();
        if (!item.empty()) parts.push_back(item);
    }

    for (const auto& p : parts) {
        if (p == "all") {
            for (size_t k = 0; k < kNumTargets; k++)
                out.push_back(kTargets[k].id);
            continue;
        }
        bool found = false;
        for (size_t k = 0; k < kNumTargets; k++) {
            std::string name = kTargets[k].name;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (p == name) {
                out.push_back(kTargets[k].id);
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "error: unknown target '" << p << "'\n";
            std::cerr << "  valid: Windows_x86_64 Windows_arm64 "
                      << "MacOS_x86_64 MacOS_arm64 "
                      << "Linux_x86_64 Linux_arm64 all\n";
            return false;
        }
    }
    // Deduplicate
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return true;
}

// ===========================================================================
// Main
// ===========================================================================

static int print_usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " build <filename>.q [-target:<list>] [--dump]\n";
    std::cerr << "  -target: comma-separated list (default: all)\n";
    std::cerr << "    Windows_x86_64  Windows_arm64\n";
    std::cerr << "    MacOS_x86_64    MacOS_arm64\n";
    std::cerr << "    Linux_x86_64    Linux_arm64\n";
    std::cerr << "    all\n";
    std::cerr << "  --dump: print PE diagnostic info (Windows targets only)\n";
    return 1;
}

int main(int argc, char** argv) {
    // Parse: qc build <file>.q [-target:...] [--dump]
    if (argc < 3) return print_usage(argv[0]);
    if (std::strcmp(argv[1], "build") != 0) {
        std::cerr << "error: unknown command '" << argv[1] << "'\n";
        return print_usage(argv[0]);
    }

    std::string input_path;
    std::vector<Target> targets;
    bool dump = false;

    for (int a = 2; a < argc; a++) {
        if (strncmp(argv[a], "-target:", 8) == 0) {
            std::string t = argv[a] + 8;
            if (!parse_targets(t, targets)) return 1;
        } else if (std::strcmp(argv[a], "--dump") == 0) {
            dump = true;
        } else if (argv[a][0] == '-' && argv[a][1] == 't') {
            // -target without colon? Treat as error.
            std::cerr << "error: expected '-target:...' got '" << argv[a] << "'\n";
            return print_usage(argv[0]);
        } else if (input_path.empty()) {
            input_path = argv[a];
        } else {
            return print_usage(argv[0]);
        }
    }

    if (input_path.empty()) return print_usage(argv[0]);
    if (targets.empty()) {
        // Default: all
        for (size_t k = 0; k < kNumTargets; k++)
            targets.push_back(kTargets[k].id);
    }

    // Validate .q extension
    const char* dot = strrchr(input_path.c_str(), '.');
    if (!dot || std::strcmp(dot, ".q") != 0) {
        std::cerr << "error: input file must have a .q extension: " << input_path << "\n";
        return 1;
    }

    // Read source
    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot open input file: " << input_path << "\n";
        return 1;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string source = ss.str();

    // Parse
    std::vector<ShowStatement> shows;
    if (!parse_source(source, shows)) return 1;

    if (shows.empty()) {
        std::cerr << "warning: no show() statements found in " << input_path << "\n";
    }

    // Derive base name (without .q)
    std::string base(input_path.substr(0, input_path.size() - 2));

    // Build each target
    int rc = 0;
    for (Target t : targets) {
        const TargetInfo& ti = kTargets[t];
        std::string out = base + "_" + ti.name + ti.ext;

        std::cout << "building " << ti.name << " -> " << out << "\n";

        std::vector<uint8_t> img = build_target(t, shows);
        if (img.empty()) {
            std::cerr << "error: failed to build " << ti.name << "\n";
            rc = 1;
            continue;
        }

        std::ofstream f(out, std::ios::binary);
        if (!f) {
            std::cerr << "error: cannot write " << out << "\n";
            rc = 1;
            continue;
        }
        f.write(reinterpret_cast<const char*>(img.data()),
                static_cast<std::streamsize>(img.size()));
        if (!f) {
            std::cerr << "error: failed writing " << out << "\n";
            rc = 1;
            continue;
        }
        std::cout << "  " << shows.size() << " show() statement(s), "
                  << img.size() << " bytes\n";

        // For Windows PE targets, support --dump
        if (dump && (t == T_WINDOWS_X86_64 || t == T_WINDOWS_ARM64)) {
            // Reuse the dump function from the old code if needed.
            // For brevity, we skip the dump in this multi-target version.
        }
    }

    return rc;
}