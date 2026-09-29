/*
 * Copyright (C) 2014 Hugh Bailey
 * Copyright (C) 2022 Patrick Mours
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Adapted from https://github.com/obsproject/obs-studio/blob/master/plugins/win-capture/graphics-hook/d3d11-capture.cpp
 */

#include <imgui.h>
#include <reshade.hpp>
#include "obs_hook_info.hpp"
#include <d3d11.h>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

struct capture_data
{
	uint32_t cx;
	uint32_t cy;
	reshade::api::format format;
	bool using_shtex;
	bool multisampled;

	union
	{
		/* shared texture */
		struct
		{
			shtex_data *shtex_info;
			reshade::api::resource texture;
			HANDLE handle;
		} shtex;
		/* shared memory */
		struct
		{
			reshade::api::resource copy_surfaces[NUM_BUFFERS];
			bool texture_ready[NUM_BUFFERS];
			bool texture_mapped[NUM_BUFFERS];
			uint32_t pitch;
			shmem_data *shmem_info;
			int cur_tex;
			int copy_wait;
		} shmem;
	};
} data;

// Settings (persisted in ReShade.ini under [OBS_CAPTURE])
static bool s_streamproof_capture = true; // Send the frame to OBS before ReShade effects and overlay are rendered
static bool s_hide_nui = true; // Send the frame to OBS before FiveM composites its NUI (CEF browser UI) layer

// Per-frame state (only touched from the render thread)
static bool s_captured_this_frame = false;
static uint32_t s_nui_draws_this_frame = 0;
static uint32_t s_nui_draws_last_frame = 0;
static reshade::api::effect_runtime *s_runtime = nullptr;

// Set of all 2D textures that were created or opened with the shared flag.
// FiveM renders its NUI browser into a Direct3D 11 shared texture inside the CEF GPU process and opens it in the game process
// via 'ID3D11Device::OpenSharedResource'. ReShade reports that as a resource with the shared flag, so any draw call that
// samples one of these textures while the swap chain back buffer is the render target is the NUI composite.
static std::shared_mutex s_shared_textures_mutex;
static std::unordered_set<uint64_t> s_shared_textures;

static bool capture_impl_init(reshade::api::device *device, const reshade::api::resource_desc &desc, void *window)
{
	data.format = reshade::api::format_to_default_typed(desc.texture.format, 0);
	data.multisampled = desc.texture.samples > 1;
	data.cx = desc.texture.width;
	data.cy = desc.texture.height;

	const reshade::api::resource_usage copy_state = data.multisampled ? reshade::api::resource_usage::resolve_dest : reshade::api::resource_usage::copy_dest;

	// Using shared texture with OBS only works in Direct3D 10/11
	if ((device->get_api() == reshade::api::device_api::d3d10 || device->get_api() == reshade::api::device_api::d3d11) && !global_hook_info->force_shmem)
	{
		data.using_shtex = true;

		if (!device->create_resource(
				reshade::api::resource_desc(data.cx, data.cy, 1, 1, data.format, 1, reshade::api::memory_heap::gpu_only, reshade::api::resource_usage::shader_resource | copy_state, reshade::api::resource_flags::shared),
				nullptr,
				copy_state,
				&data.shtex.texture,
				&data.shtex.handle))
			return false;

		data.format = device->get_resource_desc(data.shtex.texture).texture.format;

		if (!capture_init_shtex(data.shtex.shtex_info, window, data.cx, data.cy, static_cast<uint32_t>(data.format), false, (uintptr_t)data.shtex.handle))
			return false;
	}
	else
	{
		data.using_shtex = false;

		for (int i = 0; i < NUM_BUFFERS; i++)
		{
			if (!device->create_resource(
					reshade::api::resource_desc(data.cx, data.cy, 1, 1, data.format, 1, reshade::api::memory_heap::gpu_to_cpu, copy_state),
					nullptr,
					copy_state,
					&data.shmem.copy_surfaces[i]))
				return false;
		}

		// It is possible for the device to fall back to a different underlying texture format, so fetch the one actually used by the created resource now
		data.format = device->get_resource_desc(data.shmem.copy_surfaces[0]).texture.format;

		reshade::api::subresource_data mapped;
		if (device->map_texture_region(data.shmem.copy_surfaces[0], 0, nullptr, reshade::api::map_access::read_only, &mapped))
		{
			data.shmem.pitch = mapped.row_pitch;
			device->unmap_texture_region(data.shmem.copy_surfaces[0], 0);
		}

		if (!capture_init_shmem(data.shmem.shmem_info, window, data.cx, data.cy, data.shmem.pitch, static_cast<uint32_t>(data.format), false))
			return false;
	}

	return true;
}
static void capture_impl_free(reshade::api::device *device)
{
	capture_free();

	if (data.using_shtex)
	{
		device->destroy_resource(data.shtex.texture);
	}
	else
	{
		for (int i = 0; i < NUM_BUFFERS; i++)
		{
			if (data.shmem.copy_surfaces[i] == 0)
				continue;

			if (data.shmem.texture_mapped[i])
				device->unmap_texture_region(data.shmem.copy_surfaces[i], 0);

			device->destroy_resource(data.shmem.copy_surfaces[i]);
		}
	}

	memset(&data, 0, sizeof(data));
}

