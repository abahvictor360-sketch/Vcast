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

#include "receiver-source.h"
#include "app-ingest.h"
#include "container-ingest.h"
#include "receiver.h"
#include "web-server.h"

#ifdef VCAST_HAVE_AIRPLAY
#include "vcast-airplay.h"
#endif

#include "net-util.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace vcast {

namespace {

constexpr uint16_t kWebPort = 8443;
#ifdef VCAST_HAVE_AIRPLAY
constexpr uint16_t kAirPlayPortBase = 7000;
#endif

std::string random_id()
{
	std::random_device rd;
	std::mt19937_64 gen(((uint64_t)rd() << 32) ^ rd() ^ os_gettime_ns());
	const char *hex = "0123456789abcdef";
	std::string id;
	uint64_t v = gen();
	for (int i = 0; i < 12; i++, v >>= 4)
		id += hex[v & 0xF];
	return id;
}

std::string computer_name()
{
	char buf[256] = {};
	if (gethostname(buf, sizeof(buf) - 1) != 0 || !buf[0])
		return "OBS";
	std::string name = buf;
	size_t dot = name.find('.');
	if (dot != std::string::npos && dot > 0)
		name.resize(dot);
	return name;
}

std::string read_module_file(const char *relative)
{
	char *path = obs_module_file(relative);
	if (!path)
		return std::string();
	std::ifstream f(path, std::ios::binary);
	bfree(path);
	if (!f)
		return std::string();
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

} // namespace

class ReceiverSource;

/* Services shared by every receiver source: the HTTPS cast page and app discovery. */
class ReceiverHub {
public:
	static ReceiverHub &get()
	{
		static ReceiverHub hub;
		return hub;
	}

	int add(const std::shared_ptr<ReceiverSource> &receiver);
	void remove(const ReceiverSource *receiver);
	std::vector<std::shared_ptr<ReceiverSource>> list();
	std::shared_ptr<ReceiverSource> find(const std::string &id);

	uint16_t web_port() const { return web_.running() ? web_.port() : 0; }
	void shutdown();

private:
	void start_services();
	void stop_services();
	HttpReply handle_http(const HttpRequest &req);
	std::shared_ptr<WebSocketHandler> handle_ws(const HttpRequest &req);

	std::mutex mutex_;
	std::vector<std::shared_ptr<ReceiverSource>> receivers_;
	WebServer web_;
	DiscoveryResponder discovery_;
	std::string page_;
};

class ReceiverSource : public Receiver, public std::enable_shared_from_this<ReceiverSource> {
public:
	explicit ReceiverSource(obs_source_t *source) : Receiver(source), app_(this) {}

	std::string name() const override
	{
		std::lock_guard<std::mutex> lock(config_mutex_);
		return name_;
	}
	std::string id() const override
	{
		std::lock_guard<std::mutex> lock(config_mutex_);
		return id_;
	}

	bool browser_enabled() const { return browser_; }
	bool android_enabled() const { return android_ && app_.port() != 0; }
	uint16_t app_port() const { return app_.port(); }

	void apply(obs_data_t *settings)
	{
		std::string id = obs_data_get_string(settings, "receiver_id");
		if (id.empty()) {
			id = random_id();
			obs_data_set_string(settings, "receiver_id", id.c_str());
		}
		std::string name = obs_data_get_string(settings, "receiver_name");
		if (name.empty())
			name = vcast_receiver_default_name();

		bool airplay = obs_data_get_bool(settings, "airplay");
		bool android = obs_data_get_bool(settings, "android");
		bool browser = obs_data_get_bool(settings, "browser");
		set_keep_last_frame(obs_data_get_bool(settings, "keep_last_frame"));

		bool name_changed;
		{
			std::lock_guard<std::mutex> lock(config_mutex_);
			name_changed = name != name_;
			name_ = name;
			id_ = id;
		}
		browser_ = browser;

		if (android && !android_) {
			std::string error;
			if (!app_.start(kAppIngestPort, 20, &error))
				obs_log(LOG_WARNING, "[%s] %s", name.c_str(), error.c_str());
		} else if (!android && android_) {
			app_.stop();
		}
		android_ = android;

		if (airplay_enabled_ != airplay || (airplay && name_changed))
			restart_airplay(airplay);
	}

