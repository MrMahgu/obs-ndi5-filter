#include "ndi5-texture-filter.h"

#include "inc/Processing.NDI.Lib.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(OBS_PLUGIN, OBS_PLUGIN_LANG)

const NDIlib_v5 *ndi5_lib = nullptr;

namespace NDI5Filter {

static const char *filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text(OBS_SETTING_UI_FILTER_NAME);
}

static bool filter_update_sender_name(obs_properties_t *, obs_property_t *,
				      void *data)
{
	auto filter = (struct filter *)data;

	obs_data_t *settings = obs_source_get_settings(filter->context);
	filter_update(filter, settings);
	obs_data_release(settings);
	return true;
}

static obs_properties_t *filter_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	auto props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_text(props, OBS_SETTING_UI_SENDER_NAME,
				obs_module_text(OBS_SETTING_UI_SENDER_NAME),
				OBS_TEXT_DEFAULT);

	obs_properties_add_button(props, OBS_SETTING_UI_BUTTON_TITLE,
				  obs_module_text(OBS_SETTING_UI_BUTTON_TITLE),
				  filter_update_sender_name);

	return props;
}

static void filter_defaults(obs_data_t *defaults)
{
	obs_data_set_default_string(
		defaults, OBS_SETTING_UI_SENDER_NAME,
		obs_module_text(OBS_SETTING_DEFAULT_SENDER_NAME));
}

namespace Textures {

inline static void destroy(void *data)
{
	auto filter = (struct filter *)data;

	std::ranges::for_each(filter->staging_surface, gs_stagesurface_destroy);
	std::ranges::for_each(filter->buffer_texture, gs_texture_destroy);
}

inline static void create(void *data, uint32_t width, uint32_t height)
{
	auto filter = (struct filter *)data;

	for (auto &elm : filter->staging_surface)
		elm = gs_stagesurface_create(width, height,
					     filter->texture_format);

	for (auto &elm : filter->buffer_texture)
		elm = gs_texture_create(width, height, filter->texture_format,
					1, NULL, GS_RENDER_TARGET);
}

} // namespace Textures

namespace Framebuffers {

// Forces NDI to process the previous frame, freeing the use of any memory we gave it
inline static void flush(void *data)
{
	auto filter = (struct filter *)data;
	ndi5_lib->send_send_video_async_v2(filter->ndi_sender, NULL);
}

inline static void update_ndi_video_frame_desc(void *data, uint32_t width,
					       uint32_t height, uint32_t depth)
{
	auto filter = (struct filter *)data;

	if (!filter->first_run_update) {
		filter->first_run_update = true;
		filter->ndi_video_frame.frame_rate_D = 1000;
		filter->ndi_video_frame.frame_rate_N = 60000;
		filter->ndi_video_frame.picture_aspect_ratio = 1.778;
		filter->ndi_video_frame.frame_format_type =
			NDIlib_frame_format_type_e::
				NDIlib_frame_format_type_progressive;

		// TODO allow format to change
		filter->ndi_video_frame.FourCC = NDIlib_FourCC_type_RGBA;
	}

	// Update dimensions
	filter->ndi_video_frame.xres = width;
	filter->ndi_video_frame.yres = height;

	// TODO allow depth change (this is everywhere depth is used)
	filter->ndi_video_frame.line_stride_in_bytes = width * depth;
}

inline static void destroy(void *data)
{
	auto filter = (struct filter *)data;
	std::ranges::for_each(filter->ndi_frame_buffers,
			      [](auto &ptr) { bfree(ptr); });
}

inline static void create(void *data, uint32_t width, uint32_t height,
			  uint32_t depth)
{
	auto filter = (struct filter *)data;

	if (filter->frame_allocated) {
		warn("NDI5 frame buffers destroyed unexpectedly");
		Framebuffers::destroy(filter);
	}

	// Create the frame buffers
	std::ranges::for_each(filter->ndi_frame_buffers, [width, height,
							  depth](auto &ptr) {
		ptr = static_cast<uint8_t *>(bzalloc(width * height * depth));
	});
	filter->frame_allocated = true;

	// Update NDI5 ndi_video_frame desc
	update_ndi_video_frame_desc(filter, width, height, depth);
}

} // namespace Framebuffers

