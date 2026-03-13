#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "loader.h"
#include "recompiler.h"

#define SECTOR_SIZE 2352
#define DATA_OFFSET 24
#define PVD_SECTOR 16

#if defined(_WIN32) || defined(_WIN64)
#define FSEEK_64(f, off, whence) _fseeki64((f), (long long)(off), (whence))
#else
#define FSEEK_64(f, off, whence) fseeko((f), (off_t)(off), (whence))
#endif

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void trim_right(char *s) {
    size_t i = strlen(s);
    while (i && (s[i-1] == '\r' || s[i-1] == '\n' || isspace((unsigned char)s[i-1]))) { s[--i] = '\0'; }
}

static void trim_left_inplace(char **s) {
    while (**s && isspace((unsigned char)**s)) (*s)++;
}

/* Compare file identifier against target (case-insensitive), ignore trailing ;version */
static int id_matches(const unsigned char *id, int idlen, const char *target) {
    int tlen = strlen(target);
    int i = 0;
    /* Compare until ';' or end */
    for (; i < idlen && id[i] != ';'; ++i) {
        char a = id[i];
        char b = (i < tlen) ? target[i] : '\0';
        if (tolower((unsigned char)a) != tolower((unsigned char)b)) return 0;
    }
    return (i == tlen) && (i <= idlen);
}

/* case-insensitive startswith */
static int starts_with_ci(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++; prefix++;
    }
    return 1;
}

/* Resolve path: if filename is absolute, return strdup(filename), otherwise join with dir (which must end with separator or be empty) */
static char *join_path(const char *dir, const char *filename) {
    if (!dir || !dir[0]) return strdup(filename);
#if defined(_WIN32) || defined(_WIN64)
    /* consider "C:\..." or "\\" as absolute */
    if ((strlen(filename) >= 2 && filename[1] == ':') || filename[0] == '\\' || filename[0] == '/') {
        return strdup(filename);
    }
#else
    if (filename[0] == '/') return strdup(filename);
#endif
    size_t n = strlen(dir);
    int need_sep = (n && dir[n-1] != '/' && dir[n-1] != '\\');
    size_t len = n + (need_sep ? 1 : 0) + strlen(filename) + 1;
    char *out = malloc(len);
    if (!out) return NULL;
    strcpy(out, dir);
    if (need_sep) {
#if defined(_WIN32) || defined(_WIN64)
        strcat(out, "\\");
#else
        strcat(out, "/");
#endif
    }
    strcat(out, filename);
    return out;
}

/* Parse mm:ss:ff into sector count (frames); returns -1 on parse error */
static int parse_time_to_sectors(const char *t) {
    int mm = 0, ss = 0, ff = 0;
    if (sscanf(t, "%d:%d:%d", &mm, &ss, &ff) != 3) return -1;
    return mm * 60 * 75 + ss * 75 + ff;
}