	void set_slot(int slot) { slot_ = slot; }
	int slot() const { return slot_; }

	void shutdown()
	{
		restart_airplay(false);
		app_.stop();
		close();
	}

	std::string status() const
	{
		std::string sender = current_sender();
		if (!sender.empty())
			return std::string(obs_module_text("Vcast.Receiver.Status.Receiving")) + " " + sender;
		return obs_module_text("Vcast.Receiver.Status.Waiting");
	}

	std::string airplay_error() const
	{
		std::lock_guard<std::mutex> lock(config_mutex_);
		return airplay_error_;
	}
	bool airplay_running() const { return airplay_running_; }

private:
	void restart_airplay(bool enable)
	{
#ifdef VCAST_HAVE_AIRPLAY
		if (airplay_) {
			vcast_airplay_destroy(airplay_);
			airplay_ = nullptr;
			airplay_running_ = false;
			end_session(airplay_session_.exchange(0));
		}
		airplay_enabled_ = enable;
		if (!enable)
			return;

		vcast_airplay_callbacks cb = {};
		cb.opaque = this;
		cb.on_connect = [](void *opaque, const char *device) {
			auto *self = static_cast<ReceiverSource *>(opaque);
			self->airplay_begin(device ? device : "Apple device");
		};
		cb.on_disconnect = [](void *opaque) {
			auto *self = static_cast<ReceiverSource *>(opaque);
			self->end_session(self->airplay_session_.exchange(0));
		};
		cb.on_video = [](void *opaque, int codec, const uint8_t *data, size_t size, uint64_t ts_ns, int) {
			auto *self = static_cast<ReceiverSource *>(opaque);
			uint64_t session = self->airplay_session_for_data();
			if (!session)
				return;
			AVCodecID id = codec == VCAST_AIRPLAY_VIDEO_H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
			self->feed(session, [&](StreamDecoder &decoder) {
				decoder.configure_video(id, nullptr, 0);
				decoder.decode_video(data, size, ts_ns ? (int64_t)(ts_ns / 1000) : -1);
			});
		};
		cb.on_audio_format = [](void *opaque, int codec, int rate, int channels, const uint8_t *config,
					size_t config_size) {
			auto *self = static_cast<ReceiverSource *>(opaque);
			uint64_t session = self->airplay_session_for_data();
			if (!session)
				return;
			AVCodecID id = codec == VCAST_AIRPLAY_AUDIO_ALAC ? AV_CODEC_ID_ALAC : AV_CODEC_ID_AAC;
			self->feed(session, [&](StreamDecoder &decoder) {
				decoder.configure_audio(id, rate, channels, config, config_size);
			});
		};
		cb.on_audio = [](void *opaque, const uint8_t *data, size_t size, uint64_t ts_ns) {
			auto *self = static_cast<ReceiverSource *>(opaque);
			uint64_t session = self->airplay_session_for_data();
			if (!session)
				return;
			self->feed(session, [&](StreamDecoder &decoder) {
				decoder.decode_audio(data, size, ts_ns ? (int64_t)(ts_ns / 1000) : -1);
			});
		};
		cb.on_log = [](void *opaque, int level, const char *msg) {
			auto *self = static_cast<ReceiverSource *>(opaque);
			int obs_level = level >= 3   ? LOG_ERROR
					: level == 2 ? LOG_WARNING
					: level == 1 ? LOG_INFO
						     : LOG_DEBUG;
			obs_log(obs_level, "[%s] AirPlay: %s", self->name().c_str(), msg);
		};

		char err[256] = {};
		uint16_t base = (uint16_t)(kAirPlayPortBase + slot_ * 10);
		airplay_ = vcast_airplay_create(name().c_str(), base, &cb, err, sizeof(err));
		airplay_running_ = airplay_ != nullptr;
		{
			std::lock_guard<std::mutex> lock(config_mutex_);
			airplay_error_ = airplay_ ? std::string() : std::string(err);
		}
		if (!airplay_)
			obs_log(LOG_WARNING, "[%s] AirPlay could not start: %s", name().c_str(), err);
#else
		airplay_enabled_ = enable;
#endif
	}

#ifdef VCAST_HAVE_AIRPLAY
	void airplay_begin(const std::string &device)
	{
		uint64_t session = begin_session("AirPlay", device, nullptr);
		uint64_t old = airplay_session_.exchange(session);
		(void)old;
		airplay_device_ = device;
	}

