/*
 * Ethernos Tiny C Compiler (ETCC)
 * platform.h - host filesystem and process helpers.
 */
#ifndef ETCC_PLATFORM_H
#define ETCC_PLATFORM_H

#include "etcc/common.h"

bool etcc_file_exists(const char *path);
char *etcc_path_dirname(EtccArena *a, const char *path);
char *etcc_path_join(EtccArena *a, const char *dir, const char *name);
char *etcc_path_canonical(EtccArena *a, const char *path);
const char *etcc_path_filename(const char *path);
char *etcc_get_executable_dir(EtccArena *a);
char *etcc_make_temp_path(EtccArena *a, const char *extension);

bool etcc_run_process(char *const argv[], bool verbose);
void etcc_remove_file_if_exists(const char *path);

#endif /* ETCC_PLATFORM_H */