/* Parse .cue file to find first MODE2/2352 track.
    On success, returns 0 and outputs bin_path (malloc'd) and track_start_sector.
    Caller must free *bin_path.
*/
static int parse_cue_mode2_2352(const char *cue_path, char **out_bin_path, uint32_t *out_start_sector) {
    FILE *cf = fopen(cue_path, "r");
    if (!cf) return -1;

    /* determine cue directory */
    char cue_dir[1024];
    strncpy(cue_dir, cue_path, sizeof(cue_dir)-1);
    cue_dir[sizeof(cue_dir)-1] = '\0';
    char *p = cue_dir + strlen(cue_dir);
    while (p > cue_dir && *p != '/' && *p != '\\') p--;
    if (p > cue_dir) {
        /* keep up to slash */
        p[1] = '\0';
    } else {
        cue_dir[0] = '\0';
    }

    char line[2048];
    char *current_file = NULL;
    char current_track_type[64] = {0};
    int found = 0;
    char *found_file = NULL;
    uint32_t found_sector = 0;

    while (fgets(line, sizeof(line), cf)) {
        char *s = line;
        trim_left_inplace(&s);
        trim_right(s);
        if (!*s) continue;

        if (starts_with_ci(s, "FILE")) {
            /* FILE "name.bin" BINARY  or FILE name.bin BINARY */
            const char *q = s + 4;
            while (*q && isspace((unsigned char)*q)) q++;
            free(current_file);
            current_file = NULL;
            if (*q == '"') {
                q++;
                const char *e = strchr(q, '"');
                if (!e) continue;
                size_t len = e - q;
                char tmp[1024];
                if (len >= sizeof(tmp)) continue;
                memcpy(tmp, q, len);
                tmp[len] = '\0';
                current_file = join_path(cue_dir, tmp);
            } else {
                /* token up to space */
                const char *e = q;
                while (*e && !isspace((unsigned char)*e)) e++;
                size_t len = e - q;
                char tmp[1024];
                if (len >= sizeof(tmp)) continue;
                memcpy(tmp, q, len);
                tmp[len] = '\0';
                current_file = join_path(cue_dir, tmp);
            }
            continue;
        }

        if (starts_with_ci(s, "TRACK")) {
            /* TRACK NN MODE2/2352 */
            const char *q = s + 5;
            while (*q && isspace((unsigned char)*q)) q++;
            /* skip track number */
            while (*q && !isspace((unsigned char)*q)) q++;
            while (*q && isspace((unsigned char)*q)) q++;
            /* q now points to type */
            current_track_type[0] = '\0';
            if (*q) {
                strncpy(current_track_type, q, sizeof(current_track_type)-1);
                current_track_type[sizeof(current_track_type)-1] = '\0';
                /* uppercase trim after token */
                char *t = current_track_type;
                char *space = t;
                while (*space && !isspace((unsigned char)*space)) space++;
                *space = '\0';
            }
            continue;
        }

        if (starts_with_ci(s, "INDEX")) {
            /* INDEX NN mm:ss:ff */
            const char *q = s + 5;
            while (*q && isspace((unsigned char)*q)) q++;
            /* index number */
            int idx = atoi(q);
            while (*q && !isspace((unsigned char)*q)) q++;
            while (*q && isspace((unsigned char)*q)) q++;
            if (idx == 1 && current_file && current_track_type[0]) {
                if (starts_with_ci(current_track_type, "MODE2/2352")) {
                    int sec = parse_time_to_sectors(q);
                    if (sec >= 0) {
                        found = 1;
                        found_sector = (uint32_t)sec;
                        found_file = strdup(current_file);
                        break;
                    }
                }
            }
            continue;
        }
    }

    free(current_file);
    fclose(cf);

    if (!found) {
        free(found_file);
        return -2;
    }
    *out_bin_path = found_file;
    *out_start_sector = found_sector;
    return 0;
}

void extract_file(FILE *iso_f, uint32_t lba, uint32_t size, uint32_t track_start, const char *out_name) {
    // Calculate absolute offset in the .bin
    uint64_t offset = ((uint64_t)lba + (uint64_t)track_start) * SECTOR_SIZE + DATA_OFFSET;
    FSEEK_64(iso_f, offset, SEEK_SET);
    
    FILE *out_f = fopen(out_name, "wb");
    if (!out_f) return;

    uint8_t buffer[2048];
    uint32_t remaining = size;
    while (remaining > 0) {
        uint32_t to_read = (remaining > 2048) ? 2048 : remaining;
        fread(buffer, 1, to_read, iso_f);
        fwrite(buffer, 1, to_read, out_f);
        remaining -= to_read;
        if (remaining > 0) {
            offset += SECTOR_SIZE;
            FSEEK_64(iso_f, offset, SEEK_SET);
        }
    }
    fclose(out_f);
}

