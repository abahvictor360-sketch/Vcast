/*
Vcast - cast to and from OBS on your local network
Copyright (C) 2026 Vcast contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "miracast-source.h"

#ifndef _WIN32

void vcast_register_miracast_source() {}

#else

#include <windows.h>
#include <appmodel.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/vec4.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/windows/window-helpers.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cwchar>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>

namespace {

/* Windows' own Miracast receiver: "Connect" on Windows 10, "Wireless Display" on 11. */
const wchar_t *kReceiverAumid = L"Microsoft.PPIProjection_cw5n1h2txyewy!Microsoft.PPIProjection";
const wchar_t *kReceiverFamily = L"Microsoft.PPIProjection_cw5n1h2txyewy";
const char *kCapability = "App.WirelessDisplay.Connect~~~~0.0.1.0";

/* Seconds between automatic starts of the Windows receiver while it is closed. */
constexpr uint64_t kRelaunchSeconds = 30;

enum class State {
	Starting,     /* asked Windows to start the receiver, waiting for its window */
	Ready,        /* receiver window found and captured */
	NotInstalled, /* Windows could not start the receiver */
	Stopped,      /* automatic start is off and the receiver is closed */
};

bool contains_nocase(const std::wstring &haystack, const wchar_t *needle)
{
	std::wstring h = haystack, n = needle;
	std::transform(h.begin(), h.end(), h.begin(), ::towlower);
	std::transform(n.begin(), n.end(), n.begin(), ::towlower);
	return h.find(n) != std::wstring::npos;
}

/* True if the process is Windows' Miracast receiver app. */
bool is_receiver_process(DWORD pid)
{
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process)
		return false;

	bool match = false;
	wchar_t family[256];
	UINT32 family_len = (UINT32)(sizeof(family) / sizeof(family[0]));
	if (GetPackageFamilyName(process, &family_len, family) == ERROR_SUCCESS)
		match = _wcsicmp(family, kReceiverFamily) == 0;

	if (!match) {
		wchar_t image[MAX_PATH];
		DWORD image_len = MAX_PATH;
		if (QueryFullProcessImageNameW(process, 0, image, &image_len)) {
			std::wstring path(image, image_len);
			match = contains_nocase(path, L"PPIProjection") || contains_nocase(path, L"WirelessDisplay");
		}
	}
	CloseHandle(process);
	return match;
}

struct FoundWindow {
	HWND frame = nullptr;   /* the top-level window (what gets captured) */
	HWND content = nullptr; /* the app's own window inside the frame */
};

BOOL CALLBACK find_receiver_proc(HWND window, LPARAM param)
{
	if (!IsWindowVisible(window))
		return TRUE;

	HWND content = window;
	if (ms_is_uwp_window(window)) {
		content = ms_get_uwp_actual_window(window);
		if (!content)
			return TRUE;
	}
	DWORD pid = 0;
	GetWindowThreadProcessId(content, &pid);
	if (!pid || !is_receiver_process(pid))
		return TRUE;

	auto *found = reinterpret_cast<FoundWindow *>(param);
	found->frame = window;
	found->content = content;
	return FALSE;
}

FoundWindow find_receiver_window()
{
	FoundWindow found;
	EnumWindows(find_receiver_proc, reinterpret_cast<LPARAM>(&found));
	return found;
}

/* "title:class:exe", the way OBS window capture names a window. */
std::string window_setting(HWND frame, HWND content)
{
	struct dstr title = {}, window_class = {}, exe = {};
	/* OBS matches UWP windows by the frame's title. */
	ms_get_window_title(&title, frame);
	ms_get_window_class(&window_class, content);
	ms_get_window_exe(&exe, content);

	std::string result;
	if (title.len) {
		for (dstr *part : {&title, &window_class, &exe}) {
			dstr_replace(part, "#", "#22");
			dstr_replace(part, ":", "#3A");
		}
		result = std::string(title.array) + ":" + (window_class.array ? window_class.array : "") + ":" +
			 (exe.array ? exe.array : "");
	}
	dstr_free(&title);
	dstr_free(&window_class);
	dstr_free(&exe);
	return result;
}

/* Height of the window's title bar in client-area pixels (0 when it has none). */
int title_bar_height(HWND frame)
{
	RECT buttons = {};
	if (FAILED(DwmGetWindowAttribute(frame, DWMWA_CAPTION_BUTTON_BOUNDS, &buttons, sizeof(buttons))) ||
	    buttons.bottom <= buttons.top)
		return 0;
	RECT window_rect;
	POINT client_origin = {0, 0};
	if (!GetWindowRect(frame, &window_rect) || !ClientToScreen(frame, &client_origin))
		return 0;
	int height = buttons.bottom + (window_rect.top - client_origin.y);
	return std::clamp(height, 0, 200);
}

