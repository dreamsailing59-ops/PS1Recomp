#ifndef PS1_RUNTIME_H
#define PS1_RUNTIME_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t r[32];   // GPR
    uint32_t pc;      // Program Counter
    uint32_t hi, lo;  // Mult/Div registers
    uint8_t ram[2048 * 1024]; // 2MB RAM
} CPU_Context;

// Map PS1 Address to local RAM offset
// PS1 RAM is mirrored at 0x00000000, 0x80000000, and 0xA0000000
static inline uint32_t map_addr(uint32_t addr) {
    return addr & 0x1FFFFF;
}

static inline uint32_t mem_read32(CPU_Context* ctx, uint32_t addr) {
    // 0x1F801814 is the GPU STAT register
    if (addr == 0x1F801814) {
        // Return a fake value that says "GPU is ready and not busy"
        // This often tricks the game into moving past the startup loop
        return 0x14000000; 
    }

    uint32_t offset = map_addr(addr);
    return *(uint32_t*)&ctx->ram[offset];
}

static inline void mem_write32(CPU_Context* ctx, uint32_t addr, uint32_t val) {
    uint32_t offset = map_addr(addr);
    if (offset < (2048 * 1024)) {
        *(uint32_t*)&ctx->ram[offset] = val;
    }
}

// Add these for the SB (Store Byte) and SH (Store Halfword) opcodes
static inline void mem_write16(CPU_Context* ctx, uint32_t addr, uint16_t val) {
    *(uint16_t*)&ctx->ram[map_addr(addr)] = val;
}

static inline void mem_write8(CPU_Context* ctx, uint32_t addr, uint8_t val) {
    ctx->ram[map_addr(addr)] = val;
}

#endif

// Declarations for the recompiled game functions
void game_entry(CPU_Context* ctx);
void dispatcher(CPU_Context* ctx);