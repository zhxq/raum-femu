#ifndef __FEMU_WRITE_LOG_H
#define __FEMU_WRITE_LOG_H

#include <stdio.h>
#define FEMU_DEBUG_FTL

#ifdef FEMU_DEBUG_FTL
static FILE * femu_log_file;
#define write_log(fmt, ...) \
    do { if (femu_log_file) {fprintf(femu_log_file, fmt, ## __VA_ARGS__); fflush(NULL);}} while (0)
#define write_screen(fmt, ...) \
    do { printf(fmt, ## __VA_ARGS__); fflush(NULL);} while (0)
#else
#define write_log(fmt, ...) \
    do { if (0) {printf(fmt, ## __VA_ARGS__);}} while (0)
#endif

#endif