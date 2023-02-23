#include "ndi5-texture-filter.h"

#include "inc/Processing.NDI.Lib.h"

#include <thread>

// TODO deside how to name all the plugins (obs-xxx-filter vs what we use inside, ndi5-texture-filter..etc)
// TODO compare reading styles of std::ranges vs oldschool
#include <ranges>
#include <future>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(OBS_PLUGIN, OBS_PLUGIN_LANG)

//HMODULE dll;

const NDIlib_v5 *ndi5_lib = nullptr;

namespace NDI5Filter {

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

// Sends an null frame to NDI to flush the last frame and allows us to free our memory
inline static void flush(void *data)
{
	auto filter = (struct filter *)data;
	// v1

	ndi5_lib->send_send_video_async_v2(filter->ndi_sender, NULL);
}

inline static void update(void *data, uint32_t width, uint32_t height,
			  uint32_t depth)
{
	auto filter = (struct filter *)data;

	filter->ndi_video_frame.frame_rate_D = 1000;
	filter->ndi_video_frame.frame_rate_N = 60000;

	filter->ndi_video_frame.picture_aspect_ratio = 1.778;

	filter->ndi_video_frame.frame_format_type =
		NDIlib_frame_format_type_e::NDIlib_frame_format_type_progressive;

	filter->ndi_video_frame.xres = width;
	filter->ndi_video_frame.yres = height;

	// TODO allow format to change
	filter->ndi_video_frame.FourCC = NDIlib_FourCC_type_RGBA;

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

	// Create the frame buffers
	std::ranges::for_each(filter->ndi_frame_buffers, [width, height,
							  depth](auto &ptr) {
		ptr = static_cast<char *>(bzalloc(width * height * depth));
	});

	// Make sure to update the frame buffer meta data
	update(filter, width, height, depth);

	// Resize our frame data
	// remove any existing

	if (filter->frame_allocated) {
		bfree(filter->frame_buffer1);
		bfree(filter->frame_buffer2);
	}
	filter->frame_buffer1 = (uint8_t *)(bzalloc(width * height * depth));
	filter->frame_buffer2 = (uint8_t *)(bzalloc(width * height * depth));
	filter->frame_allocated = true;

	// texture data
	if (filter->texture_data_malloc)
		bfree(filter->texture_data);
	filter->texture_data = (uint8_t *)(bzalloc(width * height * depth));
	filter->texture_data_malloc = true;
}

} // namespace Framebuffers