	/* If another sender took over and then left, an AirPlay sender that is still
	 * mirroring becomes the active one again. */
	uint64_t airplay_session_for_data()
	{
		uint64_t session = airplay_session_;
		if (session && is_active(session))
			return session;
		if (current_sender().empty()) {
			airplay_begin(airplay_device_.empty() ? std::string("Apple device") : airplay_device_);
			return airplay_session_;
		}
		return 0;
	}

	vcast_airplay_t *airplay_ = nullptr;
	std::atomic<uint64_t> airplay_session_{0};
	std::string airplay_device_;
#endif

	mutable std::mutex config_mutex_;
	std::string name_;
	std::string id_;
	std::string airplay_error_;
	std::atomic<bool> browser_{true};
	bool android_ = false;
	bool airplay_enabled_ = false;
	std::atomic<bool> airplay_running_{false};
	std::atomic<int> slot_{-1};
	AppIngestServer app_;
};

/* ------------------------------------------------------------------------- */
/* Hub                                                                        */

int ReceiverHub::add(const std::shared_ptr<ReceiverSource> &receiver)
{
	bool first;
	int slot;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		first = receivers_.empty();
		/* Lowest free slot, so a receiver's ports don't collide after others are removed. */
		slot = 0;
		while (std::any_of(receivers_.begin(), receivers_.end(),
				   [&](const std::shared_ptr<ReceiverSource> &r) { return r->slot() == slot; }))
			slot++;
		receiver->set_slot(slot);
		receivers_.push_back(receiver);
	}
	if (first)
		start_services();
	return slot;
}

void ReceiverHub::remove(const ReceiverSource *receiver)
{
	bool last;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		receivers_.erase(
			std::remove_if(receivers_.begin(), receivers_.end(),
				       [&](const std::shared_ptr<ReceiverSource> &r) { return r.get() == receiver; }),
			receivers_.end());
		last = receivers_.empty();
	}
	if (last)
		stop_services();
}

std::vector<std::shared_ptr<ReceiverSource>> ReceiverHub::list()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return receivers_;
}

std::shared_ptr<ReceiverSource> ReceiverHub::find(const std::string &id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &r : receivers_)
		if (r->id() == id)
			return r;
	return nullptr;
}

void ReceiverHub::start_services()
{
	page_ = read_module_file("web/index.html");
	if (page_.empty())
		page_ = "<!doctype html><title>Vcast</title><p>The Vcast cast page is missing from the plugin's data folder.";

	web_.set_http_handler([this](const HttpRequest &req) { return handle_http(req); });
	web_.set_ws_handler([this](const HttpRequest &req) { return handle_ws(req); });

	char *config_dir = obs_module_config_path("");
	std::string cert_dir = config_dir ? config_dir : ".";
	bfree(config_dir);
	os_mkdirs(cert_dir.c_str());
	if (!cert_dir.empty() && (cert_dir.back() == '/' || cert_dir.back() == '\\'))
		cert_dir.pop_back();

	std::string error;
	if (!web_.start(kWebPort, 10, cert_dir, &error))
		obs_log(LOG_WARNING, "Laptop casting is unavailable: %s", error.c_str());

	discovery_.start([this]() {
		std::vector<std::pair<std::string, uint16_t>> entries;
		for (auto &r : list())
			if (r->android_enabled())
				entries.emplace_back(r->name(), r->app_port());
		return entries;
	});
}

