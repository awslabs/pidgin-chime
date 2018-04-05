/*
 * Pidgin/libpurple Chime client plugin
 *
 * Copyright © 2017 Amazon.com, Inc. or its affiliates.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#define _chime_prop_enum(low, up, json, name, nick, req) \
	PROP_##up,
#define CHIME_PROPS_ENUM STRING_PROPS(_chime_prop_enum) BOOL_PROPS(_chime_prop_enum)

#define _chime_prop_var_str(low, up, jaon, name, nick, req) \
	gchar *low;
#define _chime_prop_var_bool(low, up, json, name, nick, req) \
	gboolean low;
#define CHIME_PROPS_VARS STRING_PROPS(_chime_prop_var_str) BOOL_PROPS(_chime_prop_var_bool)

#define _chime_prop_parse_var_str(low, up, json, name, nick, req) \
	const gchar *low = NULL;
#define _chime_prop_parse_var_bool(low, up, json, name, nick, req) \
	gboolean low = FALSE;
#define CHIME_PROPS_PARSE_VARS STRING_PROPS(_chime_prop_parse_var_str) BOOL_PROPS(_chime_prop_parse_var_bool)

#define _chime_prop_free_str(low, up, json, name, nick, req) \
	g_free(self->low);
#define CHIME_PROPS_FREE STRING_PROPS(_chime_prop_free_str) /* Nothing for bools */

#define _chime_prop_get_str(low, up, json, name, nick, req) \
	case PROP_##up: g_value_set_string(value, self->low); break;
#define _chime_prop_get_bool(low, up, json, name, nick, req) \
	case PROP_##up: g_value_set_boolean(value, self->low); break;
#define CHIME_PROPS_GET STRING_PROPS(_chime_prop_get_str) BOOL_PROPS(_chime_prop_get_bool)

#define _chime_prop_set_str(low, up, json, name, nick, req) \
	case PROP_##up: g_free(self->low); self->low = g_value_dup_string(value); break;
#define _chime_prop_set_bool(low, up, json, name, nick, req) \
	case PROP_##up: self->low = g_value_get_boolean(value); break;
#define CHIME_PROPS_SET STRING_PROPS(_chime_prop_set_str) BOOL_PROPS(_chime_prop_set_bool)

#define _chime_prop_reg_str(low, up, json, name, nick, req) \
	props[PROP_##up] = g_param_spec_string(name, nick, nick, NULL, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
#define _chime_prop_reg_bool(low, up, json, name, nick, req) \
	props[PROP_##up] = g_param_spec_boolean(name, nick, nick, FALSE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
#define CHIME_PROPS_REG STRING_PROPS(_chime_prop_reg_str) BOOL_PROPS(_chime_prop_reg_bool)

#define _chime_prop_parse_str(low, up, json, name, nick, req) \
	(!parse_string(node, json, &low) && req) ||
#define _chime_prop_parse_bool(low, up, json, name, nick, req) \
	(!parse_boolean(node, json, &low) && req) ||
#define CHIME_PROPS_PARSE STRING_PROPS(_chime_prop_parse_str) BOOL_PROPS(_chime_prop_parse_bool) 0

#define _chime_prop_newobj(low, up, json, name, nick, req) \
	nick, low,
#define CHIME_PROPS_NEWOBJ STRING_PROPS(_chime_prop_newobj) BOOL_PROPS(_chime_prop_newobj)

#define _chime_prop_update_str(low, up, json, name, nick, req)	\
	if (low && g_strcmp0(low, CHIME_PROP_OBJ_VAR->low)) {		\
		g_free(CHIME_PROP_OBJ_VAR->low);			\
		CHIME_PROP_OBJ_VAR->low = g_strdup(low);		\
		g_object_notify(G_OBJECT(CHIME_PROP_OBJ_VAR), name);	\
	}
#define _chime_prop_update_bool(low, up, json, name, nick, req)	\
	if (low != CHIME_PROP_OBJ_VAR->low) {				\
		CHIME_PROP_OBJ_VAR->low = low;				\
		g_object_notify(G_OBJECT(CHIME_PROP_OBJ_VAR), name);	\
	}
#define CHIME_PROPS_UPDATE STRING_PROPS(_chime_prop_update_str) BOOL_PROPS(_chime_prop_update_bool)
