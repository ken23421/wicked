/*
 * Common DBus functions
 *
 * Copyright (C) 2011 Olaf Kirch <okir@suse.de>
 */

#include <dbus/dbus.h>
#include <sys/poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <wicked/util.h>
#include <wicked/logging.h>
#include "socket_priv.h"
#include "dbus-common.h"
#include "dbus-dict.h"

#define TRACE_ENTER()		ni_debug_dbus("%s()", __FUNCTION__)
#define TP()			ni_debug_dbus("TP - %s:%u", __FUNCTION__, __LINE__)

static ni_intmap_t      __ni_dbus_error_map[] = {
	{ "org.freedesktop.DBus.Error.AccessDenied",	EACCES },
	{ "org.freedesktop.DBus.Error.InvalidArgs",	EINVAL },
	{ "org.freedesktop.DBus.Error.UnknownMethod",	EOPNOTSUPP },

	{ NULL }
};


int
ni_dbus_translate_error(const DBusError *err, const ni_intmap_t *error_map)
{
	unsigned int errcode;

	ni_debug_dbus("%s(%s, msg=%s)", __FUNCTION__, err->name, err->message);

	if (error_map && ni_parse_int_mapped(err->name, error_map, &errcode) >= 0)
		return errcode;

	if (ni_parse_int_mapped(err->name, __ni_dbus_error_map, &errcode) >= 0)
		return errcode;

	ni_warn("Cannot translate DBus error <%s>", err->name);
	return EIO;
}

/*
 * Deserialize message
 *
 * We need this wrapper function because dbus_message_get_args_valist
 * does not copy any strings, but returns char pointers that point at
 * the message body. Which is bad if you want to access these strings
 * after you've freed the message.
 */
int
ni_dbus_message_get_args(ni_dbus_message_t *msg, ...)
{
	DBusError error;
	va_list ap;
	int rv = 0, type;

	TRACE_ENTER();
	dbus_error_init(&error);
	va_start(ap, msg);

	type = va_arg(ap, int);
	if (type
	 && !dbus_message_get_args_valist(msg, &error, type, ap)) {
		ni_error("%s: unable to retrieve msg data", __FUNCTION__);
		rv = -EINVAL;
		goto done;
	}

	while (type) {
		char **data = va_arg(ap, char **);

		switch (type) {
		case DBUS_TYPE_STRING:
		case DBUS_TYPE_OBJECT_PATH:
			if (data && *data)
				*data = xstrdup(*data);
			break;
		}

		type = va_arg(ap, int);
	}

done:
	va_end(ap);
	return rv;
}

/*
 * Deserialize message and store data in an array of variant objects
 */
int
ni_dbus_message_get_args_variants(ni_dbus_message_t *msg, ni_dbus_variant_t *argv, unsigned int max_args)
{
	DBusMessageIter iter;
	unsigned int argc = 0;

	dbus_message_iter_init(msg, &iter);
	for (argc = 0; argc < max_args; ++argc) {
		if (!ni_dbus_message_iter_get_variant_data(&iter, &argv[argc]))
			return -1;
		if (!dbus_message_iter_next(&iter))
			break;
	}

	return argc;
}

/*
 * Helper function for processing a DBusDict
 */
static inline const struct ni_dbus_dict_entry_handler *
__ni_dbus_get_property_handler(const struct ni_dbus_dict_entry_handler *handlers, const char *name)
{
	const struct ni_dbus_dict_entry_handler *h;

	for (h = handlers; h->type; ++h) {
		if (!strcmp(h->name, name))
			return h;
	}
	return NULL;
}

