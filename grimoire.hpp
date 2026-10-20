#pragma once
#include <sys/mman.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

enum Reg : uint8_t {
    RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15
};

// second byte of the 0F 8x jcc rel32 encodings
enum Cond : uint8_t { JE = 0x84, JNE = 0x85, JL = 0x8C, JGE = 0x8D, JLE = 0x8E, JG = 0x8F };

struct Label {
    long pos = -1;
    std::vector<size_t> fixups;
};

class Grimoire {
public:
    Grimoire() = default;
    Grimoire(const Grimoire&) = delete;
    Grimoire& operator=(const Grimoire&) = delete;
    ~Grimoire() { if (mem_) munmap(mem_, mem_size_); }

    // moves and arithmetic (all 64-bit)
    void mov(Reg dst, Reg src) { rex_w(src, dst); byte(0x89); modrm_rr(src, dst); }
    void mov_imm(Reg dst, int64_t imm) {
        byte(0x48 | (dst >> 3));
        byte(0xB8 | (dst & 7));
        for (int i = 0; i < 8; i++) byte((imm >> (8 * i)) & 0xFF);
    }
    void add(Reg dst, Reg src) { rex_w(src, dst); byte(0x01); modrm_rr(src, dst); }
    void sub(Reg dst, Reg src) { rex_w(src, dst); byte(0x29); modrm_rr(src, dst); }
    void cmp(Reg a, Reg b)     { rex_w(b, a);     byte(0x39); modrm_rr(b, a); }
    void imul(Reg dst, Reg src) { rex_w(dst, src); byte(0x0F); byte(0xAF); modrm_rr(dst, src); }
    void add_imm(Reg dst, int32_t imm) { alu_imm(0, dst, imm); }
    void sub_imm(Reg dst, int32_t imm) { alu_imm(5, dst, imm); }
    void cmp_imm(Reg dst, int32_t imm) { alu_imm(7, dst, imm); }

    // stack and calls
    void push(Reg r) { if (r >= 8) byte(0x41); byte(0x50 | (r & 7)); }
    void pop(Reg r)  { if (r >= 8) byte(0x41); byte(0x58 | (r & 7)); }
    void call(Reg r) { if (r >= 8) byte(0x41); byte(0xFF); byte(0xC0 | (2 << 3) | (r & 7)); }
    void ret() { byte(0xC3); }

    // control flow
    void jmp(Label& l) { byte(0xE9); rel32(l); }
    void jcc(Cond c, Label& l) { byte(0x0F); byte(c); rel32(l); }
    void bind(Label& l) {
        l.pos = static_cast<long>(code_.size());
        for (size_t at : l.fixups) patch32(at, static_cast<int32_t>(l.pos - static_cast<long>(at + 4)));
        l.fixups.clear();
    }

void raw(std::initializer_list<int> bs) { for (int b : bs) byte(b); }
    const std::vector<uint8_t>& bytes() const { return code_; }

    // copy into fresh memory, flip to executable, hand back a function pointer
    template <typename Fn>
    Fn finalize() {
        if (mem_) throw std::runtime_error("already finalized");
        if (code_.empty()) throw std::runtime_error("no code");
        mem_size_ = (code_.size() + 4095) & ~size_t(4095);
        mem_ = mmap(nullptr, mem_size_, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem_ == MAP_FAILED) { mem_ = nullptr; throw std::runtime_error("mmap failed"); }
        std::memcpy(mem_, code_.data(), code_.size());
        if (mprotect(mem_, mem_size_, PROT_READ | PROT_EXEC) != 0)
            throw std::runtime_error("mprotect failed");
        return reinterpret_cast<Fn>(mem_);
    }

private:
    std::vector<uint8_t> code_;
    void* mem_ = nullptr;
    size_t mem_size_ = 0;

    void byte(int b) { code_.push_back(static_cast<uint8_t>(b)); }
    void imm32(int32_t v) { for (int i = 0; i < 4; i++) byte((v >> (8 * i)) & 0xFF); }
    void patch32(size_t at, int32_t v) { for (int i = 0; i < 4; i++) code_[at + i] = (v >> (8 * i)) & 0xFF; }
    void rex_w(int reg, int rm) { byte(0x48 | ((reg >> 3) << 2) | (rm >> 3)); }
    void modrm_rr(int reg, int rm) { byte(0xC0 | ((reg & 7) << 3) | (rm & 7)); }
    void alu_imm(int ext, Reg dst, int32_t imm) { rex_w(0, dst); byte(0x81); modrm_rr(ext, dst); imm32(imm); }
    void rel32(Label& l) {
        size_t at = code_.size();
        if (l.pos >= 0) imm32(static_cast<int32_t>(l.pos - static_cast<long>(at + 4)));
        else { l.fixups.push_back(at); imm32(0); }
    }
};
