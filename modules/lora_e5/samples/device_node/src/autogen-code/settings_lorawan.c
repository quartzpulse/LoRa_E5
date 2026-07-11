/* settings_lorawan.c — AUTO-GENERATED from settings_lorawan.yaml */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "settings_lorawan.h"

#include <zephyr/logging/log.h>
/* Registers its own log module rather than LOG_MODULE_DECLARE(main, ...)
 * -- the previous version of this template assumed every consuming app
 * names its own log module "main", which is not a safe assumption for
 * a generic, reusable generator (breaks the link for any app that
 * registers its own module under a different name, e.g.
 * LOG_MODULE_REGISTER(lora_e5_device_node, ...)). Self-registering
 * avoids the assumption entirely. */
LOG_MODULE_REGISTER(lorawan, LOG_LEVEL_INF);

/* ── Default-initialised config instance ─────────────────────────────────── */
struct lorawan_settings lorawan_config = {
#define NVS_LORAWAN_STR_ADD(type, name_var, str_key)    .name_var = {0},
#define NVS_LORAWAN_INT_ADD(type, name_var, str_key)    .name_var = 0,
#define NVS_LORAWAN_BOOL_ADD(type, name_var, str_key)   .name_var = false,
#define NVS_LORAWAN_FLOAT_ADD(type, name_var, str_key)  .name_var = 0.0f,
#define NVS_LORAWAN_DOUBLE_ADD(type, name_var, str_key) .name_var = 0.0,
#include "settings_lorawan_list.h"
#undef NVS_LORAWAN_STR_ADD
#undef NVS_LORAWAN_INT_ADD
#undef NVS_LORAWAN_BOOL_ADD
#undef NVS_LORAWAN_FLOAT_ADD
#undef NVS_LORAWAN_DOUBLE_ADD
};

/* ── Getter / Setter implementations ────────────────────────────────────── */
#define NVS_LORAWAN_STR_ADD(type, name_var, str_key) \
	const char* get_nvs_##type##_##str_key(void) { \
		return lorawan_config.name_var; \
	} \
	int set_nvs_##type##_##str_key(const char *val) { \
		strncpy(lorawan_config.name_var, val, sizeof(lorawan_config.name_var) - 1); \
		lorawan_config.name_var[sizeof(lorawan_config.name_var) - 1] = '\0'; \
		return settings_save_one("lorawan/" #str_key, lorawan_config.name_var, strlen(lorawan_config.name_var)); \
	}
#define NVS_LORAWAN_INT_ADD(type, name_var, str_key) \
	type get_nvs_##type##_##str_key(void) { \
		k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
		type val = lorawan_config.name_var; \
		k_mutex_unlock(&lorawan_config.lock); \
		return val; \
	} \
	int set_nvs_##type##_##str_key(type val) { \
		k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
		lorawan_config.name_var = val; \
		int rc = settings_save_one("lorawan/" #str_key, &lorawan_config.name_var, sizeof(lorawan_config.name_var)); \
		k_mutex_unlock(&lorawan_config.lock); \
		return rc; \
	}
#define NVS_LORAWAN_BOOL_ADD(type, name_var, str_key) \
	bool get_nvs_##type##_##str_key(void) { \
		k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
		type val = lorawan_config.name_var; \
		k_mutex_unlock(&lorawan_config.lock); \
		return val; \
	} \
	int set_nvs_##type##_##str_key(bool val) { \
		k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
		lorawan_config.name_var = val; \
		int rc = settings_save_one("lorawan/" #str_key, &lorawan_config.name_var, sizeof(lorawan_config.name_var)); \
		k_mutex_unlock(&lorawan_config.lock); \
		return rc; \
	}
#define NVS_LORAWAN_FLOAT_ADD(type, name_var, str_key) \
	type get_nvs_##type##_##str_key(void) { \
		k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
		type val = lorawan_config.name_var; \
		k_mutex_unlock(&lorawan_config.lock); \
		return val; \
	} \
	int set_nvs_##type##_##str_key(type val) { \
		k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
		lorawan_config.name_var = val; \
		int rc = settings_save_one("lorawan/" #str_key, &lorawan_config.name_var, sizeof(lorawan_config.name_var)); \
		k_mutex_unlock(&lorawan_config.lock); \
		return rc; \
	}
