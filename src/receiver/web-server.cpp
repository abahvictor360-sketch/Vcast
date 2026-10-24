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

#include "web-server.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha1.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <errno.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#endif

namespace vcast {

namespace {

constexpr size_t kMaxRequestBytes = 16 * 1024;
constexpr size_t kMaxMessageBytes = 16 * 1024 * 1024;

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string trim(const std::string &s)
{
	size_t first = s.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return std::string();
	size_t last = s.find_last_not_of(" \t\r\n");
	return s.substr(first, last - first + 1);
}

std::string url_decode(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
		    isxdigit((unsigned char)s[i + 2])) {
			out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
			i += 2;
		} else if (s[i] == '+') {
			out += ' ';
		} else {
			out += s[i];
		}
	}
	return out;
}

const char *status_text(int status)
{
	switch (status) {
	case 200:
		return "OK";
	case 204:
		return "No Content";
	case 301:
		return "Moved Permanently";
	case 302:
		return "Found";
	case 400:
		return "Bad Request";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 409:
		return "Conflict";
	default:
		return "Error";
	}
}

bool read_file(const std::string &path, std::string &out)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

bool write_file(const std::string &path, const std::string &data)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f)
		return false;
	f << data;
	return (bool)f;
}

/* select() based wait so a blocked read can be interrupted by socket shutdown. */
int wait_readable(vcast_socket_t s, int timeout_ms)
{
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(s, &rfds);
	timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	return select((int)s + 1, &rfds, nullptr, nullptr, &tv);
}

int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	vcast_socket_t s = *static_cast<vcast_socket_t *>(ctx);
#ifdef _WIN32
	int n = send(s, (const char *)buf, (int)std::min(len, (size_t)0x100000), 0);
#elif defined(MSG_NOSIGNAL)
	ssize_t n = send(s, buf, len, MSG_NOSIGNAL);
#else
	ssize_t n = send(s, buf, len, 0);
#endif
	if (n < 0)
		return MBEDTLS_ERR_NET_SEND_FAILED;
	return (int)n;
}

int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	vcast_socket_t s = *static_cast<vcast_socket_t *>(ctx);
	int ready = wait_readable(s, 200);
	if (ready == 0)
		return MBEDTLS_ERR_SSL_WANT_READ;
	if (ready < 0)
		return MBEDTLS_ERR_NET_RECV_FAILED;
	int n = (int)recv(s, (char *)buf, (int)std::min(len, (size_t)0x100000), 0);
	if (n < 0)
		return MBEDTLS_ERR_NET_RECV_FAILED;
	return n; // 0 = connection closed
}

} // namespace

std::string HttpRequest::query_param(const std::string &name) const
{
	size_t pos = 0;
	while (pos <= query.size()) {
		size_t amp = query.find('&', pos);
		std::string pair = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
		size_t eq = pair.find('=');
		if (url_decode(pair.substr(0, eq)) == name)
			return eq == std::string::npos ? std::string() : url_decode(pair.substr(eq + 1));
		if (amp == std::string::npos)
			break;
		pos = amp + 1;
	}
	return std::string();
}

std::string HttpRequest::header(const std::string &name) const
{
	auto it = headers.find(lower(name));
	return it == headers.end() ? std::string() : it->second;
}

/* ------------------------------------------------------------------------- */
/* TLS setup and the self-signed certificate                                  */

struct WebServer::Tls {
	std::mutex rng_mutex;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt cert;
	mbedtls_pk_context key;
	mbedtls_ssl_config conf;