int
ni_dbus_process_properties(DBusMessageIter *iter, const struct ni_dbus_dict_entry_handler *handlers, void *user_object)
{
	struct ni_dbus_dict_entry entry;
	int rv = 0;

	TRACE_ENTER();
	while (ni_dbus_dict_get_entry(iter, &entry)) {
		const struct ni_dbus_dict_entry_handler *h;

#if 0
		if (entry.type == DBUS_TYPE_ARRAY) {
			ni_debug_dbus("++%s -- array of type %c", entry.key, entry.array_type);
		} else {
			ni_debug_dbus("++%s -- type %c", entry.key, entry.type);
		}
#endif

		if (!(h = __ni_dbus_get_property_handler(handlers, entry.key))) {
			ni_debug_dbus("%s: ignore unknown dict element \"%s\"", __FUNCTION__, entry.key);
			continue;
		}

		if (h->type != entry.type
		 || (h->type == DBUS_TYPE_ARRAY && h->array_type != entry.array_type)) {
			ni_error("%s: unexpected type for dict element \"%s\"", __FUNCTION__, entry.key);
			rv = -EINVAL;
			break;
		}

		if (h->type == DBUS_TYPE_ARRAY && h->array_len_max != 0
		 && (entry.array_len < h->array_len_min || h->array_len_max < entry.array_len)) {
			ni_error("%s: unexpected array length %u for dict element \"%s\"",
					__FUNCTION__, (int) entry.array_len, entry.key);
			rv = -EINVAL;
			break;
		}

		if (h->set)
			h->set(&entry, user_object);
	}

	return rv;
}

/*
 * Get/set functions for variant values
 */
static inline void
__ni_dbus_variant_change_type(ni_dbus_variant_t *var, int new_type)
{
	if (var->type == new_type)
		return;
	if (var->type != DBUS_TYPE_INVALID) {
		if (var->type == DBUS_TYPE_STRING
		 || var->type == DBUS_TYPE_OBJECT_PATH
		 || var->type == DBUS_TYPE_ARRAY)
			ni_dbus_variant_destroy(var);
	}
	var->type = new_type;
}

void
ni_dbus_variant_set_string(ni_dbus_variant_t *var, const char *value)
{
	__ni_dbus_variant_change_type(var, DBUS_TYPE_BOOLEAN);
	ni_string_dup(&var->string_value, value);
}

void
ni_dbus_variant_set_bool(ni_dbus_variant_t *var, dbus_bool_t value)
{
	__ni_dbus_variant_change_type(var, DBUS_TYPE_BOOLEAN);
	var->bool_value = value;
}

void
ni_dbus_variant_set_byte(ni_dbus_variant_t *var, unsigned char value)
{
	__ni_dbus_variant_change_type(var, DBUS_TYPE_BYTE);
	var->byte_value = value;
}

void
ni_dbus_variant_set_uint16(ni_dbus_variant_t *var, uint16_t value)
{
	__ni_dbus_variant_change_type(var, DBUS_TYPE_UINT16);
	var->uint16_value = value;
}

void
ni_dbus_variant_set_int16(ni_dbus_variant_t *var, int16_t value)
{
	__ni_dbus_variant_change_type(var, DBUS_TYPE_INT16);
	var->int16_value = value;
}

void
ni_dbus_variant_set_uint32(ni_dbus_variant_t *var, uint32_t value)
{
	__ni_dbus_variant_change_type(var, DBUS_TYPE_UINT32);
	var->uint32_value = value;
}

void
ni_dbus_variant_set_int32(ni_dbus_variant_t *var, int32_t value)
{
	__ni_dbus_variant_change_type(var, DBUS_TYPE_INT32);
	var->int32_value = value;
}

void
ni_dbus_variant_set_uint64(ni_dbus_variant_t *var, uint64_t value)
{
	__ni_dbus_variant_change_type(var, DBUS_TYPE_UINT64);
	var->uint64_value = value;
}

void
ni_dbus_variant_set_int64(ni_dbus_variant_t *var, int64_t value)
{
	__ni_dbus_variant_change_type(var, DBUS_TYPE_INT64);
	var->int64_value = value;
}

void
ni_dbus_variant_set_byte_array(ni_dbus_variant_t *var,
				unsigned int len, const unsigned char *data)
{
	ni_dbus_variant_destroy(var);
	var->type = DBUS_TYPE_ARRAY;
	var->array.element_type = DBUS_TYPE_BYTE;
	var->array.len = len;

	if (len) {
		var->byte_array_value = malloc(len);
		memcpy(var->byte_array_value, data, len);
	}
}

