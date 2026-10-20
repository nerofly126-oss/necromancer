# necromancer

A tiny JIT compiler in C++. It reads Brainfuck, generates x86-64 machine code at runtime, and jumps into it.

## What's in here

- `grimoire.hpp`: a small x86-64 assembler (registers, ALU ops, labels, jumps, calls)
- `raise_the_dead.cpp`: the hello world: mmap, write bytes, mprotect, call
- `first_spell.cpp`: assembler demo (sum_to and factorial built from raw instructions)
- `ritual.cpp`: three ways to run Brainfuck: `interp`, `jit` (naive) and `jitopt` (optimizing)

## Optimizer

Run folding, pointer-offset folding, clear loops (`[-]`) and multiply loops (`[->++<]`) are rewritten before code generation.

## Build and run

    g++ -std=c++17 -O2 -Wall -o ritual ritual.cpp
    ./ritual jitopt hello.bf

Linux x86-64 only.
