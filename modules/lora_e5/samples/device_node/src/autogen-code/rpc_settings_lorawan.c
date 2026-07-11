/* rpc_settings_lorawan.c — AUTO-GENERATED from settings_lorawan.yaml */
/* Edit the YAML, then re-run gen_settings_module.py to update.   */

#include "rpc_helpers.h"
#include "settings_lorawan.h"
#include "rpc_settings_lorawan.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

int rpc_lorawan_get(const char *key, char *buf, size_t buf_size)
{
	if (strcmp(key, "dev_eui") == 0) {
		const char *v = get_nvs_lorawan_deveui_t_dev_eui();
		return snprintf(buf, buf_size,
			"{\"ok\":true,\"module\":\"lorawan\",\"key\":\"dev_eui\",\"kind\":\"STR\",\"value\":\"%s\"}",
			v ? v : "");
	}
	 else 	if (strcmp(key, "join_eui") == 0) {
		const char *v = get_nvs_lorawan_joineui_t_join_eui();
		return snprintf(buf, buf_size,
			"{\"ok\":true,\"module\":\"lorawan\",\"key\":\"join_eui\",\"kind\":\"STR\",\"value\":\"%s\"}",
			v ? v : "");
	}
	 else 	if (strcmp(key, "app_key") == 0) {
		return snprintf(buf, buf_size,
			"{\"ok\":true,\"module\":\"lorawan\",\"key\":\"app_key\",\"kind\":\"STR\",\"value\":\"****\"}");
	}
	 else 	if (strcmp(key, "nwk_key") == 0) {
		return snprintf(buf, buf_size,
			"{\"ok\":true,\"module\":\"lorawan\",\"key\":\"nwk_key\",\"kind\":\"STR\",\"value\":\"****\"}");
	}
	 else 	if (strcmp(key, "dev_addr") == 0) {
		const char *v = get_nvs_lorawan_devaddr_t_dev_addr();
		return snprintf(buf, buf_size,
			"{\"ok\":true,\"module\":\"lorawan\",\"key\":\"dev_addr\",\"kind\":\"STR\",\"value\":\"%s\"}",
			v ? v : "");
	}
	 else 	if (strcmp(key, "nwk_s_key") == 0) {
		return snprintf(buf, buf_size,
			"{\"ok\":true,\"module\":\"lorawan\",\"key\":\"nwk_s_key\",\"kind\":\"STR\",\"value\":\"****\"}");
	}
	 else 	if (strcmp(key, "app_s_key") == 0) {
		return snprintf(buf, buf_size,
			"{\"ok\":true,\"module\":\"lorawan\",\"key\":\"app_s_key\",\"kind\":\"STR\",\"value\":\"****\"}");
	}
	 else 	if (strcmp(key, "join_mode_otaa") == 0) {
		return snprintf(buf, buf_size,
			"{\"ok\":true,\"module\":\"lorawan\",\"key\":\"join_mode_otaa\",\"kind\":\"BOOL\",\"value\":%s}",
			get_nvs_bool_join_mode_otaa() ? "true" : "false");
	}
	 else 	if (strcmp(key, "region") == 0) {
		const char *v = get_nvs_lorawan_region_t_region();
		return snprintf(buf, buf_size,
			"{\"ok\":true,\"module\":\"lorawan\",\"key\":\"region\",\"kind\":\"STR\",\"value\":\"%s\"}",
			v ? v : "");
	}
	return snprintf(buf, buf_size,
			"{\"ok\":false,\"error\":\"Unknown key: lorawan.%s\"}",
			key);
}

int rpc_lorawan_set(const char *key, const char *params, char *buf, size_t buf_size)
{
	int ret = -ENOENT;

	if (strcmp(key, "dev_eui") == 0) {
		char sval[17] = {0};
		if (rpc_str(params, "value", sval, sizeof(sval)) == 0) {
			ret = set_nvs_lorawan_deveui_t_dev_eui(sval);
		} else { ret = -EINVAL; }
	}
	 else 	if (strcmp(key, "join_eui") == 0) {
		char sval[17] = {0};
		if (rpc_str(params, "value", sval, sizeof(sval)) == 0) {
			ret = set_nvs_lorawan_joineui_t_join_eui(sval);
		} else { ret = -EINVAL; }
	}
	 else 	if (strcmp(key, "app_key") == 0) {
		char sval[33] = {0};
		if (rpc_str(params, "value", sval, sizeof(sval)) == 0) {
			ret = set_nvs_lorawan_appkey_t_app_key(sval);
		} else { ret = -EINVAL; }
	}
	 else 	if (strcmp(key, "nwk_key") == 0) {
		char sval[33] = {0};
		if (rpc_str(params, "value", sval, sizeof(sval)) == 0) {
			ret = set_nvs_lorawan_nwkkey_t_nwk_key(sval);
		} else { ret = -EINVAL; }
	}
	 else 	if (strcmp(key, "dev_addr") == 0) {
		char sval[9] = {0};
		if (rpc_str(params, "value", sval, sizeof(sval)) == 0) {
			ret = set_nvs_lorawan_devaddr_t_dev_addr(sval);
		} else { ret = -EINVAL; }
	}
	 else 	if (strcmp(key, "nwk_s_key") == 0) {
		char sval[33] = {0};
		if (rpc_str(params, "value", sval, sizeof(sval)) == 0) {
			ret = set_nvs_lorawan_nwkskey_t_nwk_s_key(sval);
		} else { ret = -EINVAL; }
	}
	 else 	if (strcmp(key, "app_s_key") == 0) {
		char sval[33] = {0};
		if (rpc_str(params, "value", sval, sizeof(sval)) == 0) {
			ret = set_nvs_lorawan_appskey_t_app_s_key(sval);
		} else { ret = -EINVAL; }
	}
	 else 	if (strcmp(key, "join_mode_otaa") == 0) {
		bool bval;
		if (rpc_bool(params, "value", &bval) == 0) {
			ret = set_nvs_bool_join_mode_otaa(bval);
		} else { ret = -EINVAL; }
	}
	 else 	if (strcmp(key, "region") == 0) {
		char sval[8] = {0};
		if (rpc_str(params, "value", sval, sizeof(sval)) == 0) {
			ret = set_nvs_lorawan_region_t_region(sval);
		} else { ret = -EINVAL; }
	}

	if (ret == 0) {
		return snprintf(buf, buf_size,
			"{\"ok\":true,\"module\":\"lorawan\",\"key\":\"%s\",\"saved\":true}",
			key);
	} else if (ret == -EINVAL) {
		return snprintf(buf, buf_size,
			"{\"ok\":false,\"error\":\"Invalid value for lorawan.%s\"}",
			key);
	} else if (ret == -ENOENT) {
		return snprintf(buf, buf_size,
			"{\"ok\":false,\"error\":\"Unknown key: lorawan.%s\"}",
			key);
	}
	return snprintf(buf, buf_size,
		"{\"ok\":false,\"error\":\"NVS write failed: %d\"}",
		ret);
}