/* Makes the receiver's picture area out_w x out_h and puts it behind other windows. */
void place_receiver_window(HWND frame, uint32_t out_w, uint32_t out_h)
{
	if (IsIconic(frame))
		ShowWindow(frame, SW_SHOWNOACTIVATE);
	WINDOWPLACEMENT placement = {};
	placement.length = sizeof(placement);
	if (GetWindowPlacement(frame, &placement) && placement.showCmd == SW_SHOWMAXIMIZED)
		return;

	RECT window_rect, visible;
	if (!GetWindowRect(frame, &window_rect))
		return;
	if (FAILED(DwmGetWindowAttribute(frame, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible))))
		visible = window_rect;
	/* Invisible resize borders around the visible frame. */
	int extra_w = (window_rect.right - window_rect.left) - (visible.right - visible.left);
	int extra_h = (window_rect.bottom - window_rect.top) - (visible.bottom - visible.top);

	int width = (int)out_w + extra_w;
	int height = (int)out_h + title_bar_height(frame) + extra_h;
	SetWindowPos(frame, HWND_BOTTOM, 0, 0, width, height, SWP_NOMOVE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

/* Starts Windows' receiver app. Returns false if Windows could not start it. */
bool launch_receiver(HRESULT *result)
{
	IApplicationActivationManager *manager = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
				      IID_PPV_ARGS(&manager));
	if (SUCCEEDED(hr)) {
		DWORD pid = 0;
		hr = manager->ActivateApplication(kReceiverAumid, nullptr, AO_NOERRORUI, &pid);
		manager->Release();
	}
	*result = hr;
	if (SUCCEEDED(hr))
		return true;

	/* Same app through the shell, which works in a few setups COM activation does not. */
	std::wstring target = std::wstring(L"shell:appsFolder\\") + kReceiverAumid;
	SHELLEXECUTEINFOW info = {};
	info.cbSize = sizeof(info);
	info.fMask = SEE_MASK_FLAG_NO_UI;
	info.lpFile = target.c_str();
	info.nShow = SW_SHOWNOACTIVATE;
	return ShellExecuteExW(&info) != FALSE;
}

std::string computer_name()
{
	wchar_t name[MAX_COMPUTERNAME_LENGTH + 64];
	DWORD len = (DWORD)(sizeof(name) / sizeof(name[0]));
	if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &len) || !len)
		return "this PC";
	char utf8[256];
	int n = WideCharToMultiByte(CP_UTF8, 0, name, (int)len, utf8, sizeof(utf8) - 1, nullptr, nullptr);
	return n > 0 ? std::string(utf8, (size_t)n) : std::string("this PC");
}

void parse_size(const char *text, uint32_t *width, uint32_t *height)
{
	unsigned w = 0, h = 0;
	if (text && sscanf(text, "%ux%u", &w, &h) == 2 && w && h && w <= 7680 && h <= 4320) {
		*width = w;
		*height = h;
	} else {
		*width = *height = 0;
	}
}

void reroute_audio(obs_source_t *wasapi, obs_source_t *target)
{
	proc_handler_t *ph = obs_source_get_proc_handler(wasapi);
	calldata_t cd = {};
	calldata_set_ptr(&cd, "target", target);
	proc_handler_call(ph, "reroute_audio", &cd);
	calldata_free(&cd);
}

class MiracastSource {
public:
	explicit MiracastSource(obs_source_t *source) : source_(source)
	{
		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, "method", 2); /* Windows Graphics Capture: works for app windows */
		obs_data_set_int(settings, "priority", WINDOW_PRIORITY_TITLE);
		obs_data_set_bool(settings, "client_area", true);
		obs_data_set_bool(settings, "cursor", false);
		obs_data_set_bool(settings, "capture_audio", false);
		capture_ = obs_source_create_private("window_capture", "Vcast Win+K picture", settings);
		obs_data_release(settings);
		if (capture_)
			obs_source_add_active_child(source_, capture_);
		else
			obs_log(LOG_WARNING, "Win+K receiver: OBS window capture is not available");