static void capture_impl_shtex(reshade::api::command_queue *queue, reshade::api::resource back_buffer, reshade::api::resource_usage back_buffer_state)
{
	reshade::api::command_list *cmd_list = queue->get_immediate_command_list();

	if (data.multisampled)
	{
		cmd_list->barrier(back_buffer, back_buffer_state, reshade::api::resource_usage::resolve_source);
		cmd_list->resolve_texture_region(back_buffer, 0, nullptr, data.shtex.texture, 0, 0, 0, 0, data.format);
		cmd_list->barrier(back_buffer, reshade::api::resource_usage::resolve_source, back_buffer_state);
	}
	else
	{
		cmd_list->barrier(back_buffer, back_buffer_state, reshade::api::resource_usage::copy_source);
		cmd_list->copy_resource(back_buffer, data.shtex.texture);
		cmd_list->barrier(back_buffer, reshade::api::resource_usage::copy_source, back_buffer_state);
	}
}
static void capture_impl_shmem(reshade::api::command_queue *queue, reshade::api::resource back_buffer, reshade::api::resource_usage back_buffer_state)
{
	reshade::api::device *device = queue->get_device();
	reshade::api::command_list *cmd_list = queue->get_immediate_command_list();

	int next_tex = (data.shmem.cur_tex + 1) % NUM_BUFFERS;

	if (data.shmem.texture_ready[next_tex])
	{
		data.shmem.texture_ready[next_tex] = false;

		reshade::api::subresource_data mapped;
		if (device->map_texture_region(data.shmem.copy_surfaces[next_tex], 0, nullptr, reshade::api::map_access::read_only, &mapped))
		{
			data.shmem.texture_mapped[next_tex] = true;
			shmem_copy_data(next_tex, mapped.data);
		}
	}

	if (data.shmem.copy_wait < NUM_BUFFERS - 1)
	{
		data.shmem.copy_wait++;
	}
	else
	{
		if (shmem_texture_data_lock(data.shmem.cur_tex))
		{
			device->unmap_texture_region(data.shmem.copy_surfaces[data.shmem.cur_tex], 0);
			data.shmem.texture_mapped[data.shmem.cur_tex] = false;
			shmem_texture_data_unlock(data.shmem.cur_tex);
		}

		if (data.multisampled)
		{
			cmd_list->barrier(back_buffer, back_buffer_state, reshade::api::resource_usage::resolve_source);
			cmd_list->resolve_texture_region(back_buffer, 0, nullptr, data.shmem.copy_surfaces[data.shmem.cur_tex], 0, 0, 0, 0, data.format);
			cmd_list->barrier(back_buffer, reshade::api::resource_usage::resolve_source, back_buffer_state);
		}
		else
		{
			cmd_list->barrier(back_buffer, back_buffer_state, reshade::api::resource_usage::copy_source);
			cmd_list->copy_resource(back_buffer, data.shmem.copy_surfaces[data.shmem.cur_tex]);
			cmd_list->barrier(back_buffer, reshade::api::resource_usage::copy_source, back_buffer_state);
		}

		data.shmem.texture_ready[data.shmem.cur_tex] = true;
	}

	data.shmem.cur_tex = next_tex;
}