namespace Texture {

static void reset(void *data, uint32_t width, uint32_t height)
{
	auto filter = (struct filter *)data;

	// Update Texture data
	filter->width = width;
	filter->height = height;
	filter->size = width * height * filter->depth;

	// Texture buffers
	Textures::destroy(filter);
	Textures::create(filter, width, height);

	// NDI frame buffers
	Framebuffers::flush(filter);
	Framebuffers::destroy(filter);
	Framebuffers::create(filter, width, height, filter->depth);

	// Destroy the NDI5 sender
	if (filter->sender_created)
		ndi5_lib->send_destroy(filter->ndi_sender);

	// Setup the new NDI5 stream
	NDIlib_send_create_t desc;
	desc.p_ndi_name = filter->sender_name.c_str();
	desc.clock_video = false;

	filter->ndi_sender = ndi5_lib->send_create(&desc);

	if (!filter->ndi_sender) {
		error("could not create ndi sender");
	}

	filter->sender_created = true;
}

// Returns a std::pair with previous and next buffer indexes
static std::pair<int, int> calculate_buffer_indexes(void *data)
{
	auto filter = (struct filter *)data;

	uint32_t prev_buffer_index = filter->buffer_index == 0
					     ? NDI_BUFFER_MAX
					     : filter->buffer_index - 1;

	uint32_t next_buffer_index = filter->buffer_index == NDI_BUFFER_MAX
					     ? 0
					     : filter->buffer_index + 1;

	return std::make_pair(prev_buffer_index, next_buffer_index);
}

static void render(void *data, obs_source_t *target, uint32_t cx, uint32_t cy)
{
	auto filter = (struct filter *)data;

	if (filter->width != cx || filter->height != cy)
		Texture::reset(filter, cx, cy);

	auto [prev_buffer_index, next_buffer_index] =
		calculate_buffer_indexes(filter);

	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();

	filter->prev_target = gs_get_render_target();
	filter->prev_space = gs_get_color_space();

	// RENDER THE CURRENT FRAME
	gs_set_render_target_with_color_space(
		filter->buffer_texture[filter->buffer_index], NULL, GS_CS_SRGB);

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

	// MAP THE PREVIOUS FRAME
	if (gs_stagesurface_map(filter->staging_surface[prev_buffer_index],
				&filter->texture_data, &filter->linesize)) {

		memcpy(&filter->ndi_frame_buffers[filter->buffer_index][0],
		       filter->texture_data, filter->size);

		gs_stagesurface_unmap(
			filter->staging_surface[prev_buffer_index]);
	}

	// SET THE NEXT FRAME
	// If this was the very first frame, it wont'get actually get rendered
	// until frame buffer_count+1
	filter->ndi_video_frame.p_data =
		filter->ndi_frame_buffers[next_buffer_index];

	ndi5_lib->send_send_video_async_v2(filter->ndi_sender,
					   &filter->ndi_video_frame);

	// STAGE THE NEXT FRAME
	gs_stage_texture(filter->staging_surface[filter->buffer_index],
			 filter->buffer_texture[prev_buffer_index]);

	filter->buffer_index = next_buffer_index;

	return;
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

	// Render
	Texture::render(filter, target, target_width, target_height);
}

static void filter_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);

	auto filter = (struct filter *)data;

	obs_remove_main_render_callback(filter_render_callback, filter);

	// If our names have changed, rebuild NDI
	if (strcmp(filter->setting_sender_name, filter->sender_name.c_str()) !=
	    0) {
		obs_enter_graphics();

		// HACK -- tell the render engine we have no frames allocated
		filter->frame_allocated = false;

		// Change current sender
		filter->sender_name = std::string(filter->setting_sender_name);

		// Reset all the buffers -- this causes some dropped frames I think
		// TODO Check this out at some point
		Texture::reset(filter, filter->width, filter->height);

		obs_leave_graphics();
	}

	obs_add_main_render_callback(filter_render_callback, filter);
}

