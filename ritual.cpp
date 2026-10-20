#include "grimoire.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static int bf_out(int c) { return std::putchar(c); }
static int bf_in() { return std::getchar(); }

using JitFn = void (*)(uint8_t*);

static std::string load(const char* path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error(std::string("cannot open ") + path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string out;
    for (char c : ss.str())
        if (std::string("+-<>[].,").find(c) != std::string::npos) out += c;
    return out;
}

// ---------- 1. the boring interpreter ----------
static void interpret(const std::string& code, uint8_t* tape) {
    std::vector<size_t> jump(code.size()), st;
    for (size_t i = 0; i < code.size(); i++) {
        if (code[i] == '[') st.push_back(i);
        else if (code[i] == ']') {
            if (st.empty()) throw std::runtime_error("unmatched ]");
            jump[i] = st.back(); jump[st.back()] = i; st.pop_back();
        }
    }
    if (!st.empty()) throw std::runtime_error("unmatched [");
    uint8_t* p = tape;
    for (size_t pc = 0; pc < code.size(); pc++) {
        switch (code[pc]) {
            case '>': ++p; break;
            case '<': --p; break;
            case '+': ++*p; break;
            case '-': --*p; break;
            case '.': bf_out(*p); break;
            case ',': *p = static_cast<uint8_t>(bf_in()); break;
            case '[': if (!*p) pc = jump[pc]; break;
            case ']': if (*p) pc = jump[pc]; break;
        }
    }
}

// ---------- 2. the naive JIT (one machine op per BF op, runs folded) ----------
static JitFn compile_naive(const std::string& code, Grimoire& g) {
    struct Loop { Label body, end; };
    std::vector<std::unique_ptr<Loop>> loops;
    std::vector<Loop*> open;

    g.push(RBX);
    g.mov(RBX, RDI);

    for (size_t i = 0; i < code.size();) {
        char c = code[i];
        if (c == '+' || c == '-' || c == '>' || c == '<') {
            size_t j = i;
            while (j < code.size() && code[j] == c) j++;
            size_t n = j - i;
            i = j;
            if (c == '>') g.add_imm(RBX, static_cast<int32_t>(n));
            else if (c == '<') g.sub_imm(RBX, static_cast<int32_t>(n));
            else {
                int k = static_cast<int>(n & 0xFF);
                if (k) {
                    if (c == '+') g.raw({0x80, 0x03, k});
                    else          g.raw({0x80, 0x2B, k});
                }
            }
            continue;
        }
        i++;
        if (c == '.') {
            g.raw({0x0F, 0xB6, 0x3B});
            g.mov_imm(RAX, reinterpret_cast<int64_t>(&bf_out));
            g.call(RAX);
        } else if (c == ',') {
            g.mov_imm(RAX, reinterpret_cast<int64_t>(&bf_in));
            g.call(RAX);
            g.raw({0x88, 0x03});
        } else if (c == '[') {
            loops.push_back(std::make_unique<Loop>());
            open.push_back(loops.back().get());
            g.raw({0x80, 0x3B, 0x00});
            g.jcc(JE, open.back()->end);
            g.bind(open.back()->body);
        } else if (c == ']') {
            if (open.empty()) throw std::runtime_error("unmatched ]");
            Loop* l = open.back(); open.pop_back();
            g.raw({0x80, 0x3B, 0x00});
            g.jcc(JNE, l->body);
            g.bind(l->end);
        }
    }
    if (!open.empty()) throw std::runtime_error("unmatched [");

    g.pop(RBX);
    g.ret();
    return g.finalize<JitFn>();
}

// ---------- 3. the optimizer: BF text -> IR ----------
struct Op {
    enum Kind { ADD, SET, MUL, OUT, IN, MOVE, LOOP, END } kind;
    int off;   // cell this op touches, relative to rbx
    int src;   // MUL only: cell to read
    int val;   // ADD/SET/MUL: byte value, MOVE: pointer delta
};
static Op mk(Op::Kind k, int off = 0, int src = 0, int val = 0) { return Op{k, off, src, val}; }

static std::vector<Op> optimize(const std::string& code) {
    std::vector<Op> ops;
    int off = 0;      // pending pointer displacement, not yet applied to rbx
    int depth = 0;

    auto push = [&](Op o) {
        if (o.kind == Op::ADD && !ops.empty() && ops.back().off == o.off) {
            Op& p = ops.back();
            if (p.kind == Op::ADD) {
                p.val = (p.val + o.val) & 0xFF;
                if (p.val == 0) ops.pop_back();
                return;
            }
            if (p.kind == Op::SET) { p.val = (p.val + o.val) & 0xFF; return; }
        }
        if (o.kind == Op::ADD && (o.val & 0xFF) == 0) return;
        ops.push_back(o);
    };
    auto flush = [&]() {
        if (off) { ops.push_back(mk(Op::MOVE, 0, 0, off)); off = 0; }
    };

    for (size_t i = 0; i < code.size(); i++) {
        switch (code[i]) {
            case '+': push(mk(Op::ADD, off, 0, 1)); break;
            case '-': push(mk(Op::ADD, off, 0, 0xFF)); break;
            case '>': off++; break;
            case '<': off--; break;
            case '.': ops.push_back(mk(Op::OUT, off)); break;
            case ',': ops.push_back(mk(Op::IN, off)); break;
            case '[': {
                // is this a simple loop: only + - < > inside, no nesting, no I/O?
                size_t j = i + 1;
                while (j < code.size() && std::string("+-<>").find(code[j]) != std::string::npos) j++;
                if (j < code.size() && code[j] == ']' && j > i + 1) {
                    std::map<int, int> d;
                    int p = 0;
                    for (size_t k = i + 1; k < j; k++) {
                        if (code[k] == '+') d[p]++;
                        else if (code[k] == '-') d[p]--;
                        else if (code[k] == '>') p++;
                        else p--;
                    }
                    int d0 = d.count(0) ? d[0] : 0;
                    // pointer returns home and the counter cell steps by exactly -1 or +1
                    if (p == 0 && ((d0 & 0xFF) == 0xFF || (d0 & 0xFF) == 1)) {
                        int sign = ((d0 & 0xFF) == 0xFF) ? 1 : -1;
                        for (auto& [o, v] : d)
                            if (o != 0 && (v & 0xFF))
                                ops.push_back(mk(Op::MUL, off + o, off, (sign * v) & 0xFF));
                        push(mk(Op::SET, off, 0, 0));
                        i = j;
                        break;
                    }
                }
                flush();
                ops.push_back(mk(Op::LOOP));
                depth++;
                break;
            }
            case ']':
                if (--depth < 0) throw std::runtime_error("unmatched ]");
                flush();
                ops.push_back(mk(Op::END));
                break;
        }
    }
    if (depth != 0) throw std::runtime_error("unmatched [");
    flush();
    return ops;
}

// ---------- 4. IR -> x86-64 ----------
static void emit(Grimoire& g, std::initializer_list<int> head, int disp, int imm = -1) {
    for (int b : head) g.raw({b});
    for (int i = 0; i < 4; i++) g.raw({(disp >> (8 * i)) & 0xFF});
    if (imm >= 0) g.raw({imm});
}

static JitFn compile_opt(const std::vector<Op>& ops, Grimoire& g) {
    struct Loop { Label body, end; };
    std::vector<std::unique_ptr<Loop>> loops;
    std::vector<Loop*> open;

    g.push(RBX);
    g.mov(RBX, RDI);

    for (const Op& o : ops) {
        switch (o.kind) {
            case Op::ADD:  emit(g, {0x80, 0x83}, o.off, o.val & 0xFF); break;   // add byte [rbx+off], val
            case Op::SET:  emit(g, {0xC6, 0x83}, o.off, o.val & 0xFF); break;   // mov byte [rbx+off], val
            case Op::MUL:
                emit(g, {0x0F, 0xB6, 0x83}, o.src);                             // movzx eax, byte [rbx+src]
                if ((o.val & 0xFF) != 1) g.raw({0x69, 0xC0, o.val & 0xFF, 0, 0, 0}); // imul eax, eax, val
                emit(g, {0x00, 0x83}, o.off);                                   // add [rbx+off], al
                break;
            case Op::OUT:
                emit(g, {0x0F, 0xB6, 0xBB}, o.off);                             // movzx edi, byte [rbx+off]
                g.mov_imm(RAX, reinterpret_cast<int64_t>(&bf_out));
                g.call(RAX);
                break;
            case Op::IN:
                g.mov_imm(RAX, reinterpret_cast<int64_t>(&bf_in));
                g.call(RAX);
                emit(g, {0x88, 0x83}, o.off);                                   // mov [rbx+off], al
                break;
            case Op::MOVE:
                if (o.val > 0) g.add_imm(RBX, o.val);
                else           g.sub_imm(RBX, -o.val);
                break;
            case Op::LOOP:
                loops.push_back(std::make_unique<Loop>());
                open.push_back(loops.back().get());
                g.raw({0x80, 0x3B, 0x00});                                      // cmp byte [rbx], 0
                g.jcc(JE, open.back()->end);
                g.bind(open.back()->body);
                break;
            case Op::END: {
                Loop* l = open.back(); open.pop_back();
                g.raw({0x80, 0x3B, 0x00});
                g.jcc(JNE, l->body);
                g.bind(l->end);
                break;
            }
        }
    }

    g.pop(RBX);
    g.ret();
    return g.finalize<JitFn>();
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <interp|jit|jitopt> <file.bf>\n", argv[0]);
        return 1;
    }
    try {
        std::string mode = argv[1];
        std::string code = load(argv[2]);
        std::vector<uint8_t> tape(65536, 0);
        uint8_t* start = tape.data() + 1024;

        auto t0 = std::chrono::steady_clock::now();
        if (mode == "interp") {
            interpret(code, start);
        } else if (mode == "jit") {
            Grimoire g;
            JitFn fn = compile_naive(code, g);
            fn(start);
        } else if (mode == "jitopt") {
            std::vector<Op> ops = optimize(code);
            std::fprintf(stderr, "[opt] %zu BF ops -> %zu IR ops\n", code.size(), ops.size());
            Grimoire g;
            JitFn fn = compile_opt(ops, g);
            fn(start);
        } else {
            std::fprintf(stderr, "mode must be interp, jit or jitopt\n");
            return 1;
        }
        std::fflush(stdout);
        auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[%s] %.1f ms\n", mode.c_str(), ms);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