static void capture_impl_frame(reshade::api::effect_runtime *runtime, reshade::api::resource_usage back_buffer_state)
{
	if (capture_ready())
	{
		const reshade::api::resource back_buffer = runtime->get_current_back_buffer();

		if (data.using_shtex)
			capture_impl_shtex(runtime->get_command_queue(), back_buffer, back_buffer_state);
		else
			capture_impl_shmem(runtime->get_command_queue(), back_buffer, back_buffer_state);
	}
}

static void on_init_resource(reshade::api::device *, const reshade::api::resource_desc &desc, const reshade::api::subresource_data *, reshade::api::resource_usage, reshade::api::resource resource)
{
	if (desc.type != reshade::api::resource_type::texture_2d || (desc.flags & reshade::api::resource_flags::shared) == reshade::api::resource_flags::none)
		return;

	const std::unique_lock<std::shared_mutex> lock(s_shared_textures_mutex);
	s_shared_textures.insert(resource.handle);
}
static void on_destroy_resource(reshade::api::device *, reshade::api::resource resource)
{
	const std::unique_lock<std::shared_mutex> lock(s_shared_textures_mutex);
	s_shared_textures.erase(resource.handle);
}

// Returns true if the draw call that is about to be executed on 'cmd_list' composites a shared texture (the NUI browser surface) onto the swap chain back buffer
static bool is_nui_composite_draw(reshade::api::command_list *cmd_list, uint32_t vertex_count, uint32_t instance_count)
{
	// FiveM draws each NUI window as a single 4-vertex triangle strip quad
	if (vertex_count != 4 || instance_count > 1)
		return false;

	reshade::api::effect_runtime *const runtime = s_runtime;
	if (runtime == nullptr || runtime->get_device() != cmd_list->get_device() || runtime->get_device()->get_api() != reshade::api::device_api::d3d11)
		return false;

	{
		const std::shared_lock<std::shared_mutex> lock(s_shared_textures_mutex);
		if (s_shared_textures.empty())
			return false;
	}

	ID3D11DeviceContext *const ctx = reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native());
	if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
		return false;

	// FiveM creates the shader resource view for the NUI texture on the unwrapped device (bypassing ReShade), so query the bound views directly instead of tracking them via events
	bool samples_shared_texture = false;
	ID3D11ShaderResourceView *srvs[8] = {};
	ctx->PSGetShaderResources(0, ARRAYSIZE(srvs), srvs);
	for (ID3D11ShaderResourceView *const srv : srvs)
	{
		if (srv == nullptr)
			continue;

		if (!samples_shared_texture)
		{
			ID3D11Resource *res = nullptr;
			srv->GetResource(&res);
			if (res != nullptr)
			{
				const std::shared_lock<std::shared_mutex> lock(s_shared_textures_mutex);
				samples_shared_texture = s_shared_textures.find(reinterpret_cast<uint64_t>(res)) != s_shared_textures.end();
				res->Release();
			}
		}

		srv->Release();
	}

	if (!samples_shared_texture)
		return false;

	// Only the final composite onto the back buffer counts (FiveM also blits shared textures into intermediate render targets for script-owned DUI windows)
	bool targets_back_buffer = false;
	ID3D11RenderTargetView *rtv = nullptr;
	ctx->OMGetRenderTargets(1, &rtv, nullptr);
	if (rtv != nullptr)
	{
		ID3D11Resource *res = nullptr;
		rtv->GetResource(&res);
		if (res != nullptr)
		{
			targets_back_buffer = reinterpret_cast<uint64_t>(res) == runtime->get_current_back_buffer().handle;
			res->Release();
		}

		rtv->Release();
	}

	return targets_back_buffer;
}

static void on_nui_composite_draw()
{
	s_nui_draws_this_frame++;

	// Snapshot the frame right before the first NUI window is drawn over it
	if (s_captured_this_frame || s_runtime == nullptr)
		return;
	s_captured_this_frame = true;

	capture_impl_frame(s_runtime, reshade::api::resource_usage::render_target);
}

static bool on_draw(reshade::api::command_list *cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t, uint32_t)
{
	if (s_hide_nui && global_hook_info != nullptr && is_nui_composite_draw(cmd_list, vertex_count, instance_count))
		on_nui_composite_draw();

	return false; // Never skip the draw, so the UI stays visible to the player
}
static bool on_draw_indexed(reshade::api::command_list *cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t, int32_t, uint32_t)
{
	if (s_hide_nui && global_hook_info != nullptr && is_nui_composite_draw(cmd_list, index_count, instance_count))
		on_nui_composite_draw();

	return false;
}