		worker_ = std::thread([this]() { run(); });
	}

	~MiracastSource()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stopping_ = true;
		}
		wake_.notify_all();
		if (worker_.joinable())
			worker_.join();

		set_audio(false);
		if (capture_) {
			obs_source_remove_active_child(source_, capture_);
			obs_source_release(capture_);
		}
		if (texrender_) {
			obs_enter_graphics();
			gs_texrender_destroy(texrender_);
			obs_leave_graphics();
		}
	}

	void update(obs_data_t *settings)
	{
		uint32_t w = 0, h = 0;
		parse_size(obs_data_get_string(settings, "output_size"), &w, &h);
		out_width_ = w;
		out_height_ = h;
		auto_start_ = obs_data_get_bool(settings, "auto_start");
		set_audio(obs_data_get_bool(settings, "capture_audio"));
		{
			std::lock_guard<std::mutex> lock(mutex_);
			window_.clear(); /* re-apply the window to new children */
			placed_frame_ = nullptr;
		}
		wake_.notify_all();
	}

	void start_now()
	{
		{
			std::lock_guard<std::mutex> lock(mutex_);
			launch_requested_ = true;
		}
		wake_.notify_all();
	}

	/* Shown in Program (active) or in Preview / a projector (showing). */
	void set_active(bool active)
	{
		active_ = active;
		if (active)
			wake_.notify_all();
	}

	void set_showing(bool showing)
	{
		showing_ = showing;
		if (showing)
			wake_.notify_all();
	}

	std::string status() const
	{
		switch (state_.load()) {
		case State::Ready: {
			std::string text = obs_module_text("Vcast.Miracast.Status.Ready");
			size_t at = text.find("%1");
			if (at != std::string::npos)
				text.replace(at, 2, computer_name());
			return text;
		}
		case State::NotInstalled:
			return obs_module_text("Vcast.Miracast.Status.NotInstalled");
		case State::Stopped:
			return obs_module_text("Vcast.Miracast.Status.Stopped");
		default:
			return obs_module_text("Vcast.Miracast.Status.Starting");
		}
	}

	uint32_t width() const
	{
		if (out_width_)
			return out_width_;
		uint32_t w = capture_ ? obs_source_get_width(capture_) : 0;
		return w;
	}

	uint32_t height() const
	{
		if (out_height_)
			return out_height_;
		uint32_t h = capture_ ? obs_source_get_height(capture_) : 0;
		int crop = crop_top_;
		return h > (uint32_t)crop ? h - (uint32_t)crop : h;
	}

	void render()
	{
		uint32_t out_w = width(), out_h = height();
		if (!out_w || !out_h)
			return;

		/* Black bars wherever the cast does not reach. */
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		vec4 black;
		vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
		gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &black);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, out_w, out_h);

		if (!capture_ || state_ != State::Ready)
			return;
		uint32_t cw = obs_source_get_width(capture_);
		uint32_t ch = obs_source_get_height(capture_);
		uint32_t crop = std::min<uint32_t>((uint32_t)std::max(0, crop_top_.load()), ch / 4);
		if (cw < 2 || ch <= crop + 2)
			return;
		uint32_t src_h = ch - crop;

		if (!texrender_)
			texrender_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		gs_texrender_reset(texrender_);
		if (!gs_texrender_begin(texrender_, cw, ch))
			return;
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(capture_);
		gs_blend_state_pop();
		gs_texrender_end(texrender_);

		gs_texture_t *tex = gs_texrender_get_texture(texrender_);
		if (!tex)
			return;

		/* Scale to fit, centred, keeping the shape. */
		float scale = std::min((float)out_w / (float)cw, (float)out_h / (float)src_h);
		float draw_w = (float)cw * scale, draw_h = (float)src_h * scale;

		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), tex);
		gs_matrix_push();
		gs_matrix_translate3f(((float)out_w - draw_w) * 0.5f, ((float)out_h - draw_h) * 0.5f, 0.0f);
		gs_matrix_scale3f(scale, scale, 1.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite_subregion(tex, 0, 0, crop, cw, src_h);
		gs_matrix_pop();
		gs_enable_framebuffer_srgb(previous_srgb);
	}

	void enum_children(obs_source_enum_proc_t enum_callback, void *param)
	{
		if (capture_)
			enum_callback(source_, capture_, param);
	}