	Tls()
	{
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&drbg);
		mbedtls_x509_crt_init(&cert);
		mbedtls_pk_init(&key);
		mbedtls_ssl_config_init(&conf);
	}
	~Tls()
	{
		mbedtls_ssl_config_free(&conf);
		mbedtls_pk_free(&key);
		mbedtls_x509_crt_free(&cert);
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
	}

	static int rng(void *ctx, unsigned char *out, size_t len)
	{
		auto *self = static_cast<Tls *>(ctx);
		std::lock_guard<std::mutex> lock(self->rng_mutex);
		return mbedtls_ctr_drbg_random(&self->drbg, out, len);
	}

	bool generate(const std::string &cert_path, const std::string &key_path, std::string *error)
	{
		mbedtls_pk_free(&key);
		mbedtls_pk_init(&key);
		int rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
		if (rc == 0)
			rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), &Tls::rng, this);
		if (rc != 0) {
			if (error)
				*error = "could not generate a TLS key";
			return false;
		}

		/* Browsers cap certificate lifetime at 398 days; it is renewed automatically. */
		time_t now = time(nullptr);
		time_t until = now + 390LL * 24 * 3600;
		char not_before[16], not_after[16];
		struct tm tm_now, tm_until;
#ifdef _WIN32
		gmtime_s(&tm_now, &now);
		gmtime_s(&tm_until, &until);
#else
		gmtime_r(&now, &tm_now);
		gmtime_r(&until, &tm_until);
#endif
		strftime(not_before, sizeof(not_before), "%Y%m%d%H%M%S", &tm_now);
		strftime(not_after, sizeof(not_after), "%Y%m%d%H%M%S", &tm_until);

		unsigned char serial[16];
		rng(this, serial, sizeof(serial));
		serial[0] &= 0x7F;

		mbedtls_x509write_cert crt;
		mbedtls_x509write_crt_init(&crt);
		mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
		mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
		mbedtls_x509write_crt_set_subject_key(&crt, &key);
		mbedtls_x509write_crt_set_issuer_key(&crt, &key);
		rc = mbedtls_x509write_crt_set_subject_name(&crt, "CN=Vcast OBS receiver,O=Vcast");
		if (rc == 0)
			rc = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=Vcast OBS receiver,O=Vcast");
		if (rc == 0)
			rc = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
		if (rc == 0)
			rc = mbedtls_x509write_crt_set_validity(&crt, not_before, not_after);
		if (rc == 0)
			rc = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);

		std::vector<unsigned char> pem(8192);
		if (rc == 0)
			rc = mbedtls_x509write_crt_pem(&crt, pem.data(), pem.size(), &Tls::rng, this);
		mbedtls_x509write_crt_free(&crt);
		if (rc != 0) {
			if (error)
				*error = "could not create the TLS certificate";
			return false;
		}
		std::string cert_pem((const char *)pem.data());

		std::fill(pem.begin(), pem.end(), 0);
		if (mbedtls_pk_write_key_pem(&key, pem.data(), pem.size()) != 0) {
			if (error)
				*error = "could not save the TLS key";
			return false;
		}
		std::string key_pem((const char *)pem.data());

		if (!write_file(cert_path, cert_pem) || !write_file(key_path, key_pem))
			obs_log(LOG_WARNING, "Could not save the TLS certificate to %s", cert_path.c_str());

		mbedtls_x509_crt_free(&cert);
		mbedtls_x509_crt_init(&cert);
		return mbedtls_x509_crt_parse(&cert, (const unsigned char *)cert_pem.c_str(), cert_pem.size() + 1) == 0;
	}

	bool load(const std::string &cert_path, const std::string &key_path)
	{
		std::string cert_pem, key_pem;
		if (!read_file(cert_path, cert_pem) || !read_file(key_path, key_pem))
			return false;
		mbedtls_x509_crt_free(&cert);
		mbedtls_x509_crt_init(&cert);
		mbedtls_pk_free(&key);
		mbedtls_pk_init(&key);
		if (mbedtls_x509_crt_parse(&cert, (const unsigned char *)cert_pem.c_str(), cert_pem.size() + 1) != 0)
			return false;
		if (mbedtls_pk_parse_key(&key, (const unsigned char *)key_pem.c_str(), key_pem.size() + 1, nullptr, 0,
					 &Tls::rng, this) != 0)
			return false;
		/* Renew a week before it expires. */
		time_t soon = time(nullptr) + 7 * 24 * 3600;
		struct tm t;
#ifdef _WIN32
		gmtime_s(&t, &soon);
#else
		gmtime_r(&soon, &t);
#endif
		const mbedtls_x509_time &to = cert.valid_to;
		long long expires = (long long)to.year * 10000000000LL + (long long)to.mon * 100000000LL +
				    (long long)to.day * 1000000LL + to.hour * 10000LL + to.min * 100LL + to.sec;
		long long renew_at = (long long)(t.tm_year + 1900) * 10000000000LL +
				     (long long)(t.tm_mon + 1) * 100000000LL + (long long)t.tm_mday * 1000000LL +
				     t.tm_hour * 10000LL + t.tm_min * 100LL + t.tm_sec;
		return expires > renew_at;
	}

	bool init(const std::string &cert_dir, std::string *error)
	{
		static std::once_flag psa_once;
		std::call_once(psa_once, []() { psa_crypto_init(); });

		const char *pers = "vcast-web";
		if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers,
					  strlen(pers)) != 0) {
			if (error)
				*error = "could not seed the random number generator";
			return false;
		}

		std::string cert_path = cert_dir + "/vcast-cert.pem";
		std::string key_path = cert_dir + "/vcast-key.pem";
		if (!load(cert_path, key_path)) {
			obs_log(LOG_INFO, "Creating a new self-signed certificate for the cast page");
			if (!generate(cert_path, key_path, error))
				return false;
		}

		if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
						MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
			if (error)
				*error = "could not configure TLS";
			return false;
		}
		/* TLS 1.3 in mbedTLS needs its PSA threading support, which we don't build. */
		mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
		mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
		mbedtls_ssl_conf_rng(&conf, &Tls::rng, this);
		if (mbedtls_ssl_conf_own_cert(&conf, &cert, &key) != 0) {
			if (error)
				*error = "could not use the TLS certificate";
			return false;
		}
		return true;
	}
};

