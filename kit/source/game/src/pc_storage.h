#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int ma2_pc_settings_path(const char *defaults, char *path, size_t capacity);
void ma2_pc_storage_initialize(const char *game_directory);
/* Copies missing files only; existing saves are never replaced. */
int ma2_pc_copy_missing(const char *source, const char *destination);
/* FT-RT: removes stale <cache>\PC2-* namespace folders, keeping the newest `keep` besides the current one.
 * Returns the number of folders removed. Never touches UserData, TitleData or reparse points. */
int ma2_pc_prune_cache_namespaces(const char *cache_directory, const char *current_namespace, int keep);
#ifdef __cplusplus
}
#endif