static void on_reshade_begin_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *, reshade::api::resource_view, reshade::api::resource_view)
{
	if (!s_streamproof_capture || s_captured_this_frame)
		return;
	s_captured_this_frame = true;

	capture_impl_frame(runtime, reshade::api::resource_usage::render_target);
}

static void on_present(reshade::api::effect_runtime *runtime)
{
	if (global_hook_info == nullptr)
		return;

	if (capture_should_stop())
	{
		runtime->get_command_queue()->wait_idle();

		capture_impl_free(runtime->get_device());
	}

	if (capture_should_init())
		capture_impl_init(runtime->get_device(), runtime->get_device()->get_resource_desc(runtime->get_back_buffer(0)), runtime->get_hwnd());

	// Fall back to capturing the finished frame if no earlier capture point was hit (e.g. no NUI was drawn this frame and no effects are loaded)
	if (!s_captured_this_frame)
		capture_impl_frame(runtime, reshade::api::resource_usage::present);

	s_captured_this_frame = false;
	s_nui_draws_last_frame = s_nui_draws_this_frame;
	s_nui_draws_this_frame = 0;
}

static void on_init_effect_runtime(reshade::api::effect_runtime *runtime)
{
	// Only handle the first runtime (the game swap chain); FiveM's in-process CEF GPU device has no swap chain and never gets one
	if (s_runtime == nullptr)
		s_runtime = runtime;
}
static void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
{
	if (s_runtime == runtime)
		s_runtime = nullptr;

	if (global_hook_info == nullptr)
		return;

	capture_impl_free(runtime->get_device());
}

static void draw_settings(reshade::api::effect_runtime *)
{
	bool modified = false;

	modified |= ImGui::Checkbox("Streamproof capture", &s_streamproof_capture);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Send OBS the raw game frame before ReShade effects and overlay are rendered.");

	modified |= ImGui::Checkbox("Hide FiveM NUI from OBS", &s_hide_nui);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Send OBS the game frame right before FiveM draws its NUI (browser-based HUD/menus) on top.\nThe UI stays visible in-game, only the OBS capture excludes it.");

	if (s_hide_nui)
	{
		size_t shared_texture_count = 0;
		{
			const std::shared_lock<std::shared_mutex> lock(s_shared_textures_mutex);
			shared_texture_count = s_shared_textures.size();
		}

		if (s_nui_draws_last_frame != 0)
			ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "NUI detected: %u composite draw(s) last frame, capturing before the first one", s_nui_draws_last_frame);
		else if (shared_texture_count != 0)
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "No NUI composite drawn last frame (%zu shared texture(s) known), capturing at %s", shared_texture_count, s_streamproof_capture ? "begin of effects" : "present");
		else
			ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "No shared textures seen yet. Is this FiveM with 'nui_useSharedResources' enabled?");
	}

	if (modified)
	{
		reshade::set_config_value(nullptr, "OBS_CAPTURE", "StreamproofCapture", s_streamproof_capture);
		reshade::set_config_value(nullptr, "OBS_CAPTURE", "HideNUI", s_hide_nui);
	}
}

extern "C" __declspec(dllexport) const char *NAME = "OBS Streamproof Capture";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "An OBS capture driver which can send raw game frames to OBS before ReShade effects and overlay are rendered, and optionally before FiveM draws its NUI layer.";

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module)
{
	if (!reshade::register_addon(addon_module, reshade_module))
		return false;

	if (!hook_init())
	{
		reshade::unregister_addon(addon_module, reshade_module);
		return false;
	}

	reshade::get_config_value(nullptr, "OBS_CAPTURE", "StreamproofCapture", s_streamproof_capture);
	reshade::get_config_value(nullptr, "OBS_CAPTURE", "HideNUI", s_hide_nui);

	reshade::register_event<reshade::addon_event::init_resource>(on_init_resource);
	reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
	reshade::register_event<reshade::addon_event::draw>(on_draw);
	reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
	reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_reshade_begin_effects);
	reshade::register_event<reshade::addon_event::reshade_present>(on_present);
	reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init_effect_runtime);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);

	reshade::register_overlay(nullptr, draw_settings);

	return true;
}
extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module)
{
	hook_free();

	reshade::unregister_addon(addon_module, reshade_module);
}
