#ifndef SETTINGS_LORAWAN_H
#define SETTINGS_LORAWAN_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

/* ── String type aliases (buffer sizes include NUL terminator) ─────────── */
typedef char lorawan_deveui_t[17];
typedef char lorawan_joineui_t[17];
typedef char lorawan_appkey_t[33];
typedef char lorawan_nwkkey_t[33];
typedef char lorawan_devaddr_t[9];
typedef char lorawan_nwkskey_t[33];
typedef char lorawan_appskey_t[33];
typedef char lorawan_region_t[8];

/* ── Settings struct ───────────────────────────────────────────────────── */
struct lorawan_settings {
	struct k_mutex lock;
#define NVS_LORAWAN_STR_ADD(type, name_var, str_key)  type name_var;
#define NVS_LORAWAN_INT_ADD(type, name_var, str_key)  type name_var;
#define NVS_LORAWAN_BOOL_ADD(type, name_var, str_key)  type name_var;
#define NVS_LORAWAN_FLOAT_ADD(type, name_var, str_key)  type name_var;
#define NVS_LORAWAN_DOUBLE_ADD(type, name_var, str_key)  type name_var;
#include "settings_lorawan_list.h"
#undef NVS_LORAWAN_STR_ADD
#undef NVS_LORAWAN_INT_ADD
#undef NVS_LORAWAN_BOOL_ADD
#undef NVS_LORAWAN_FLOAT_ADD
#undef NVS_LORAWAN_DOUBLE_ADD
};

extern struct lorawan_settings lorawan_config;
void settings_lorawan_init(void);

/* ── Auto-generated getter / setter declarations ───────────────────────── */
#define NVS_LORAWAN_STR_ADD(type, name_var, str_key) \
	const char* get_nvs_##type##_##str_key(void); \
	int set_nvs_##type##_##str_key(const char *val);
#define NVS_LORAWAN_INT_ADD(type, name_var, str_key) \
	type get_nvs_##type##_##str_key(void); \
	int set_nvs_##type##_##str_key(type val);
#define NVS_LORAWAN_FLOAT_ADD(type, name_var, str_key) \
	type get_nvs_##type##_##str_key(void); \
	int set_nvs_##type##_##str_key(type val);
#define NVS_LORAWAN_DOUBLE_ADD(type, name_var, str_key) \
	type get_nvs_##type##_##str_key(void); \
	int set_nvs_##type##_##str_key(type val);
#define NVS_LORAWAN_BOOL_ADD(type, name_var, str_key) \
	bool get_nvs_##type##_##str_key(void); \
	int set_nvs_##type##_##str_key(bool val);
#include "settings_lorawan_list.h"
#undef NVS_LORAWAN_STR_ADD
#undef NVS_LORAWAN_INT_ADD
#undef NVS_LORAWAN_BOOL_ADD
#undef NVS_LORAWAN_FLOAT_ADD
#undef NVS_LORAWAN_DOUBLE_ADD

#endif /* SETTINGS_LORAWAN_H */
