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

#include "receiver.h"

#include <plugin-support.h>

namespace vcast {

uint64_t Receiver::begin_session(const std::string &kind, const std::string &device, std::function<void()> stop)
{
	std::function<void()> previous_stop;
	uint64_t session;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		previous_stop = std::move(stop_);
		session = next_session_++;
		active_ = session;
		kind_ = kind;
		device_ = device;
		stop_ = std::move(stop);
	}
	if (previous_stop)
		previous_stop();
	std::shared_lock<std::shared_mutex> alive_lock(alive_mutex_);
	if (alive_)
		decoder_.reset(false);
	obs_log(LOG_INFO, "[%s] %s connected via %s", name().c_str(), device.empty() ? "a device" : device.c_str(),
		kind.c_str());
	return session;
}

bool Receiver::is_active(uint64_t session) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return session != 0 && session == active_;
}

void Receiver::end_session(uint64_t session)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (session == 0 || session != active_)
			return;
		active_ = 0;
		stop_ = nullptr;
		obs_log(LOG_INFO, "[%s] %s disconnected", name().c_str(), device_.c_str());
		kind_.clear();
		device_.clear();
	}
	std::shared_lock<std::shared_mutex> alive_lock(alive_mutex_);
	if (alive_)
		decoder_.reset(!keep_last_frame_);
}

void Receiver::set_device_name(uint64_t session, const std::string &device)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (session == active_)
		device_ = device;
}

void Receiver::close()
{
	std::function<void()> stop;
	{
		std::unique_lock<std::shared_mutex> alive_lock(alive_mutex_);
		alive_ = false;
		std::lock_guard<std::mutex> lock(mutex_);
		stop = std::move(stop_);
		active_ = 0;
	}
	if (stop)
		stop();
	decoder_.reset(true);
}

std::string Receiver::current_sender() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!active_)
		return std::string();
	return (device_.empty() ? std::string("Device") : device_) + " (" + kind_ + ")";
}

} // namespace vcast