#include "loader.h" // MAKE SURE THIS IS AT THE VERY TOP OF boot.c

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <image.bin|image.cue>\n", argv[0]);
        return 2;
    }

    const char *input_path = argv[1];
    const char *iso_path = input_path;
    char *bin_path = NULL;
    uint32_t track_start_sector = 0;

    const char *ext = strrchr(input_path, '.');
    if (ext && (strcasecmp(ext, ".cue") == 0 || strcasecmp(ext, ".CUE") == 0)) {
        int r = parse_cue_mode2_2352(input_path, &bin_path, &track_start_sector);
        if (r != 0) {
            fprintf(stderr, "Failed to parse .cue (err=%d)\n", r);
            return 2;
        }
        iso_path = bin_path;
    }

    FILE *f = fopen(iso_path, "rb");
    if (!f) {
        perror("fopen");
        free(bin_path);
        return 3;
    }

    uint64_t pvd_offset = ((uint64_t)track_start_sector + (uint64_t)PVD_SECTOR) * (uint64_t)SECTOR_SIZE + (uint64_t)DATA_OFFSET;
    FSEEK_64(f, pvd_offset, SEEK_SET);
    
    unsigned char pvd[SECTOR_SIZE];
    if (fread(pvd, 1, SECTOR_SIZE, f) != SECTOR_SIZE) {
        fclose(f);
        free(bin_path);
        return 5;
    }

    unsigned char *root = pvd + 156;
    uint32_t root_lba = le32(root + 2);
    uint32_t root_size = le32(root + 10);

    uint64_t root_offset = ((uint64_t)root_lba + (uint64_t)track_start_sector) * SECTOR_SIZE + DATA_OFFSET;
    FSEEK_64(f, root_offset, SEEK_SET);

    unsigned char *dirbuf = malloc(root_size);
    fread(dirbuf, 1, root_size, f);

    /* Pass 1: Find SYSTEM.CNF */
    uint32_t pos = 0;
    int found = 0;
    uint32_t sf_lba = 0, sf_size = 0;

    while (pos < root_size) {
        unsigned char dr_len = dirbuf[pos];
        if (dr_len == 0) {
            uint32_t next = ((pos / SECTOR_SIZE) + 1) * SECTOR_SIZE;
            if (next <= pos) break;
            pos = next;
            continue;
        }
        unsigned char file_id_len = dirbuf[pos + 32];
        unsigned char *file_id = dirbuf + pos + 33;

        if (id_matches(file_id, file_id_len, "SYSTEM.CNF")) {
            sf_lba = le32(dirbuf + pos + 2);
            sf_size = le32(dirbuf + pos + 10);
            found = 1;
            break;
        }
        pos += dr_len;
    }

    if (!found) {
        fprintf(stderr, "SYSTEM.CNF not found\n");
        return 11;
    }

    /* Read SYSTEM.CNF */
    uint64_t sf_offset = ((uint64_t)sf_lba + (uint64_t)track_start_sector) * SECTOR_SIZE + DATA_OFFSET;
    FSEEK_64(f, sf_offset, SEEK_SET);
    char *cnf_data = malloc(sf_size + 1);
    fread(cnf_data, 1, sf_size, f);
    cnf_data[sf_size] = '\0';

    /* Parse BOOT line and find EXE name */
    char exe_name[32] = {0};
    char *line = strtok(cnf_data, "\r\n");
    while (line) {
        char *s = line;
        trim_left_inplace(&s);
        if (strncasecmp(s, "BOOT", 4) == 0) {
            char *start = strstr(s, "cdrom:\\");
            if (start) {
                start += 7;
                char *end_ptr = strchr(start, ';');
                if (!end_ptr) end_ptr = strchr(start, ' ');
                if (end_ptr) {
                    strncpy(exe_name, start, end_ptr - start);
                } else {
                    strcpy(exe_name, start);
                }
            }
            break;
        }
        line = strtok(NULL, "\r\n");
    }

    if (exe_name[0] == '\0') {
        fprintf(stderr, "Could not find EXE name in SYSTEM.CNF\n");
        return 17;
    }

    /* Pass 2: Find the EXE on disc */
    found = 0;
    pos = 0;
    while (pos < root_size) {
        unsigned char dr_len = dirbuf[pos];
        if (dr_len == 0) {
            uint32_t next = ((pos / SECTOR_SIZE) + 1) * SECTOR_SIZE;
            pos = next; continue;
        }
        unsigned char file_id_len = dirbuf[pos + 32];
        unsigned char *file_id = dirbuf + pos + 33;
        if (id_matches(file_id, file_id_len, exe_name)) {
            sf_lba = le32(dirbuf + pos + 2);
            sf_size = le32(dirbuf + pos + 10);
            found = 1; break;
        }
        pos += dr_len;
    }
    
    if (found) {
        extract_file(f, sf_lba, sf_size, track_start_sector, "extracted.exe");
        
        PSX_Exe game_exe;
        if (load_psx_exe("extracted.exe", &game_exe) == 0) {
            FILE* out = fopen("recompiled_game.c", "w");
            fprintf(out, "#include \"../src/ps1_runtime.h\"\n\n");
            fprintf(out, "void game_entry(CPU_Context* ctx) {\n");
            
            uint32_t current_pc = game_exe.pc;
            uint32_t buffer_offset = current_pc - game_exe.dest_vaddr;
            uint32_t instructions_count = game_exe.file_size / 4;

            // Use a switch-case for the entire executable area
            fprintf(out, "    switch(ctx->pc) {\n");

            for (uint32_t i = 0; i < instructions_count; i++) {
                uint32_t pc = current_pc + (i * 4);
                uint32_t phys_offset = buffer_offset + (i * 4);
                if (phys_offset >= game_exe.file_size) break;

                uint32_t instr = *(uint32_t*)&game_exe.body[phys_offset];
                uint32_t next = (phys_offset + 4 < game_exe.file_size) ? *(uint32_t*)&game_exe.body[phys_offset + 4] : 0;

                fprintf(out, "block_%08X: ;\n", pc);
                recompile_instruction(out, pc, instr, next);
                
                uint8_t op = instr >> 26;
                int is_branch = ((op >= 0x01 && op <= 0x07) || op == 0x02 || op == 0x03);

                // CRITICAL: If NOT a branch, the C code must move the PC manually
                if (!is_branch) {
                    fprintf(out, "    ctx->pc = 0x%08X;\n", pc + 4);
                } else {
                    // If IT IS a branch, we already handled the delay slot, so skip next instr
                    i++;
                }
            }

            fprintf(out, "        default: return;\n");
            fprintf(out, "    }\n}\n\n");

            fprintf(out, "void dispatcher(CPU_Context* ctx) {\n");
            fprintf(out, "    unsigned long long instr_count = 0;\n"); // Track progress
            fprintf(out, "    while(1) {\n");
            fprintf(out, "        if (ctx->pc >= 0x%08X && ctx->pc < 0x%08X) {\n", 
                            game_exe.dest_vaddr, game_exe.dest_vaddr + game_exe.file_size);
            fprintf(out, "            game_entry(ctx);\n");
            fprintf(out, "            instr_count++;\n");
            fprintf(out, "            if (instr_count %% 1000000 == 0) {\n");
            fprintf(out, "                printf(\"Milestone: %%llu instructions. Current PC: 0x%%08X\\n\", instr_count, ctx->pc);\n");
            // Print the values of a few registers to see if they are changing
            fprintf(out, "                printf(\"  v0: 0x%%08X, a0: 0x%%08X, sp: 0x%%08X\\n\", ctx->r[2], ctx->r[4], ctx->r[29]);\n");
            fprintf(out, "            }\n");
            fprintf(out, "        } else if (ctx->pc == 0x000000A0 || ctx->pc == 0xA0) {\n");
            fprintf(out, "            printf(\"BIOS Call A0 detected! Function: 0x%%X\\n\", ctx->r[9]);\n");
            fprintf(out, "            ctx->pc = ctx->r[31];\n"); // Return to caller ($ra)
            fprintf(out, "        } else {\n");
            fprintf(out, "            printf(\"Jumped to unknown address 0x%%08X\\n\", ctx->pc);\n");
            fprintf(out, "            return;\n");
            fprintf(out, "        }\n");
            fprintf(out, "    }\n}\n");

            fclose(out);
            printf("Successfully recompiled to recompiled_game.c\n");
        }
    }
}