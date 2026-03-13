#include "src/ps1_runtime.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    CPU_Context *ctx = calloc(1, sizeof(CPU_Context));
    
    // Set the entry point PC from your log
    ctx->pc = 0x8003E018; 
    
    // Initialize Stack Pointer to the top of 2MB RAM
    // 0x801FFF00 is a standard starting stack for PS1 homebrew/loaders
    ctx->r[29] = 0x801FFF00; 

    printf("Starting Recompiled PS1 Game...\n");
    dispatcher(ctx);

    free(ctx);
    return 0;
}