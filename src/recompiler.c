#include "recompiler.h"
#include <stdio.h>

void recompile_instruction(FILE* out, uint32_t pc, uint32_t instr, uint32_t next_instr) {
    uint8_t opcode = instr >> 26;
    uint8_t rs     = (instr >> 21) & 0x1F;
    uint8_t rt     = (instr >> 16) & 0x1F;
    uint8_t rd     = (instr >> 11) & 0x1F;
    uint16_t imm   = instr & 0xFFFF;
    uint8_t funct  = instr & 0x3F;

    // Hardwire Register 0 to always be 0
    if (instr == 0) {
        fprintf(out, "// NOP\n");
        return;
    }

    int32_t signed_imm = (int32_t)(int16_t)imm;

    switch (opcode) {
        case 0x00: // SPECIAL
            switch (funct) {
                case 0x00: fprintf(out, "    ctx->r[%d] = ctx->r[%d] << %d;\n", rd, rt, (instr >> 6) & 0x1F); break;
                case 0x02: fprintf(out, "    ctx->r[%d] = ctx->r[%d] >> %d;\n", rd, rt, (instr >> 6) & 0x1F); break;
                case 0x03: fprintf(out, "    ctx->r[%d] = (int32_t)ctx->r[%d] >> %d;\n", rd, rt, (instr >> 6) & 0x1F); break;
                case 0x08: fprintf(out, "    ctx->pc = ctx->r[%d]; return;\n", rs); break; // JR
                case 0x21: fprintf(out, "    ctx->r[%d] = ctx->r[%d] + ctx->r[%d];\n", rd, rs, rt); break;
                case 0x24: fprintf(out, "    ctx->r[%d] = ctx->r[%d] & ctx->r[%d];\n", rd, rs, rt); break;
                case 0x25: fprintf(out, "    ctx->r[%d] = ctx->r[%d] | ctx->r[%d];\n", rd, rs, rt); break;
                case 0x2B: fprintf(out, "    ctx->r[%d] = ((int32_t)ctx->r[%d] < (int32_t)ctx->r[%d]);\n", rd, rs, rt); break;
                default:   fprintf(out, "    // Unknown SPECIAL 0x%02x\n", funct); break;
            }
            break;

        case 0x03: // JAL
            fprintf(out, "    {\n        ctx->r[31] = 0x%08X;\n", pc + 8);
            recompile_instruction(out, pc + 4, next_instr, 0); 
            fprintf(out, "        ctx->pc = 0x%08X; return;\n    }\n", (pc & 0xF0000000) | ((instr & 0x3FFFFFF) << 2));
            break;

        case 0x04: // BEQ
        case 0x05: // BNE
            fprintf(out, "    {\n");
            recompile_instruction(out, pc + 4, next_instr, 0); // Delay Slot
            uint32_t target = pc + 4 + (signed_imm << 2);
            fprintf(out, "        if (ctx->r[%d] %s ctx->r[%d]) { ctx->pc = 0x%08X; return; }\n", 
                    rs, (opcode == 0x04 ? "==" : "!="), rt, target);
            fprintf(out, "    }\n");
            break;

        case 0x08: // ADDI
        case 0x09: // ADDIU
            fprintf(out, "    ctx->r[%d] = ctx->r[%d] + %d;\n", rt, rs, signed_imm);
            break;

        case 0x0F: // LUI
            fprintf(out, "    ctx->r[%d] = 0x%08X;\n", rt, imm << 16);
            break;

        case 0x23: // LW
            fprintf(out, "    ctx->r[%d] = mem_read32(ctx, ctx->r[%d] + %d);\n", rt, rs, signed_imm);
            break;

        case 0x2B: // SW
            fprintf(out, "    mem_write32(ctx, ctx->r[%d] + %d, ctx->r[%d]);\n", rs, signed_imm, rt);
            break;

        default:
            fprintf(out, "    // Unknown Opcode 0x%02x\n", opcode);
            break;
    }
    // Final check for Register 0
    fprintf(out, "    ctx->r[0] = 0;\n");
}