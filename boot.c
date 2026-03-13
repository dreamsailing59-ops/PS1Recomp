/*
ps1recomp - simple ISO scanner to find SYSTEM.CNF and print its BOOT line
Build: cc -o ps1recomp ps1recomp.c
This version adds .cue parsing: finds MODE2/2352 track, computes its start sector,
and reads ISO9660 starting from that track's sector.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

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

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <image.bin|image.cue>\n", argv[0]);
        return 2;
    }

    const char *input_path = argv[1];
    const char *iso_path = input_path;
    char *bin_path = NULL;
    uint32_t track_start_sector = 0;

    /* detect .cue (case-insensitive) */
    const char *ext = strrchr(input_path, '.');
    if (ext && (strcasecmp(ext, ".cue") == 0 || strcasecmp(ext, ".CUE") == 0)) {
        int r = parse_cue_mode2_2352(input_path, &bin_path, &track_start_sector);
        if (r != 0) {
            fprintf(stderr, "Failed to parse .cue or MODE2/2352 track not found (err=%d)\n", r);
            return 2;
        }
        iso_path = bin_path; /* use resolved bin path */
    }

    FILE *f = fopen(iso_path, "rb");
    if (!f) {
        perror("fopen");
        free(bin_path);
        return 3;
    }

    /* Seek to Primary Volume Descriptor within the MODE2/2352 track.
        If we parsed a .cue, track_start_sector is the INDEX 01 offset (in sectors) within the bin.
        Otherwise track_start_sector is 0 and behavior matches original program.
    */
    uint64_t pvd_offset = ((uint64_t)track_start_sector + (uint64_t)PVD_SECTOR) * (uint64_t)SECTOR_SIZE + (uint64_t)DATA_OFFSET;
    if (FSEEK_64(f, pvd_offset, SEEK_SET) != 0) {
        perror("fseek");
        fclose(f);
        free(bin_path);
        return 4;
    }
    unsigned char pvd[SECTOR_SIZE];
    if (fread(pvd, 1, SECTOR_SIZE, f) != SECTOR_SIZE) {
        fprintf(stderr, "Failed to read PVD\n");
        fclose(f);
        free(bin_path);
        return 5;
    }

    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        fprintf(stderr, "Not a valid ISO9660 Primary Volume Descriptor\n");
        fclose(f);
        free(bin_path);
        return 6;
    }

    /* Root directory record starts at offset 156 in PVD */
    unsigned char *root = pvd + 156;
    int root_len = root[0];
    if (root_len < 34) {
        fprintf(stderr, "Invalid root directory record\n");
        fclose(f);
        free(bin_path);
        return 7;
    }

    uint32_t root_lba = le32(root + 2);
    uint32_t root_size = le32(root + 10);

    /* Read root directory extent */
    uint64_t root_offset = (uint64_t)root_lba * SECTOR_SIZE + (uint64_t)track_start_sector * SECTOR_SIZE;
    /* Note: root_lba is relative to the start of the track; we have already positioned at track start for PVD,
        but the file is a single .bin file; compute absolute byte offset within file as (track_start_sector + root_lba)*SECTOR_SIZE.
        However since we didn't reposition to track start, use absolute: (root_lba + track_start_sector)*SECTOR_SIZE.
    */
    root_offset =
        ((uint64_t)root_lba + (uint64_t)track_start_sector) * SECTOR_SIZE
        + DATA_OFFSET;
    
    if (FSEEK_64(f, root_offset, SEEK_SET) != 0) {
        perror("fseek root");
        fclose(f);
        free(bin_path);
        return 8;
    }

    unsigned char *dirbuf = malloc(root_size);
    if (!dirbuf) {
        fprintf(stderr, "Out of memory\n");
        fclose(f);
        free(bin_path);
        return 9;
    }
    if (fread(dirbuf, 1, root_size, f) != root_size) {
        fprintf(stderr, "Failed to read root directory data\n");
        free(dirbuf);
        fclose(f);
        free(bin_path);
        return 10;
    }

    /* Scan directory records to find SYSTEM.CNF */
    uint32_t pos = 0;
    int found = 0;
    uint32_t sf_lba = 0, sf_size = 0;

    while (pos < root_size) {
        unsigned char dr_len = dirbuf[pos];
        if (dr_len == 0) {
            /* advance to next sector boundary */
            uint32_t next = ((pos / SECTOR_SIZE) + 1) * SECTOR_SIZE;
            if (next <= pos) break;
            pos = next;
            continue;
        }
        if (pos + dr_len > root_size) break;
        unsigned char file_flags = dirbuf[pos + 25];
        unsigned char file_id_len = dirbuf[pos + 32];
        unsigned char *file_id = dirbuf + pos + 33;

        /* skip '.' and '..' entries */
        if (!(file_id_len == 1 && (file_id[0] == 0 || file_id[0] == 1))) {
            if (id_matches(file_id, file_id_len, "SYSTEM.CNF")) {
                /* found */
                printf("Found file: %.*s\n", (int)file_id_len, file_id);
                sf_lba = le32(dirbuf + pos + 2);
                sf_size = le32(dirbuf + pos + 10);
                found = 1;
                break;
            }
        }

        pos += dr_len;
    }

    if (!found) {
        fprintf(stderr, "SYSTEM.CNF not found in root directory\n");
        free(dirbuf);
        fclose(f);
        free(bin_path);
        return 11;
    }

    /* Read SYSTEM.CNF content (limit to reasonable size) */
    uint64_t sf_offset =
        ((uint64_t)sf_lba + (uint64_t)track_start_sector) * SECTOR_SIZE
        + DATA_OFFSET;
    
    if (FSEEK_64(f, sf_offset, SEEK_SET) != 0) {
        perror("fseek system.cnf");
        free(dirbuf);
        fclose(f);
        free(bin_path);
        return 12;
    }

    size_t toread = sf_size;
    if (toread == 0) {
        fprintf(stderr, "SYSTEM.CNF has zero size\n");
        free(dirbuf);
        fclose(f);
        free(bin_path);
        return 13;
    }

    unsigned char *data = malloc(toread + 1);
    if (!data) {
        fprintf(stderr, "Out of memory\n");
        free(dirbuf);
        fclose(f);
        free(bin_path);
        return 14;
    }
    if (fread(data, 1, toread, f) != toread) {
        fprintf(stderr, "Failed to read SYSTEM.CNF\n");
        free(data);
        free(dirbuf);
        fclose(f);
        free(bin_path);
        return 15;
    }
    data[toread] = '\0'; /* ensure NUL-terminated */

    /* Parse lines to find BOOT line */
    char *p = (char *)data;
    char *end = (char *)data + toread;
    int printed = 0;
    while (p < end) {
        char *line_start = p;
        char *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r') line_end++;
        size_t linelen = line_end - line_start;
        char *line = malloc(linelen + 1);
        if (!line) break;
        memcpy(line, line_start, linelen);
        line[linelen] = '\0';
        /* advance p past this line (skip possible CRLF) */
        p = line_end;
        while (p < end && (*p == '\n' || *p == '\r')) p++;

        /* trim and check */
        char *s = line;
        trim_left_inplace(&s);
        trim_right(s);
        /* check if starts with "BOOT" (case-insensitive) */
        if (strlen(s) >= 4 && tolower((unsigned char)s[0]) == 'b' &&
            tolower((unsigned char)s[1]) == 'o' &&
            tolower((unsigned char)s[2]) == 'o' &&
            tolower((unsigned char)s[3]) == 't') {
            /* print the line as-is (trimmed) */
            printf("%s\n", s);
            printed = 1;
            free(line);
            break;
        }
        free(line);
    }

    if (!printed) {
        fprintf(stderr, "BOOT line not found in SYSTEM.CNF\n");
        free(data);
        free(dirbuf);
        fclose(f);
        free(bin_path);
        return 16;
    }

    free(data);
    free(dirbuf);
    fclose(f);
    free(bin_path);
    return 0;
}