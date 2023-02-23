#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <memory>
#include <atomic>

#include <obs-module.h>
#include <graphics/graphics.h>

#include <QString>
#include <QLibrary>
#include <QFileInfo>
#include <QDir>

#include "inc/Processing.NDI.Lib.h"

/* clang-format off */

#define OBS_PLUGIN                         "ndi5-texture-filter"
#define OBS_PLUGIN_                        "ndi5_texture_filter"
#define OBS_PLUGIN_VERSION_MAJOR           0
#define OBS_PLUGIN_VERSION_MINOR           0
#define OBS_PLUGIN_VERSION_RELEASE         1
#define OBS_PLUGIN_VERSION_STRING          "0.0.1"
#define OBS_PLUGIN_LANG                    "en-US"
#define OBS_PLUGIN_COLOR_SPACE             GS_RGBA_UNORM

#define OBS_UI_SETTING_FILTER_NAME         "mahgu.ndi5texture.ui.filter_title"
#define OBS_UI_SETTING_DESC_NAME           "mahgu.ndi5texture.ui.name_desc"

/* clang-format on */

#define obs_log(level, format, ...) \
	blog(level, "[ndi5-texture-filter] " format, ##__VA_ARGS__)

#define error(format, ...) obs_log(LOG_ERROR, format, ##__VA_ARGS__)
#define warn(format, ...) obs_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) obs_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) obs_log(LOG_DEBUG, format, ##__VA_ARGS__)

bool obs_module_load(void);
void obs_module_unload();

namespace NDI5Filter {
// DEBUG stuff

void report_version();

// OBS plugin stuff

static const char *filter_get_name(void *unused);
static obs_properties_t *filter_properties(void *data);
static void filter_defaults(obs_data_t *settings);

static void *filter_create(obs_data_t *settings, obs_source_t *source);
static void filter_destroy(void *data);
static void filter_render_callback(void *data, uint32_t cx, uint32_t cy);
static void filter_update(void *data, obs_data_t *settings);
static void filter_video_render(void *data, gs_effect_t *effect);
static void filter_video_tick(void *data, float seconds);

// Shared texture stuff

namespace Texture {

static void reset(void *data, uint32_t width, uint32_t height);
static void render(void *data, obs_source_t *target, uint32_t cx, uint32_t cy);

} // namespace Texture

struct filter {
	obs_source_t *context;

	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t size;

	enum gs_color_space prev_space;
	enum gs_color_format texture_format;

	gs_texture_t *prev_target;

	gs_texture_t *buffer_texture[2];
	gs_stagesurf_t *staging_surface[2];

	NDIlib_video_frame_v2_t ndi_video_frame;

	NDIlib_send_instance_t ndi_sender;

	bool sender_created;

	// Constant frame memory buffer
	uint8_t *frame_buffer1;
	uint8_t *frame_buffer2;

	bool frame_allocated;

	// NDI frame buffers
	void *ndi_frame_buffers[2];

	// Buffer index state
	std::atomic<bool> buffer_swap;
	bool can_render;

	uint32_t linesize;
	uint8_t *texture_data;
	bool texture_data_malloc;
};

struct obs_source_info create_filter_info()
{
	struct obs_source_info filter_info = {};

	filter_info.id = OBS_PLUGIN_;
	filter_info.type = OBS_SOURCE_TYPE_FILTER;
	filter_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;

	filter_info.get_name = filter_get_name;
	filter_info.get_properties = filter_properties;
	filter_info.get_defaults = filter_defaults;
	filter_info.create = filter_create;
	filter_info.destroy = filter_destroy;
	filter_info.video_render = filter_video_render;
	filter_info.update = filter_update;
	filter_info.video_tick = filter_video_tick;

	return filter_info;
};

} // namespace NDI5Filter
