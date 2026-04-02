#include "properties_builder.h"
#include "rendering/effect_system.h"
#include "presets/preset_manager.h"
#include <obs-module.h>
#include <string.h>

/*
 * Properties Builder
 *
 * Creates OBS UI properties from effect metadata.
 * Groups effects by category in the dropdown and generates
 * per-effect parameter sliders from JSON metadata.
 */

/* Category display order and labels */
static const struct {
	const char *id;
	const char *locale_key;
} category_order[] = {
	{"tunnel",      "VJLinkCategory.tunnel"},
	{"plasma",      "VJLinkCategory.plasma"},
	{"particle",    "VJLinkCategory.particle"},
	{"fractal",     "VJLinkCategory.fractal"},
	{"geometric",   "VJLinkCategory.geometric"},
	{"glitch",      "VJLinkCategory.glitch"},
	{"retro",       "VJLinkCategory.retro"},
	{"3d",          "VJLinkCategory.3d"},
	{"audio_viz",   "VJLinkCategory.audio_viz"},
	{"postprocess", "VJLinkCategory.postprocess"},
	{"flash",       "VJLinkCategory.flash"},
	{NULL, NULL}
};

void vjlink_props_add_effect_list(obs_properties_t *props,
                                   const char *prop_name,
                                   const char *label)
{
	obs_property_t *list = obs_properties_add_list(props, prop_name,
		label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	/* Empty / none option */
	obs_property_list_add_string(list,
		obs_module_text("VJLinkCompositor.EffectNone"), "");

	uint32_t count = vjlink_effect_system_get_count();

	/* Add effects grouped by category */
	for (int c = 0; category_order[c].id; c++) {
		const char *cat_id = category_order[c].id;
		const char *cat_label = obs_module_text(
			category_order[c].locale_key);

		bool has_entries = false;

		/* Check if category has any visible effects */
		for (uint32_t i = 0; i < count; i++) {
			struct vjlink_effect_entry *e =
				vjlink_effect_system_get_entry(i);
			if (e && !e->hidden && strcmp(e->category, cat_id) == 0) {
				has_entries = true;
				break;
			}
		}

		if (!has_entries)
			continue;

		/* Add separator with category name */
		char sep_label[128];
		snprintf(sep_label, sizeof(sep_label), "--- %s ---",
		         cat_label);
		obs_property_list_add_string(list, sep_label, "");

		/* Add effects in this category */
		for (uint32_t i = 0; i < count; i++) {
			struct vjlink_effect_entry *e =
				vjlink_effect_system_get_entry(i);
			if (!e || e->hidden || strcmp(e->category, cat_id) != 0)
				continue;

			/* Use localized name if available */
			char locale_key[128];
			snprintf(locale_key, sizeof(locale_key),
			         "VJLink.Effect.%s", e->id);
			const char *display = obs_module_text(locale_key);

			/* Fallback to metadata name if no locale */
			if (strcmp(display, locale_key) == 0)
				display = e->name;

			obs_property_list_add_string(list, display, e->id);
		}
	}

	/* Add uncategorized effects */
	for (uint32_t i = 0; i < count; i++) {
		struct vjlink_effect_entry *e =
			vjlink_effect_system_get_entry(i);
		if (!e || e->hidden)
			continue;

		bool found = false;
		for (int c = 0; category_order[c].id; c++) {
			if (strcmp(e->category, category_order[c].id) == 0) {
				found = true;
				break;
			}
		}

		if (!found)
			obs_property_list_add_string(list, e->name, e->id);
	}
}

void vjlink_props_add_effect_params(obs_properties_t *props,
                                     const char *effect_id)
{
	if (!effect_id || !*effect_id)
		return;

	struct vjlink_effect_entry *entry =
		vjlink_effect_system_find(effect_id);
	if (!entry || entry->param_count == 0)
		return;

	/* Add a group for effect parameters */
	obs_properties_t *param_group = obs_properties_create();

	for (uint32_t i = 0; i < entry->param_count; i++) {
		struct vjlink_param_def *p = &entry->params[i];
		char prop_id[128];
		snprintf(prop_id, sizeof(prop_id), "ep_%s", p->name);

		switch (p->type) {
		case VJLINK_PARAM_FLOAT:
			obs_properties_add_float_slider(param_group,
				prop_id, p->label,
				p->min_val, p->max_val,
				p->step > 0 ? p->step : 0.01);
			break;

		case VJLINK_PARAM_INT:
			obs_properties_add_int_slider(param_group,
				prop_id, p->label,
				(int)p->min_val, (int)p->max_val,
				p->step > 0 ? (int)p->step : 1);
			break;

		case VJLINK_PARAM_BOOL:
			obs_properties_add_bool(param_group,
				prop_id, p->label);
			break;

		case VJLINK_PARAM_COLOR:
			obs_properties_add_color(param_group,
				prop_id, p->label);
			break;

		case VJLINK_PARAM_VEC2:
			/* Two float sliders */
			{
				char label_x[140], label_y[140];
				char id_x[140], id_y[140];
				snprintf(label_x, sizeof(label_x),
				         "%s X", p->label);
				snprintf(label_y, sizeof(label_y),
				         "%s Y", p->label);
				snprintf(id_x, sizeof(id_x),
				         "ep_%s_x", p->name);
				snprintf(id_y, sizeof(id_y),
				         "ep_%s_y", p->name);

				obs_properties_add_float_slider(param_group,
					id_x, label_x,
					p->min_val, p->max_val,
					p->step > 0 ? p->step : 0.01);
				obs_properties_add_float_slider(param_group,
					id_y, label_y,
					p->min_val, p->max_val,
					p->step > 0 ? p->step : 0.01);
			}
			break;

		case VJLINK_PARAM_VEC4:
			/* Four float sliders */
			{
				const char *comp_names[] = {"X", "Y", "Z", "W"};
				for (int c = 0; c < 4; c++) {
					char id_c[140], label_c[140];
					snprintf(id_c, sizeof(id_c),
					         "ep_%s_%d", p->name, c);
					snprintf(label_c, sizeof(label_c),
					         "%s %s", p->label,
					         comp_names[c]);

					obs_properties_add_float_slider(
						param_group,
						id_c, label_c,
						p->min_val, p->max_val,
						p->step > 0
						? p->step : 0.01);
				}
			}
			break;
		}
	}

	obs_properties_add_group(props, "effect_params",
		"Effect Parameters", OBS_GROUP_NORMAL, param_group);
}

void vjlink_props_add_preset_list(obs_properties_t *props,
                                   const char *prop_name,
                                   const char *label)
{
	obs_property_t *list = obs_properties_add_list(props, prop_name,
		label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(list,
		obs_module_text("VJLinkCompositor.PresetNone"), "");

	uint32_t count = vjlink_preset_get_count();
	for (uint32_t i = 0; i < count; i++) {
		struct vjlink_preset *preset = vjlink_preset_get(i);
		if (preset && preset->name[0])
			obs_property_list_add_string(list,
				preset->name, preset->id);
	}
}