private:
	void set_audio(bool enabled)
	{
		std::lock_guard<std::mutex> lock(audio_mutex_);
		if (enabled && !audio_) {
			audio_ = obs_source_create_private("wasapi_process_output_capture", "Vcast Win+K sound",
							   nullptr);
			if (!audio_)
				return;
			obs_source_add_active_child(source_, audio_);
			reroute_audio(audio_, source_);
			obs_source_set_audio_active(source_, true);
		} else if (!enabled && audio_) {
			obs_source_set_audio_active(source_, false);
			reroute_audio(audio_, nullptr);
			obs_source_remove_active_child(source_, audio_);
			obs_source_release(audio_);
			audio_ = nullptr;
		}
	}

	void point_children_at(const std::string &window)
	{
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "window", window.c_str());
		obs_data_set_int(settings, "priority", WINDOW_PRIORITY_TITLE);
		if (capture_)
			obs_source_update(capture_, settings);
		{
			std::lock_guard<std::mutex> lock(audio_mutex_);
			if (audio_)
				obs_source_update(audio_, settings);
		}
		obs_data_release(settings);
	}

	void run()
	{
		HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		uint64_t last_launch = 0;
		std::unique_lock<std::mutex> lock(mutex_);
		while (!stopping_) {
			bool launch_requested = launch_requested_;
			launch_requested_ = false;
			lock.unlock();

			FoundWindow found = find_receiver_window();
			uint64_t now = os_gettime_ns() / 1000000000ULL;
			if (found.frame) {
				std::string window = window_setting(found.frame, found.content);
				crop_top_ = title_bar_height(found.frame);
				bool changed, place;
				{
					std::lock_guard<std::mutex> guard(mutex_);
					changed = !window.empty() && window != window_;
					if (changed)
						window_ = window;
					place = placed_frame_ != found.frame;
					placed_frame_ = found.frame;
				}
				if (changed) {
					obs_log(LOG_INFO, "Win+K receiver: capturing \"%s\"", window.c_str());
					point_children_at(window);
				}
				if (place && out_width_ && out_height_)
					place_receiver_window(found.frame, out_width_, out_height_);
				else if (IsIconic(found.frame))
					ShowWindow(found.frame,
						   SW_SHOWNOACTIVATE); /* minimised windows can't be captured */
				state_ = State::Ready;
			} else {
				{
					std::lock_guard<std::mutex> guard(mutex_);
					placed_frame_ = nullptr;
				}
				bool want = launch_requested ||
					    (auto_start_ && (active_ || showing_) &&
					     (last_launch == 0 || now - last_launch >= kRelaunchSeconds));
				if (want) {
					last_launch = now;
					HRESULT hr = S_OK;
					if (launch_receiver(&hr)) {
						state_ = State::Starting;
					} else {
						obs_log(LOG_WARNING,
							"Win+K receiver: Windows could not start Wireless Display (0x%08lX). "
							"Is the optional feature %s installed?",
							(unsigned long)hr, kCapability);
						state_ = State::NotInstalled;
					}
				} else if (state_ == State::Ready) {
					state_ = auto_start_ ? State::Starting : State::Stopped;
				} else if (!auto_start_ && state_ != State::NotInstalled) {
					state_ = State::Stopped;
				}
			}

			lock.lock();
			wake_.wait_for(lock, std::chrono::seconds(1),
				       [this]() { return stopping_ || launch_requested_; });
		}
		lock.unlock();
		if (SUCCEEDED(com))
			CoUninitialize();
	}

	obs_source_t *source_;
	obs_source_t *capture_ = nullptr;
	obs_source_t *audio_ = nullptr;
	std::mutex audio_mutex_;
	gs_texrender_t *texrender_ = nullptr;

	std::atomic<uint32_t> out_width_{1920};
	std::atomic<uint32_t> out_height_{1080};
	std::atomic<bool> auto_start_{true};
	std::atomic<bool> active_{false};
	std::atomic<bool> showing_{false};
	std::atomic<int> crop_top_{0};
	std::atomic<State> state_{State::Starting};

	std::mutex mutex_;
	std::condition_variable wake_;
	bool stopping_ = false;
	bool launch_requested_ = false;
	std::string window_;
	HWND placed_frame_ = nullptr;
	std::thread worker_;
};

const char *miracast_name(void *)
{
	return obs_module_text("Vcast.Miracast.Name");
}

void *miracast_create(obs_data_t *settings, obs_source_t *source)
{
	auto *self = new MiracastSource(source);
	self->update(settings);
	return self;
}

void miracast_destroy(void *data)
{
	delete static_cast<MiracastSource *>(data);
}

void miracast_update(void *data, obs_data_t *settings)
{
	static_cast<MiracastSource *>(data)->update(settings);
}

void miracast_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "output_size", "1920x1080");
	obs_data_set_default_bool(settings, "auto_start", true);
	obs_data_set_default_bool(settings, "capture_audio", true);
}

bool start_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	static_cast<MiracastSource *>(data)->start_now();
	return false;
}