void ReceiverHub::stop_services()
{
	discovery_.stop();
	web_.stop();
}

void ReceiverHub::shutdown()
{
	std::vector<std::shared_ptr<ReceiverSource>> receivers;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		receivers.swap(receivers_);
	}
	for (auto &r : receivers)
		r->shutdown();
	stop_services();
}

HttpReply ReceiverHub::handle_http(const HttpRequest &req)
{
	HttpReply reply;
	if (req.path == "/" || req.path == "/index.html") {
		reply.content_type = "text/html; charset=utf-8";
		reply.body = page_;
		return reply;
	}
	if (req.path == "/api/receivers") {
		std::string json = "{\"receivers\":[";
		bool first = true;
		for (auto &r : list()) {
			if (!r->browser_enabled())
				continue;
			if (!first)
				json += ",";
			first = false;
			json += "{\"id\":\"" + json_escape(r->id()) + "\",\"name\":\"" + json_escape(r->name()) +
				"\",\"busy\":\"" + json_escape(r->current_sender()) + "\"}";
		}
		char *apk = obs_module_file("android/vcast-android.apk");
		json += std::string("],\"android_app\":") + (apk ? "true" : "false") + "}";
		bfree(apk);
		reply.content_type = "application/json";
		reply.body = json;
		return reply;
	}
	if (req.path == "/android" || req.path == "/vcast-android.apk") {
		char *apk = obs_module_file("android/vcast-android.apk");
		if (apk) {
			std::ifstream f(apk, std::ios::binary);
			bfree(apk);
			std::ostringstream ss;
			ss << f.rdbuf();
			reply.content_type = "application/vnd.android.package-archive";
			reply.headers.push_back({"Content-Disposition", "attachment; filename=\"vcast-android.apk\""});
			reply.body = ss.str();
			return reply;
		}
		reply.status = 404;
		reply.content_type = "text/html; charset=utf-8";
		reply.body =
			"<!doctype html><meta name=viewport content='width=device-width'><p>The Android app is not "
			"bundled with this build. Download it from the Vcast releases page.";
		return reply;
	}
	reply.status = 404;
	reply.body = "Not found\n";
	return reply;
}

std::shared_ptr<WebSocketHandler> ReceiverHub::handle_ws(const HttpRequest &req)
{
	if (req.path != "/ws/cast")
		return nullptr;
	std::shared_ptr<ReceiverSource> receiver = find(req.query_param("receiver"));
	if (!receiver || !receiver->browser_enabled())
		return nullptr;

	/* Lets another sender close this socket safely even while it is going away. */
	struct Closer {
		std::mutex mutex;
		WebSocket *socket = nullptr;
	};
	struct State {
		uint64_t session = 0;
		std::unique_ptr<ContainerIngest> ingest;
		std::string device;
		std::shared_ptr<Closer> closer = std::make_shared<Closer>();
	};
	auto state = std::make_shared<State>();
	state->device = req.peer;
	auto handler = std::make_shared<WebSocketHandler>();

	handler->on_text = [receiver, state](WebSocket &ws, const std::string &text) {
		/* {"type":"hello","device":"Chrome on Windows"} */
		size_t pos = text.find("\"device\"");
		if (pos != std::string::npos) {
			size_t q1 = text.find('"', text.find(':', pos) + 1);
			size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
			if (q2 != std::string::npos)
				state->device = text.substr(q1 + 1, std::min<size_t>(q2 - q1 - 1, 64));
		}
		if (state->session)
			receiver->set_device_name(state->session, state->device);
		ws.send_text("{\"type\":\"welcome\",\"name\":\"" + json_escape(receiver->name()) + "\"}");
		return true;
	};
	handler->on_binary = [receiver, state](WebSocket &ws, const uint8_t *data, size_t size) {
		if (!state->session) {
			{
				std::lock_guard<std::mutex> lock(state->closer->mutex);
				state->closer->socket = &ws;
			}
			std::shared_ptr<Closer> closer = state->closer;
			state->session = receiver->begin_session("browser", state->device, [closer]() {
				std::lock_guard<std::mutex> lock(closer->mutex);
				if (closer->socket) {
					closer->socket->send_text("{\"type\":\"replaced\"}");
					closer->socket->close();
				}
			});
			state->ingest = std::make_unique<ContainerIngest>(receiver, state->session,
									  receiver->name() + " / " + state->device);
			state->ingest->start();
		}
		if (!receiver->is_active(state->session))
			return false;
		if (!state->ingest->push(data, size)) {
			ws.send_text("{\"type\":\"error\",\"message\":\"The stream could not be decoded.\"}");
			return false;
		}
		return true;
	};
	handler->on_close = [receiver, state](WebSocket &) {
		{
			std::lock_guard<std::mutex> lock(state->closer->mutex);
			state->closer->socket = nullptr;
		}
		if (state->ingest)
			state->ingest->finish();
		state->ingest.reset();
		receiver->end_session(state->session);
	};
	return handler;
}

} // namespace vcast