namespace Texture {

static void reset(void *data, uint32_t width, uint32_t height)
{
	auto filter = (struct filter *)data;

	// Texture buffers
	Textures::destroy(filter);
	Textures::create(filter, width, height);

	// NDI frame buffers
	Framebuffers::flush(filter);
	Framebuffers::destroy(filter);
	Framebuffers::create(filter, width, height, filter->depth);

	// Update Texture data
	filter->width = width;
	filter->height = height;
	filter->size = width * height * filter->depth;

	if (filter->sender_created)
		ndi5_lib->send_destroy(filter->ndi_sender);

	NDIlib_send_create_t desc;
	desc.p_ndi_name = "Test NDI";
	desc.clock_video = false;

	filter->ndi_sender = ndi5_lib->send_create(&desc);

	if (!filter->ndi_sender) {
		error("could not create ndi sender");
	}

	filter->sender_created = true;
}

static void render(void *data, obs_source_t *target, uint32_t cx, uint32_t cy)
{
	auto filter = (struct filter *)data;

	if (!filter->can_render)
		return;

	if (filter->width != cx || filter->height != cy)
		Texture::reset(filter, cx, cy);	

	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();

	filter->prev_target = gs_get_render_target();
	filter->prev_space = gs_get_color_space();

	// Render to current buffer
	gs_set_render_target_with_color_space(
		filter->buffer_texture[!filter->buffer_swap], NULL, GS_CS_SRGB);

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

	// Render OTHER buffer to current staging surface
	gs_stage_texture(filter->staging_surface[filter->buffer_swap],
			 filter->buffer_texture[!filter->buffer_swap]);

	//uint32_t linesize;
	//uint8_t *texture_data;
	bool mapped = false;

	// Map OTHER staging surface in order to copy texture into current ndi frame_buffer
	if (gs_stagesurface_map(filter->staging_surface[!filter->buffer_swap],
				&filter->texture_data, &filter->linesize)) {

		if (filter->buffer_swap)
			memcpy(&filter->frame_buffer1[0], filter->texture_data,
			       filter->size);
		else
			memcpy(&filter->frame_buffer2[0], filter->texture_data,
			       filter->size);

		gs_stagesurface_unmap(
			filter->staging_surface[!filter->buffer_swap]);

		mapped = true;
	}

	//texture_data = nullptr;

	if (mapped) {

		//filter->ndi_video_frame.timecode

		filter->ndi_video_frame.p_data =
			filter->buffer_swap ? filter->frame_buffer1
					    : filter->frame_buffer2;

		ndi5_lib->send_send_video_async_v2(filter->ndi_sender,
						   &filter->ndi_video_frame);

	} else {
		// ??
		info("not mapped");
		ndi5_lib->send_send_video_async_v2(filter->ndi_sender, NULL);
	}

	filter->buffer_swap = !filter->buffer_swap;

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
	auto filter = (struct filter *)bzalloc(sizeof(NDI5Filter::filter));

	// Baseline everything
	filter->buffer_swap = false;
	filter->texture_format = OBS_PLUGIN_COLOR_SPACE;
	filter->width = 0;
	filter->height = 0;
	filter->frame_allocated = false;
	filter->sender_created = false;
	filter->can_render = true;

	// TODO undevtest this variable
	filter->depth = 4;

	// Setup the obs context
	filter->context = source;

	// force an update
	filter_update(filter, settings);

	// Create NDI thread
	//std::jthread ndi_video_thread(ndi_video_thread)

	return filter;
}

static void filter_destroy(void *data)
{
	auto filter = (struct filter *)data;

	if (!filter)
		return;

	// we be no rendering mon
	filter->can_render = false;

	obs_remove_main_render_callback(filter_render_callback, filter);

	// Stop rendering flag maybe??

	// Flush NDI
	Framebuffers::flush(filter);

	// Destroy any framebuffers
	Framebuffers::destroy(filter);

	// Destroy sender
	// v1

	if (filter->sender_created) {
		info("destroying ndi sender?");
		ndi5_lib->send_destroy(filter->ndi_sender);
	}

	// texture data
	if (filter->texture_data_malloc) {
		bfree(filter->texture_data);
	}

	// Cleanup OBS textures and surfaces
	obs_enter_graphics();
	std::ranges::for_each(filter->staging_surface, gs_stagesurface_destroy);
	std::ranges::for_each(filter->buffer_texture, gs_texture_destroy);
	filter->prev_target = nullptr;
	obs_leave_graphics();

	// Cleanup NDI some more
	// Remove any frame_data
	if (filter->frame_allocated) {
		bfree(filter->frame_buffer1);
		bfree(filter->frame_buffer2);
	}

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

// Writes a simple log entry to OBS
void report_version()
{
#ifdef DEBUG
	info("you can haz maybe obs-ndi5-texture tooz (Version: %s)",
	     OBS_PLUGIN_VERSION_STRING);
#else
	info("obs-ndi5-texture [mrmahgu] - version %s",
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

	// v1

	ndi5_lib = load_ndi5_lib();

	if (!ndi5_lib) {
		error("critical error");
		return false;
	}

	if (!ndi5_lib->initialize()) {
		error("NDI said no -- your CPU is unsupported");
		return false;
	}

	info("NDI5 (%s) IS READY TO ROCK", ndi5_lib->version());

	return true;
}

void obs_module_unload()
{
	// v1
	//if (ndi5_lib) ndi5_lib->destroy();

	if (ndi5_qlibrary) {
		ndi5_qlibrary->unload();
		ndi5_qlibrary.reset();
	}
}