bool install_clicked(obs_properties_t *, obs_property_t *, void *)
{
	/* Adding the optional feature needs administrator rights and downloads from Windows Update. */
	std::string script = std::string("Write-Host 'Installing Windows Wireless Display for Vcast. "
					 "This downloads from Windows Update and can take a few minutes...'; "
					 "Add-WindowsCapability -Online -Name '") +
			     kCapability +
			     "' | Out-Null; Write-Host 'Done. Restart OBS if Win+K casting does not work yet.'; "
			     "Start-Sleep -Seconds 8";
	std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -Command \"";
	args += std::wstring(script.begin(), script.end());
	args += L"\"";
	ShellExecuteW(nullptr, L"runas", L"powershell.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
	return false;
}

bool settings_clicked(obs_properties_t *, obs_property_t *, void *)
{
	ShellExecuteW(nullptr, L"open", L"ms-settings:project", nullptr, nullptr, SW_SHOWNORMAL);
	return false;
}

bool refresh_clicked(obs_properties_t *props, obs_property_t *, void *data)
{
	obs_property_t *status = obs_properties_get(props, "status");
	if (status)
		obs_property_set_description(status, static_cast<MiracastSource *>(data)->status().c_str());
	return true;
}

obs_properties_t *miracast_properties(void *data)
{
	auto *self = static_cast<MiracastSource *>(data);
	obs_properties_t *props = obs_properties_create();

	std::string help = obs_module_text("Vcast.Miracast.Help");
	size_t at = help.find("%1");
	if (at != std::string::npos)
		help.replace(at, 2, computer_name());
	obs_property_t *info = obs_properties_add_text(props, "help", help.c_str(), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(info, true);

	obs_property_t *status = obs_properties_add_text(
		props, "status", self ? self->status().c_str() : obs_module_text("Vcast.Miracast.Status.Starting"),
		OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(status, true);
	obs_properties_add_button2(props, "refresh", obs_module_text("Vcast.Receiver.Refresh"), refresh_clicked, data);
	obs_properties_add_button2(props, "start", obs_module_text("Vcast.Miracast.Start"), start_clicked, data);

	obs_property_t *size = obs_properties_add_list(props, "output_size",
						       obs_module_text("Vcast.Receiver.OutputSize"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(size, "1920 x 1080 (Full HD)", "1920x1080");
	obs_property_list_add_string(size, "1280 x 720 (HD)", "1280x720");
	obs_property_list_add_string(size, "2560 x 1440", "2560x1440");
	obs_property_list_add_string(size, "3840 x 2160 (4K)", "3840x2160");
	obs_property_list_add_string(size, obs_module_text("Vcast.Receiver.OutputSize.Device"), "device");
	obs_property_set_long_description(size, obs_module_text("Vcast.Receiver.OutputSize.Tip"));

	obs_properties_add_bool(props, "auto_start", obs_module_text("Vcast.Miracast.AutoStart"));
	obs_properties_add_bool(props, "capture_audio", obs_module_text("Vcast.Miracast.Sound"));
	obs_properties_add_button2(props, "install", obs_module_text("Vcast.Miracast.Install"), install_clicked, data);
	obs_properties_add_button2(props, "projection_settings", obs_module_text("Vcast.Miracast.Settings"),
				   settings_clicked, data);
	return props;
}

void miracast_activate(void *data)
{
	static_cast<MiracastSource *>(data)->set_active(true);
}

void miracast_deactivate(void *data)
{
	static_cast<MiracastSource *>(data)->set_active(false);
}

void miracast_show(void *data)
{
	static_cast<MiracastSource *>(data)->set_showing(true);
}

void miracast_hide(void *data)
{
	static_cast<MiracastSource *>(data)->set_showing(false);
}

void miracast_render(void *data, gs_effect_t *)
{
	static_cast<MiracastSource *>(data)->render();
}

uint32_t miracast_width(void *data)
{
	return static_cast<MiracastSource *>(data)->width();
}

uint32_t miracast_height(void *data)
{
	return static_cast<MiracastSource *>(data)->height();
}

void miracast_enum_active(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	static_cast<MiracastSource *>(data)->enum_children(enum_callback, param);
}

} // namespace

void vcast_register_miracast_source()
{
	obs_source_info info = {};
	info.id = VCAST_MIRACAST_SOURCE_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB |
			    OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE;
	info.get_name = miracast_name;
	info.create = miracast_create;
	info.destroy = miracast_destroy;
	info.update = miracast_update;
	info.get_defaults = miracast_defaults;
	info.get_properties = miracast_properties;
	info.activate = miracast_activate;
	info.deactivate = miracast_deactivate;
	info.show = miracast_show;
	info.hide = miracast_hide;
	info.video_render = miracast_render;
	info.get_width = miracast_width;
	info.get_height = miracast_height;
	info.enum_active_sources = miracast_enum_active;
	obs_register_source(&info);
}

#endif