/* ------------------------------------------------------------------------- */
/* OBS source glue                                                             */

using vcast::ReceiverHub;
using vcast::ReceiverSource;

namespace {

struct SourceData {
	std::shared_ptr<ReceiverSource> receiver;
	obs_source_t *source = nullptr;
};

const char *receiver_get_name(void *)
{
	return obs_module_text("Vcast.Receiver.Name");
}

void receiver_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "receiver_name", vcast_receiver_default_name().c_str());
	obs_data_set_default_bool(settings, "airplay", true);
	obs_data_set_default_bool(settings, "android", true);
	obs_data_set_default_bool(settings, "browser", true);
	obs_data_set_default_bool(settings, "keep_last_frame", false);
}

void *receiver_create(obs_data_t *settings, obs_source_t *source)
{
	auto *data = new SourceData();
	data->source = source;
	data->receiver = std::make_shared<ReceiverSource>(source);
	obs_source_set_async_unbuffered(source, true);
	ReceiverHub::get().add(data->receiver);
	data->receiver->apply(settings);
	return data;
}

void receiver_destroy(void *ptr)
{
	auto *data = static_cast<SourceData *>(ptr);
	ReceiverHub::get().remove(data->receiver.get());
	data->receiver->shutdown();
	delete data;
}

void receiver_update(void *ptr, obs_data_t *settings)
{
	static_cast<SourceData *>(ptr)->receiver->apply(settings);
}

bool fit_clicked(obs_properties_t *, obs_property_t *, void *ptr)
{
	auto *data = static_cast<SourceData *>(ptr);
	if (data)
		vcast_receiver_fit_to_screen(data->source, false);
	return false;
}

bool refresh_status(obs_properties_t *props, obs_property_t *, void *ptr)
{
	auto *data = static_cast<SourceData *>(ptr);
	obs_property_t *status = obs_properties_get(props, "status");
	if (status && data)
		obs_property_set_description(status, data->receiver->status().c_str());
	return true;
}

obs_properties_t *receiver_properties(void *ptr)
{
	auto *data = static_cast<SourceData *>(ptr);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "receiver_name", obs_module_text("Vcast.Receiver.ReceiverName"),
				OBS_TEXT_DEFAULT);

	std::string name = data ? data->receiver->name() : vcast_receiver_default_name();
	std::string web = vcast_receiver_web_url();
	std::string help = std::string(obs_module_text("Vcast.Receiver.Help.Title")) + "\n\n" +
			   obs_module_text("Vcast.Receiver.Help.Apple") + "\n    " +
			   obs_module_text("Vcast.Receiver.Help.AppleSteps") + " \"" + name + "\"\n\n" +
			   obs_module_text("Vcast.Receiver.Help.Laptop") + "\n    " +
			   (web.empty() ? std::string("-") : web) + "\n\n" +
			   obs_module_text("Vcast.Receiver.Help.Android") + "\n    " +
			   (web.empty() ? std::string("-") : web + "android");
