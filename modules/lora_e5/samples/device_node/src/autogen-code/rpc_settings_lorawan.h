/* rpc_settings_lorawan.h — AUTO-GENERATED from settings_lorawan.yaml */
/* Edit the YAML, then re-run gen_settings_module.py to update.   */

#ifndef RPC_SETTINGS_LORAWAN_H
#define RPC_SETTINGS_LORAWAN_H

#include <stddef.h>

/* Returns bytes written to buf (snprintf semantics, no truncation check). */
int rpc_lorawan_get(const char *key, char *buf, size_t buf_size);
int rpc_lorawan_set(const char *key, const char *params, char *buf, size_t buf_size);

#endif /* RPC_SETTINGS_LORAWAN_H */
