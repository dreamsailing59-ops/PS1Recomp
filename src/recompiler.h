#include <stdio.h>
#include <stdint.h>

#ifndef RECOMPILER_H
#define RECOMPILER_H

#include <stdint.h>

void recompile_instruction(FILE* out, uint32_t pc, uint32_t instr, uint32_t next_instr);
int is_branch(uint32_t instr);

#endif