#ifndef VCAST_HAVE_AIRPLAY
	help += std::string("\n\n") + obs_module_text("Vcast.Receiver.Help.NoAirPlay");
#else
	if (data && !data->receiver->airplay_error().empty())
		help += "\n\nAirPlay: " + data->receiver->airplay_error();
#endif
	obs_property_t *info = obs_properties_add_text(props, "help", help.c_str(), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(info, true);

	obs_property_t *status = obs_properties_add_text(props, "status",
							 data ? data->receiver->status().c_str()
							      : obs_module_text("Vcast.Receiver.Status.Waiting"),
							 OBS_TEXT_INFO);
	obs_property_text_set_info_type(status, OBS_TEXT_INFO_NORMAL);
	obs_properties_add_button2(props, "refresh", obs_module_text("Vcast.Receiver.Refresh"), refresh_status, data);

	obs_properties_add_bool(props, "airplay", obs_module_text("Vcast.Receiver.AirPlay"));
	obs_properties_add_bool(props, "android", obs_module_text("Vcast.Receiver.Android"));
	obs_properties_add_bool(props, "browser", obs_module_text("Vcast.Receiver.Browser"));
	obs_properties_add_bool(props, "keep_last_frame", obs_module_text("Vcast.Receiver.KeepLastFrame"));
	obs_properties_add_button2(props, "fit", obs_module_text("Vcast.FitToScreen"), fit_clicked, data);
	return props;
}

} // namespace

void vcast_register_receiver_source()
{
	obs_source_info info = {};
	info.id = VCAST_RECEIVER_SOURCE_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	info.get_name = receiver_get_name;
	info.create = receiver_create;
	info.destroy = receiver_destroy;
	info.update = receiver_update;
	info.get_defaults = receiver_defaults;
	info.get_properties = receiver_properties;
	obs_register_source(&info);
}

void vcast_receiver_shutdown()
{
	ReceiverHub::get().shutdown();
}

namespace {

struct FitContext {
	obs_source_t *only;
	bool only_new;
	int count;
};

void fit_item(obs_sceneitem_t *item)
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;
	vec2 pos;
	vec2_zero(&pos);
	vec2 bounds;
	vec2_set(&bounds, (float)ovi.base_width, (float)ovi.base_height);
	obs_sceneitem_crop crop = {};

	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_set_rot(item, 0.0f);
	obs_sceneitem_set_crop(item, &crop);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_defer_update_end(item);

	obs_data_t *priv = obs_sceneitem_get_private_settings(item);
	obs_data_set_bool(priv, "vcast_fitted", true);
	obs_data_release(priv);
}

bool fit_scene_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<FitContext *>(param);
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, fit_scene_item, param);
		return true;
	}
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source || strcmp(obs_source_get_id(source), VCAST_RECEIVER_SOURCE_ID) != 0)
		return true;
	if (ctx->only && source != ctx->only)
		return true;
	if (ctx->only_new) {
		obs_data_t *priv = obs_sceneitem_get_private_settings(item);
		bool fitted = obs_data_get_bool(priv, "vcast_fitted");
		obs_data_release(priv);
		if (fitted)
			return true;
	}
	fit_item(item);
	ctx->count++;
	return true;
}

} // namespace

int vcast_receiver_fit_to_screen(obs_source_t *only, bool only_new)
{
	FitContext ctx = {only, only_new, 0};
	obs_enum_scenes(
		[](void *param, obs_source_t *scene_source) {
			obs_scene_t *scene = obs_scene_from_source(scene_source);
			if (scene)
				obs_scene_enum_items(scene, fit_scene_item, param);
			return true;
		},
		&ctx);
	return ctx.count;
}

std::string vcast_receiver_web_url()
{
	uint16_t port = ReceiverHub::get().web_port();
	if (!port)
		return std::string();
	return "https://" + vcast::primary_local_ip() + ":" + std::to_string(port) + "/";
}

std::string vcast_receiver_default_name()
{
	return "Vcast (" + vcast::computer_name() + ")";
}
