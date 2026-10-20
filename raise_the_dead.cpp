#include <sys/mman.h>
#include <cstdio>
#include <cstring>

int main() {
    // mov eax, 42 ; ret
    unsigned char ret42[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    // lea eax, [rdi + rsi] ; ret   (add(int, int))
    unsigned char add[] = {0x8D, 0x04, 0x37, 0xC3};

    void* mem = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); return 1; }

    memcpy(mem, ret42, sizeof ret42);
    memcpy(static_cast<unsigned char*>(mem) + 64, add, sizeof add);

    // W^X: flip from writable to executable, never both
    if (mprotect(mem, 4096, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        return 1;
    }

    auto f42 = reinterpret_cast<int (*)()>(mem);
    auto fadd = reinterpret_cast<int (*)(int, int)>(
        static_cast<unsigned char*>(mem) + 64);

    std::printf("f42()     = %d\n", f42());
    std::printf("add(7,35) = %d\n", fadd(7, 35));

    munmap(mem, 4096);
    return 0;
}
