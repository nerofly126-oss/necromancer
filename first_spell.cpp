#include "grimoire.hpp"
#include <cstdio>
#include <fstream>

int main() {
    // sum_to(n) = 1 + 2 + ... + n
    Grimoire s;
    Label again, finish;
    s.mov_imm(RAX, 0);
    s.bind(again);
    s.cmp_imm(RDI, 0);
    s.jcc(JLE, finish);
    s.add(RAX, RDI);
    s.sub_imm(RDI, 1);
    s.jmp(again);
    s.bind(finish);
    s.ret();
    std::ofstream("sum_to.bin", std::ios::binary)
        .write(reinterpret_cast<const char*>(s.bytes().data()), s.bytes().size());
    auto sum_to = s.finalize<long (*)(long)>();

    // fact(n) = n!
    Grimoire f;
    Label top, out;
    f.mov_imm(RAX, 1);
    f.bind(top);
    f.cmp_imm(RDI, 1);
    f.jcc(JLE, out);
    f.imul(RAX, RDI);
    f.sub_imm(RDI, 1);
    f.jmp(top);
    f.bind(out);
    f.ret();
    auto fact = f.finalize<long (*)(long)>();

    std::printf("sum_to(10)  = %ld\n", sum_to(10));
    std::printf("sum_to(100) = %ld\n", sum_to(100));
    std::printf("fact(10)    = %ld\n", fact(10));
    std::printf("fact(20)    = %ld\n", fact(20));
    return 0;
}