/* ------------------------------------------------------------------------- */
/* Connection                                                                 */

namespace {

class Connection : public WebSocket {
public:
	Connection(vcast_socket_t sock, std::string peer, mbedtls_ssl_config *conf)
		: sock_(sock),
		  peer_(std::move(peer))
	{
		mbedtls_ssl_init(&ssl_);
		ok_ = mbedtls_ssl_setup(&ssl_, conf) == 0;
		mbedtls_ssl_set_bio(&ssl_, &sock_, bio_send, bio_recv, nullptr);
	}

	~Connection() override { mbedtls_ssl_free(&ssl_); }

	bool handshake(std::atomic<bool> &running)
	{
		if (!ok_)
			return false;
		uint64_t deadline = os_gettime_ns() + 10000000000ULL;
		for (;;) {
			int rc = mbedtls_ssl_handshake(&ssl_);
			if (rc == 0)
				return true;
			if ((rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) || !running ||
			    os_gettime_ns() > deadline) {
				if (rc != MBEDTLS_ERR_SSL_WANT_READ) {
					char err[128];
					mbedtls_strerror(rc, err, sizeof(err));
					/* Normal when a browser first rejects the self-signed certificate. */
					obs_log(LOG_DEBUG, "TLS handshake with %s failed: %s", peer_.c_str(), err);
				}
				return false;
			}
		}
	}

	/* Reads up to size bytes. Returns >0 bytes, 0 on timeout (call again), <0 on close/error. */
	int read(uint8_t *buf, size_t size)
	{
		int rc = mbedtls_ssl_read(&ssl_, buf, size);
		if (rc > 0)
			return rc;
		if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
		    rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
			return 0;
		return -1;
	}

	bool write_all(const void *data, size_t size)
	{
		const auto *p = static_cast<const uint8_t *>(data);
		while (size > 0) {
			int rc = mbedtls_ssl_write(&ssl_, p, size);
			if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
			if (rc <= 0)
				return false;
			p += rc;
			size -= (size_t)rc;
		}
		return true;
	}

	void close_notify() { mbedtls_ssl_close_notify(&ssl_); }

