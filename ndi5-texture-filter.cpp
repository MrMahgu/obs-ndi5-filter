#include "shared-texture-filter.h"

#ifdef DEBUG
#include <string>
#endif;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(OBS_PLUGIN, OBS_PLUGIN_LANG)

namespace NDI5Texture {

static const char *filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text(OBS_UI_SETTING_FILTER_NAME);
}

static obs_properties_t *filter_properties(void *data)
{
	UNUSED_PARAMETER(data);

	auto props = obs_properties_create();

	obs_properties_add_text(props, OBS_UI_SETTING_DESC_NAME,
				obs_module_text(OBS_UI_SETTING_DESC_NAME),
				OBS_TEXT_DEFAULT);

	return props;
}

static void filter_defaults(obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
}

namespace Texture {

#ifdef DEBUG
static void debug_report_shared_handle2(void *data)
{
	auto filter = (struct filter *)data;

	auto handle = gs_texture_get_shared_handle(filter->shared_texture);

	auto ws = "\r\n\r\n\r\n<<<===>>> POSSIBLE TEXTURE HANDLE : " +
		  std::to_string(handle) + "\r\n\r\n\r\n";

	blog(LOG_INFO, ws.c_str());
}
#endif

static void reset(void *data, uint32_t width, uint32_t height)
{
	auto filter = (struct filter *)data;

	gs_texture_destroy(filter->shared_texture);

	filter->shared_texture = NULL;

	filter->width = width;
	filter->height = height;

	filter->shared_texture =
		gs_texture_create(width, height, filter->shared_format, 1, NULL,
				  GS_RENDER_TARGET | GS_SHARED_TEX);
#ifdef DEBUG
	debug_report_shared_handle2(filter);
#endif
}

static void render(void *data, obs_source_t *target, uint32_t cx, uint32_t cy)
{
	auto filter = (struct filter *)data;

	if (filter->width != cx || filter->height != cy)
		Texture::reset(filter, cx, cy);

	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();

	filter->prev_target = gs_get_render_target();
	filter->prev_space = gs_get_color_space();

	gs_set_render_target_with_color_space(filter->shared_texture, NULL,
					      GS_CS_SRGB);

	gs_set_viewport(0, 0, filter->width, filter->height);

	struct vec4 background;

	vec4_zero(&background);

	gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	obs_source_video_render(target);

	gs_blend_state_pop();

	gs_set_render_target_with_color_space(filter->prev_target, NULL,
					      filter->prev_space);

	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();
}

} // namespace Texture

static void filter_render_callback(void *data, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);

	if (!data)
		return;

	auto filter = (struct filter *)data;

	if (!filter->context)
		return;

	auto target = obs_filter_get_parent(filter->context);

	if (!target)
		return;

	auto target_width = obs_source_get_base_width(target);
	auto target_height = obs_source_get_base_height(target);

	if (target_width == 0 || target_height == 0)
		return;

	// Render latest
	Texture::render(filter, target, target_width, target_height);
}

static void filter_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);

	auto filter = (struct filter *)data;

	obs_remove_main_render_callback(filter_render_callback, filter);

	// do some thing??

	obs_add_main_render_callback(filter_render_callback, filter);
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto filter = (struct filter *)bzalloc(sizeof(NDI5Texture::filter));

	// Baseline everything
	filter->prev_target = nullptr;
	filter->shared_texture = nullptr;
	filter->shared_format = OBS_PLUGIN_COLOR_SPACE;
	filter->width = 0;
	filter->height = 0;

	// Setup the obs context
	filter->context = source;

	// force an update
	filter_update(filter, settings);

	return filter;
}

static void filter_destroy(void *data)
{
	auto filter = (struct filter *)data;

	if (filter) {
		obs_remove_main_render_callback(filter_render_callback, filter);

		obs_enter_graphics();

		gs_texture_destroy(filter->shared_texture);

		filter->shared_texture = nullptr;
		filter->prev_target = nullptr;

		obs_leave_graphics();

		bfree(filter);
	}
}

static void filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	auto filter = (struct filter *)data;

	if (!filter->context)
		return;

	obs_source_skip_video_filter(filter->context);
}

// Writes a simple log entry to OBS
void report_version()
{
#ifdef DEBUG
	info("you can haz ndi5-texture tooz (Version: %s)",
	     OBS_PLUGIN_VERSION_STRING);
#else
	info("obs-ndi5texture-filter [mrmahgu] - version %s",
	     OBS_PLUGIN_VERSION_STRING);
#endif
}

} // namespace NDI5Texture

bool obs_module_load(void)
{
	auto filter_info = NDI5Texture::create_filter_info();

	obs_register_source(&filter_info);

	NDI5Texture::report_version();

	return true;
}

void obs_module_unload() {}