#define NVS_LORAWAN_DOUBLE_ADD(type, name_var, str_key) \
	type get_nvs_##type##_##str_key(void) { \
		k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
		type val = lorawan_config.name_var; \
		k_mutex_unlock(&lorawan_config.lock); \
		return val; \
	} \
	int set_nvs_##type##_##str_key(type val) { \
		k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
		lorawan_config.name_var = val; \
		int rc = settings_save_one("lorawan/" #str_key, &lorawan_config.name_var, sizeof(lorawan_config.name_var)); \
		k_mutex_unlock(&lorawan_config.lock); \
		return rc; \
	}
#include "settings_lorawan_list.h"
#undef NVS_LORAWAN_STR_ADD
#undef NVS_LORAWAN_INT_ADD
#undef NVS_LORAWAN_BOOL_ADD
#undef NVS_LORAWAN_FLOAT_ADD
#undef NVS_LORAWAN_DOUBLE_ADD

/* ── Settings load callback (h_set) ─────────────────────────────────────── */
static int lorawan_settings_set(const char *name, size_t len,
                                  settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

#define NVS_LORAWAN_STR_ADD(type, name_var, str_key) \
	if (settings_name_steq(name, #str_key, &next) && !next) { \
		if (len != 0) { \
			k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
			int rc = read_cb(cb_arg, &lorawan_config.name_var, sizeof(lorawan_config.name_var)); \
			if (rc >= 0) { \
				((char *)&lorawan_config.name_var)[sizeof(lorawan_config.name_var) - 1] = '\0'; \
			} \
			k_mutex_unlock(&lorawan_config.lock); \
		} \
		return 0; \
	}
#define NVS_LORAWAN_INT_ADD(type, name_var, str_key) \
	if (settings_name_steq(name, #str_key, &next) && !next) { \
		if (len != 0) { \
			k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
			read_cb(cb_arg, &lorawan_config.name_var, sizeof(lorawan_config.name_var)); \
			k_mutex_unlock(&lorawan_config.lock); \
		} \
		return 0; \
	}
#define NVS_LORAWAN_BOOL_ADD(type, name_var, str_key) \
	if (settings_name_steq(name, #str_key, &next) && !next) { \
		if (len != 0) { \
			k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
			read_cb(cb_arg, &lorawan_config.name_var, sizeof(lorawan_config.name_var)); \
			k_mutex_unlock(&lorawan_config.lock); \
		} \
		return 0; \
	}
#define NVS_LORAWAN_FLOAT_ADD(type, name_var, str_key) \
	if (settings_name_steq(name, #str_key, &next) && !next) { \
		if (len != 0) { \
			k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
			read_cb(cb_arg, &lorawan_config.name_var, sizeof(lorawan_config.name_var)); \
			k_mutex_unlock(&lorawan_config.lock); \
		} \
		return 0; \
	}
#define NVS_LORAWAN_DOUBLE_ADD(type, name_var, str_key) \
	if (settings_name_steq(name, #str_key, &next) && !next) { \
		if (len != 0) { \
			k_mutex_lock(&lorawan_config.lock, K_FOREVER); \
			read_cb(cb_arg, &lorawan_config.name_var, sizeof(lorawan_config.name_var)); \
			k_mutex_unlock(&lorawan_config.lock); \
		} \
		return 0; \
	}
#include "settings_lorawan_list.h"
#undef NVS_LORAWAN_STR_ADD
#undef NVS_LORAWAN_INT_ADD
#undef NVS_LORAWAN_BOOL_ADD
#undef NVS_LORAWAN_FLOAT_ADD
#undef NVS_LORAWAN_DOUBLE_ADD

	LOG_WRN("Ignoring unknown/deprecated lorawan key: %s", name);
	return 0;
}

static struct settings_handler lorawan_settings_handler = {
	.name = "lorawan",
	.h_set = lorawan_settings_set,
};

/* ── Shell commands — controlled by CONFIG_NVS_SETTINGS_LORAWAN_SHELL ──── */
#ifdef CONFIG_NVS_SETTINGS_LORAWAN_SHELL

#define NVS_LORAWAN_STR_ADD(type, name_var, str_key) \
static int cmd_lorawan_##str_key(const struct shell *sh, size_t argc, char **argv) \
{ \
	if (argc < 2) { shell_error(sh, "Usage: lorawan " #str_key " <value>"); return -EINVAL; } \
	set_nvs_##type##_##str_key(argv[1]); \
	shell_print(sh, "Saved " #str_key " [STR]: %s", (char*)get_nvs_##type##_##str_key()); \
	return 0; \
}
#define NVS_LORAWAN_INT_ADD(type, name_var, str_key) \
static int cmd_lorawan_##str_key(const struct shell *sh, size_t argc, char **argv) \
{ \
	if (argc < 2) { shell_error(sh, "Usage: lorawan " #str_key " <integer>"); return -EINVAL; } \
	char *endptr; \
	long long ll = strtoll(argv[1], &endptr, 0); \
	if (*endptr != '\0') { shell_error(sh, "Invalid integer: %s", argv[1]); return -EINVAL; } \
	set_nvs_##type##_##str_key((type)ll); \
	shell_print(sh, "Saved " #str_key " [INT]: %lld", (long long)get_nvs_##type##_##str_key()); \
	return 0; \
}
#define NVS_LORAWAN_BOOL_ADD(type, name_var, str_key) \
static int cmd_lorawan_##str_key(const struct shell *sh, size_t argc, char **argv) \
{ \
	if (argc < 2) { shell_error(sh, "Usage: lorawan " #str_key " <1|0 or true|false>"); return -EINVAL; } \
	bool b; \
	if (strcmp(argv[1],"true")==0||strcmp(argv[1],"1")==0) b = true; \
	else if (strcmp(argv[1],"false")==0||strcmp(argv[1],"0")==0) b = false; \
	else { shell_error(sh, "Invalid bool: %s. Use true/false/1/0", argv[1]); return -EINVAL; } \
	set_nvs_##type##_##str_key(b); \
	shell_print(sh, "Saved " #str_key " [BOOL]: %s", get_nvs_##type##_##str_key()?"true":"false"); \
	return 0; \
}
#define NVS_LORAWAN_FLOAT_ADD(type, name_var, str_key) \
static int cmd_lorawan_##str_key(const struct shell *sh, size_t argc, char **argv) \
{ \
	if (argc < 2) { shell_error(sh, "Usage: lorawan " #str_key " <float>"); return -EINVAL; } \
	char *endptr; \
	double d = strtod(argv[1], &endptr); \
	if (*endptr != '\0') { shell_error(sh, "Invalid number: %s", argv[1]); return -EINVAL; } \
	set_nvs_##type##_##str_key((float)d); \
	shell_print(sh, "Saved " #str_key " [FLOAT]: %f", (double)get_nvs_##type##_##str_key()); \
	return 0; \
}
#define NVS_LORAWAN_DOUBLE_ADD(type, name_var, str_key) \
static int cmd_lorawan_##str_key(const struct shell *sh, size_t argc, char **argv) \
{ \
	if (argc < 2) { shell_error(sh, "Usage: lorawan " #str_key " <double>"); return -EINVAL; } \
	char *endptr; \
	double d = strtod(argv[1], &endptr); \
	if (*endptr != '\0') { shell_error(sh, "Invalid number: %s", argv[1]); return -EINVAL; } \
	set_nvs_##type##_##str_key((double)d); \
	shell_print(sh, "Saved " #str_key " [DOUBLE]: %f", (double)get_nvs_##type##_##str_key()); \
	return 0; \
}
#include "settings_lorawan_list.h"
#undef NVS_LORAWAN_STR_ADD
#undef NVS_LORAWAN_INT_ADD
#undef NVS_LORAWAN_BOOL_ADD
#undef NVS_LORAWAN_FLOAT_ADD
#undef NVS_LORAWAN_DOUBLE_ADD

static int cmd_lorawan_show(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Current lorawan settings:");
#define NVS_LORAWAN_STR_ADD(type, name_var, str_key) \
	shell_print(sh, "  " #str_key " [STR]: '%s'", strlen(get_nvs_##type##_##str_key())>0?get_nvs_##type##_##str_key():"<not set>");
#define NVS_LORAWAN_INT_ADD(type, name_var, str_key) \
	shell_print(sh, "  " #str_key " [INT]: %lld", (long long)get_nvs_##type##_##str_key());
#define NVS_LORAWAN_BOOL_ADD(type, name_var, str_key) \
	shell_print(sh, "  " #str_key " [BOOL]: %s", get_nvs_##type##_##str_key()?"true":"false");
#define NVS_LORAWAN_FLOAT_ADD(type, name_var, str_key) \
	shell_print(sh, "  " #str_key " [FLOAT]: %f", (double)get_nvs_##type##_##str_key());
#define NVS_LORAWAN_DOUBLE_ADD(type, name_var, str_key) \
	shell_print(sh, "  " #str_key " [DOUBLE]: %f", (double)get_nvs_##type##_##str_key());
#include "settings_lorawan_list.h"
#undef NVS_LORAWAN_STR_ADD
#undef NVS_LORAWAN_INT_ADD
#undef NVS_LORAWAN_BOOL_ADD
#undef NVS_LORAWAN_FLOAT_ADD
#undef NVS_LORAWAN_DOUBLE_ADD
	return 0;
}

static const struct shell_static_entry shell_sub_lorawan[] = {
#define NVS_LORAWAN_STR_ADD(type, name_var, str_key) \
	SHELL_CMD_ARG(str_key, NULL, "Set " #str_key, cmd_lorawan_##str_key, 2, 0),
#define NVS_LORAWAN_INT_ADD(type, name_var, str_key) \
	SHELL_CMD_ARG(str_key, NULL, "Set " #str_key, cmd_lorawan_##str_key, 2, 0),
#define NVS_LORAWAN_BOOL_ADD(type, name_var, str_key) \
	SHELL_CMD_ARG(str_key, NULL, "Set " #str_key, cmd_lorawan_##str_key, 2, 0),
#define NVS_LORAWAN_FLOAT_ADD(type, name_var, str_key) \
	SHELL_CMD_ARG(str_key, NULL, "Set " #str_key, cmd_lorawan_##str_key, 2, 0),
#define NVS_LORAWAN_DOUBLE_ADD(type, name_var, str_key) \
	SHELL_CMD_ARG(str_key, NULL, "Set " #str_key, cmd_lorawan_##str_key, 2, 0),
#include "settings_lorawan_list.h"
#undef NVS_LORAWAN_STR_ADD
#undef NVS_LORAWAN_INT_ADD
#undef NVS_LORAWAN_BOOL_ADD
#undef NVS_LORAWAN_FLOAT_ADD
#undef NVS_LORAWAN_DOUBLE_ADD
	SHELL_CMD_ARG(show, NULL, "Show all lorawan settings", cmd_lorawan_show, 1, 0),
	SHELL_SUBCMD_SET_END
};

static const union shell_cmd_entry sub_lorawan = {
	.entry = shell_sub_lorawan
};

SHELL_CMD_REGISTER(lorawan, &sub_lorawan, "Lorawan Configuration Commands", NULL);

#endif /* CONFIG_NVS_SETTINGS_LORAWAN_SHELL */

/* ── Init ────────────────────────────────────────────────────────────────── */
void settings_lorawan_init(void)
{
	k_mutex_init(&lorawan_config.lock);
	settings_register(&lorawan_settings_handler);
}
