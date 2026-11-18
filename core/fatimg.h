/* ZodiacDroid - SD card image (MBR + FAT16) creation and file copying */
#pragma once
#include <stdint.h>

int fat_format(int fd, uint64_t size);                       /* 8MB .. 2GB */
int fat_mkdir(int fd, const char *path);                     /* creates parents too */
int fat_write_file(int fd, const char *path, const uint8_t *data, uint32_t len);   /* replaces an existing file */
int fat_list(int fd, const char *path, char *out, int max);  /* "name\tsize\tisdir\n" lines */
int fat_delete(int fd, const char *path);                   /* file, or folder with its contents */
