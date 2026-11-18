/* mkcard: make or fill a ZodiacDroid SD card image.
 *   mkcard card.img --format 512            (MB)
 *   mkcard card.img --put hostfile /PALM/Launcher/app.prc
 *   mkcard card.img --ls /PALM              */
#include "../core/fatimg.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: mkcard img --format MB | --put file path | --ls path\n"); return 1; }
    int fd = open(argv[1], O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror(argv[1]); return 1; }
    int ok = 1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--format") && i + 1 < argc) ok &= fat_format(fd, (uint64_t)atoi(argv[++i]) << 20);
        else if (!strcmp(argv[i], "--put") && i + 2 < argc) {
            FILE *f = fopen(argv[i + 1], "rb");
            if (!f) { perror(argv[i + 1]); return 1; }
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            uint8_t *d = malloc((size_t)n + 1);
            if (fread(d, 1, (size_t)n, f) != (size_t)n) ok = 0;
            fclose(f);
            ok &= fat_write_file(fd, argv[i + 2], d, (uint32_t)n);
            free(d); i += 2;
        } else if (!strcmp(argv[i], "--ls") && i + 1 < argc) {
            static char out[65536];
            int n = fat_list(fd, argv[++i], out, sizeof out);
            fwrite(out, 1, (size_t)n, stdout);
        }
    }
    close(fd);
    if (!ok) fprintf(stderr, "failed\n");
    return ok ? 0 : 1;
}