	/* WebSocket interface: queued, written by the connection thread. */
	bool send_text(const std::string &text) override
	{
		std::lock_guard<std::mutex> lock(out_mutex_);
		if (closed_)
			return false;
		outbox_.push_back(text);
		return true;
	}

	void close() override
	{
		std::lock_guard<std::mutex> lock(out_mutex_);
		closed_ = true;
	}

	const std::string &peer() const override { return peer_; }

	bool flush_outbox()
	{
		std::deque<std::string> pending;
		bool closed;
		{
			std::lock_guard<std::mutex> lock(out_mutex_);
			pending.swap(outbox_);
			closed = closed_;
		}
		for (auto &text : pending) {
			if (!write_frame(0x1, text.data(), text.size()))
				return false;
		}
		if (closed) {
			write_frame(0x8, nullptr, 0);
			return false;
		}
		return true;
	}

	bool write_frame(uint8_t opcode, const void *data, size_t size)
	{
		uint8_t header[10];
		size_t n = 0;
		header[n++] = (uint8_t)(0x80 | opcode);
		if (size < 126) {
			header[n++] = (uint8_t)size;
		} else if (size <= 0xFFFF) {
			header[n++] = 126;
			header[n++] = (uint8_t)(size >> 8);
			header[n++] = (uint8_t)size;
		} else {
			header[n++] = 127;
			for (int i = 7; i >= 0; i--)
				header[n++] = (uint8_t)((uint64_t)size >> (i * 8));
		}
		return write_all(header, n) && (size == 0 || write_all(data, size));
	}

private:
	vcast_socket_t sock_;
	std::string peer_;
	mbedtls_ssl_context ssl_;
	bool ok_ = false;
	std::mutex out_mutex_;
	std::deque<std::string> outbox_;
	bool closed_ = false;
};

bool parse_request(const std::string &raw, HttpRequest &req)
{
	size_t line_end = raw.find("\r\n");
	std::string line = raw.substr(0, line_end);
	size_t sp1 = line.find(' ');
	size_t sp2 = line.find(' ', sp1 + 1);
	if (sp1 == std::string::npos || sp2 == std::string::npos)
		return false;
	req.method = line.substr(0, sp1);
	std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
	size_t q = target.find('?');
	req.path = target.substr(0, q);
	req.query = q == std::string::npos ? std::string() : target.substr(q + 1);

	size_t pos = line_end + 2;
	while (pos < raw.size()) {
		size_t end = raw.find("\r\n", pos);
		if (end == std::string::npos || end == pos)
			break;
		std::string h = raw.substr(pos, end - pos);
		size_t colon = h.find(':');
		if (colon != std::string::npos)
			req.headers[lower(trim(h.substr(0, colon)))] = trim(h.substr(colon + 1));
		pos = end + 2;
	}
	return true;
}

std::string websocket_accept(const std::string &key)
{
	std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	unsigned char digest[20];
	mbedtls_sha1((const unsigned char *)input.data(), input.size(), digest);
	unsigned char out[64];
	size_t olen = 0;
	mbedtls_base64_encode(out, sizeof(out), &olen, digest, sizeof(digest));
	return std::string((const char *)out, olen);
}

} // namespace

/* ------------------------------------------------------------------------- */

WebServer::WebServer() = default;

WebServer::~WebServer()
{
	stop();
}

bool WebServer::start(uint16_t port, int attempts, const std::string &cert_dir, std::string *error)
{
	if (running_)
		return true;

	tls_ = std::make_unique<Tls>();
	if (!tls_->init(cert_dir, error)) {
		tls_.reset();
		return false;
	}

	for (int i = 0; i < std::max(attempts, 1); i++) {
		vcast_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == VCAST_INVALID_SOCKET)
			break;
		int one = 1;
#ifdef _WIN32
		setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof(one));