void
ni_dbus_variant_set_string_array(ni_dbus_variant_t *var,
				unsigned int len, const char **data)
{
	ni_dbus_variant_destroy(var);
	var->type = DBUS_TYPE_ARRAY;
	var->array.element_type = DBUS_TYPE_STRING;
	var->array.len = len;

	if (len) {
		unsigned int i;

		var->string_array_value = calloc(len, sizeof(data[0]));
		for (i = 0; i < len; ++i)
			var->string_array_value[i] = xstrdup(data[i]?: "");
	}
}

dbus_bool_t
ni_dbus_variant_append_string_array(ni_dbus_variant_t *var, const char *string)
{
	unsigned int len = var->array.len;

	if (var->type != DBUS_TYPE_ARRAY
	 || var->array.element_type != DBUS_TYPE_STRING)
		return FALSE;

	var->string_array_value = realloc(var->string_array_value, (len + 1) * sizeof(string));
	var->string_array_value[len] = xstrdup(string?: "");
	var->array.len++;

	return TRUE;
}

void
ni_dbus_variant_destroy(ni_dbus_variant_t *var)
{
	if (var->type == DBUS_TYPE_STRING)
		ni_string_free(&var->string_value);
	else if (var->type == DBUS_TYPE_ARRAY) {
		switch (var->array.element_type) {
		case DBUS_TYPE_BYTE:
			free(var->byte_array_value);
			break;
		}
	}
	memset(var, 0, sizeof(*var));
	var->type = DBUS_TYPE_INVALID;
}

const char *
ni_dbus_variant_sprint(const ni_dbus_variant_t *var)
{
	static char buffer[256];

	switch (var->type) {
	case DBUS_TYPE_STRING:
	case DBUS_TYPE_OBJECT_PATH:
		return var->string_value;

	case DBUS_TYPE_BYTE:
		snprintf(buffer, sizeof(buffer), "0x%02x", var->byte_value);
		break;

	case DBUS_TYPE_BOOLEAN:
		return var->bool_value? "true" : "false";
		break;

	case DBUS_TYPE_INT16:
		snprintf(buffer, sizeof(buffer), "%d", var->int16_value);
		break;

	case DBUS_TYPE_UINT16:
		snprintf(buffer, sizeof(buffer), "%u", var->uint16_value);
		break;

	case DBUS_TYPE_INT32:
		snprintf(buffer, sizeof(buffer), "%d", var->int32_value);
		break;

	case DBUS_TYPE_UINT32:
		snprintf(buffer, sizeof(buffer), "%u", var->uint32_value);
		break;

	case DBUS_TYPE_INT64:
		snprintf(buffer, sizeof(buffer), "%lld", (long long) var->int64_value);
		break;

	case DBUS_TYPE_UINT64:
		snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long) var->uint64_value);
		break;

	default:
		return "<unknown type>";
	}


	return buffer;
}

/*
 * Offsets of all elements in the variant struct
 */
unsigned int
__ni_dbus_variant_offsets[256] = {
[DBUS_TYPE_BYTE]		= offsetof(ni_dbus_variant_t, byte_value),
[DBUS_TYPE_BOOLEAN]		= offsetof(ni_dbus_variant_t, bool_value),
[DBUS_TYPE_STRING]		= offsetof(ni_dbus_variant_t, string_value),
[DBUS_TYPE_INT16]		= offsetof(ni_dbus_variant_t, int16_value),
[DBUS_TYPE_UINT16]		= offsetof(ni_dbus_variant_t, uint16_value),
[DBUS_TYPE_INT32]		= offsetof(ni_dbus_variant_t, int32_value),
[DBUS_TYPE_UINT32]		= offsetof(ni_dbus_variant_t, uint32_value),
[DBUS_TYPE_INT64]		= offsetof(ni_dbus_variant_t, int64_value),
[DBUS_TYPE_UINT64]		= offsetof(ni_dbus_variant_t, uint64_value),
};