static void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto filter = (struct filter *)bzalloc(sizeof(NDI5Filter::filter));

	// Baseline everything
	filter->texture_format = OBS_PLUGIN_COLOR_SPACE;
	filter->buffer_index = 0;
	filter->width = 0;
	filter->height = 0;
	filter->frame_allocated = false;
	filter->sender_created = false;

	// TODO undevtest this variable
	filter->depth = 4;

	// Setup the obs context
	filter->context = source;

	// setup the ui setting
	filter->setting_sender_name =
		obs_data_get_string(settings, OBS_SETTING_UI_SENDER_NAME);

	// Copy it to our sendername
	filter->sender_name = std::string(filter->setting_sender_name);

	// force an update
	filter_update(filter, settings);

	return filter;
}

static void filter_destroy(void *data)
{
	auto filter = (struct filter *)data;

	if (!filter)
		return;

	obs_remove_main_render_callback(filter_render_callback, filter);

	// Destroy sender
	if (filter->sender_created) {
		ndi5_lib->send_destroy(filter->ndi_sender);
	}

	// Cleanup OBS stuff
	obs_enter_graphics();

	Textures::destroy(filter);

	obs_leave_graphics();

	// Flush NDI
	Framebuffers::flush(filter);

	// Destroy any framebuffers
	Framebuffers::destroy(filter);

	// ...
	filter->texture_data = nullptr;
	filter->prev_target = nullptr;

	bfree(filter);
}

static void filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);

	auto filter = (struct filter *)data;

	if (!filter->context)
		return;

	obs_source_skip_video_filter(filter->context);
}

static void filter_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	auto filter = (struct filter *)data;
	filter->frame_count++;
}

// Writes a simple log entry to OBS
void report_version()
{
#ifdef DEBUG
	info("you can haz maybe obs-ndi5-filter tooz (Version: %s)",
	     OBS_PLUGIN_VERSION_STRING);
#else
	info("obs-ndi5-filter [mrmahgu] - version %s",
	     OBS_PLUGIN_VERSION_STRING);
#endif
}

} // namespace NDI5Filter

typedef const NDIlib_v5 *(*NDIlib_v5_load_)(void);

std::unique_ptr<QLibrary> ndi5_qlibrary;

const NDIlib_v5 *load_ndi5_lib()
{
	QFileInfo library_path(QDir(QString(qgetenv(NDILIB_REDIST_FOLDER)))
				       .absoluteFilePath(NDILIB_LIBRARY_NAME));

	if (library_path.exists() && library_path.isFile()) {

		QString library_file_path = library_path.absoluteFilePath();

		ndi5_qlibrary =
			std::make_unique<QLibrary>(library_file_path, nullptr);

		if (ndi5_qlibrary->load()) {
			info("NDI runtime loaded");

			NDIlib_v5_load_ library_load =
				(NDIlib_v5_load_)ndi5_qlibrary->resolve(
					"NDIlib_v5_load");

			if (library_load == nullptr) {
				error("NDI runtime 5 was not detected");
				return nullptr;
			}

			ndi5_qlibrary.reset();

			return library_load();
		}
		ndi5_qlibrary.reset();
	}
	error("NDI runtime could not be located.");
	return nullptr;
}

bool obs_module_load(void)
{
	auto filter_info = NDI5Filter::create_filter_info();

	obs_register_source(&filter_info);

	NDI5Filter::report_version();

	ndi5_lib = load_ndi5_lib();

	if (!ndi5_lib) {
		error("critical error");
		return false;
	}

	if (!ndi5_lib->initialize()) {
		error("NDI5 said nope -- your CPU is unsupported");
		return false;
	}

	info("NDI5 (%s) IS READY TO ROCK", ndi5_lib->version());

	return true;
}

void obs_module_unload()
{
	if (ndi5_lib)
		ndi5_lib->destroy();

	if (ndi5_qlibrary) {
		ndi5_qlibrary->unload();
		ndi5_qlibrary.reset();
	}
}