#else
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons((uint16_t)(port + i));
		if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 && listen(s, 16) == 0) {
			listen_sock_ = s;
			port_ = (uint16_t)(port + i);
			break;
		}
		socket_close(s);
	}
	if (listen_sock_ == VCAST_INVALID_SOCKET) {
		if (error)
			*error = "Could not open a network port for the cast page (tried " + std::to_string(port) +
				 "-" + std::to_string(port + attempts - 1) + ")";
		tls_.reset();
		return false;
	}

	running_ = true;
	accept_thread_ = std::thread(&WebServer::accept_loop, this);
	obs_log(LOG_INFO, "Cast page listening on https://%s:%u/", primary_local_ip().c_str(), port_);
	return true;
}

void WebServer::stop()
{
	if (!running_.exchange(false))
		return;
	socket_shutdown(listen_sock_);
	if (accept_thread_.joinable())
		accept_thread_.join();
	socket_close(listen_sock_);
	listen_sock_ = VCAST_INVALID_SOCKET;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto s : open_sockets_)
			socket_shutdown(s);
	}
	reap(true);
	tls_.reset();
}

void WebServer::reap(bool all)
{
	std::vector<Worker> finished;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto it = workers_.begin(); it != workers_.end();) {
			if (all || it->done->load()) {
				finished.push_back(std::move(*it));
				it = workers_.erase(it);
			} else {
				++it;
			}
		}
	}
	for (auto &w : finished)
		if (w.thread.joinable())
			w.thread.join();
}

void WebServer::accept_loop()
{
	while (running_) {
		int rc = wait_readable(listen_sock_, 250);
		reap(false);
		if (rc <= 0 || !running_)
			continue;
		sockaddr_in addr{};
		socklen_t len = sizeof(addr);
		vcast_socket_t s = accept(listen_sock_, reinterpret_cast<sockaddr *>(&addr), &len);
		if (s == VCAST_INVALID_SOCKET)
			continue;
		char ip[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
		std::string peer = ip;
		socket_set_nosigpipe(s);
		int one = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

		Worker w;
		w.done = std::make_shared<std::atomic<bool>>(false);
		auto done = w.done;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			open_sockets_.push_back(s);
		}
		w.thread = std::thread([this, s, peer, done]() {
			handle_connection(s, peer);
			{
				std::lock_guard<std::mutex> lock(mutex_);
				open_sockets_.erase(std::remove(open_sockets_.begin(), open_sockets_.end(), s),
						    open_sockets_.end());
			}
			socket_close(s);
			done->store(true);
		});
		std::lock_guard<std::mutex> lock(mutex_);
		workers_.push_back(std::move(w));
	}
}

void WebServer::handle_connection(vcast_socket_t sock, const std::string &peer)
{
	/* TLS 1.2 handshakes only touch per-connection state plus the locked RNG,
	 * so connections can run in parallel without mbedTLS's threading layer. */
	Connection conn(sock, peer, &tls_->conf);
	if (!conn.handshake(running_))
		return;

	/* HTTP/1.1 keep-alive: serve requests until the browser closes or upgrades. */
	std::string buffer;
	uint8_t chunk[16384];
	uint64_t idle_deadline = os_gettime_ns() + 30000000000ULL;
	while (running_) {
		size_t header_end = buffer.find("\r\n\r\n");
		if (header_end == std::string::npos) {
			if (buffer.size() > kMaxRequestBytes || os_gettime_ns() > idle_deadline)
				return;
			int n = conn.read(chunk, sizeof(chunk));
			if (n < 0)
				return;
			buffer.append((const char *)chunk, (size_t)n);
			continue;
		}

		HttpRequest req;
		req.peer = peer;
		if (!parse_request(buffer.substr(0, header_end + 2), req))
			return;
		buffer.erase(0, header_end + 4);
		idle_deadline = os_gettime_ns() + 30000000000ULL;

		bool upgrade = lower(req.header("upgrade")) == "websocket";
		if (upgrade) {
			std::shared_ptr<WebSocketHandler> handler = ws_route_ ? ws_route_(req) : nullptr;
			if (!handler || (!handler->on_binary && !handler->on_text)) {
				std::string resp =
					"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
				conn.write_all(resp.data(), resp.size());
				return;
			}
			std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
					   "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
					   websocket_accept(req.header("sec-websocket-key")) + "\r\n\r\n";
			if (!conn.write_all(resp.data(), resp.size()))
				return;

			/* WebSocket frame loop. */
			std::vector<uint8_t> message;
			uint8_t message_opcode = 0;
			std::vector<uint8_t> in(buffer.begin(), buffer.end());
			buffer.clear();
			bool open = true;
			while (open && running_) {
				if (!conn.flush_outbox())
					break;
				/* Parse every complete frame in the input buffer. */
				for (;;) {
					if (in.size() < 2)
						break;
					uint8_t b0 = in[0], b1 = in[1];
					bool fin = (b0 & 0x80) != 0;
					uint8_t opcode = b0 & 0x0F;
					bool masked = (b1 & 0x80) != 0;
					uint64_t len = b1 & 0x7F;
					size_t off = 2;
					if (len == 126) {
						if (in.size() < 4)
							break;
						len = ((uint64_t)in[2] << 8) | in[3];
						off = 4;
					} else if (len == 127) {
						if (in.size() < 10)
							break;
						len = 0;
						for (int i = 0; i < 8; i++)
							len = (len << 8) | in[2 + i];
						off = 10;
					}
					if (len > kMaxMessageBytes) {
						open = false;
						break;
					}
					size_t mask_off = off;
					if (masked)
						off += 4;
					if (in.size() < off + len)
						break;
					uint8_t *payload = in.data() + off;
					if (masked) {
						for (uint64_t i = 0; i < len; i++)
							payload[i] ^= in[mask_off + (i & 3)];
					}

					if (opcode == 0x8) { // close
						conn.write_frame(0x8, nullptr, 0);
						open = false;
					} else if (opcode == 0x9) { // ping
						conn.write_frame(0xA, payload, (size_t)len);
					} else if (opcode == 0x1 || opcode == 0x2 || opcode == 0x0) {
						if (opcode != 0x0) {
							message.clear();
							message_opcode = opcode;
						}
						message.insert(message.end(), payload, payload + len);
						if (message.size() > kMaxMessageBytes)
							open = false;
						if (fin && open) {
							bool keep = true;
							if (message_opcode == 0x2 && handler->on_binary)
								keep = handler->on_binary(conn, message.data(),
											  message.size());
							else if (message_opcode == 0x1 && handler->on_text)
								keep = handler->on_text(conn,
											std::string(message.begin(),
												    message.end()));
							message.clear();
							if (!keep)
								open = false;
						}
					}
					in.erase(in.begin(), in.begin() + (std::ptrdiff_t)(off + len));
					if (!open)
						break;
				}
				if (!open)
					break;
				int n = conn.read(chunk, sizeof(chunk));
				if (n < 0)
					break;
				in.insert(in.end(), chunk, chunk + n);
			}
			conn.flush_outbox();
			if (handler->on_close)
				handler->on_close(conn);
			conn.close_notify();
			return;
		}

		HttpReply reply = http_route_ ? http_route_(req) : HttpReply{404, "text/plain", "Not found\n", {}};
		std::string head = "HTTP/1.1 " + std::to_string(reply.status) + " " + status_text(reply.status) +
				   "\r\nServer: Vcast\r\nContent-Type: " + reply.content_type +
				   "\r\nContent-Length: " + std::to_string(reply.body.size()) +
				   "\r\nCache-Control: no-store\r\n";
		for (auto &h : reply.headers)
			head += h.first + ": " + h.second + "\r\n";
		head += "\r\n";
		if (!conn.write_all(head.data(), head.size()))
			return;
		if (req.method != "HEAD" && !reply.body.empty() &&
		    !conn.write_all(reply.body.data(), reply.body.size()))
			return;
		if (lower(req.header("connection")) == "close")
			break;
	}
	conn.close_notify();
}

} // namespace vcast
