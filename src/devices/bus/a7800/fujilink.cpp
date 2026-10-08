// license:BSD-3-Clause
// copyright-holders:Thomas Cherryhomes
/***********************************************************************************************************

 Atari 7800 FujiNet cartridge: the link to fujinet-pc (see fujilink.h)

 ***********************************************************************************************************/

#include "emu.h"
#include "fujilink.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
using socklen_arg = int;
int close_socket(sock_t s) { return ::closesocket(s); }
int last_socket_error() { return ::WSAGetLastError(); }
bool would_block(int e) { return e == WSAEWOULDBLOCK; }
bool in_progress(int e) { return e == WSAEWOULDBLOCK; }
constexpr int SEND_FLAGS = 0;

void init_sockets()
{
	static const bool init = [] {
		WSADATA data;
		return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	(void)init;
}

bool set_nonblocking(sock_t s)
{
	u_long on = 1;
	return ::ioctlsocket(s, FIONBIO, &on) == 0;
}

void shutdown_socket(sock_t s) { ::shutdown(s, SD_BOTH); }
#else
using sock_t = int;
using socklen_arg = socklen_t;
int close_socket(sock_t s) { return ::close(s); }
int last_socket_error() { return errno; }
bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
bool in_progress(int e) { return e == EINPROGRESS; }
void init_sockets() { }

// Linux suppresses SIGPIPE per call; macOS has no MSG_NOSIGNAL and does it
// per socket instead (SO_NOSIGPIPE in open()).
#ifdef MSG_NOSIGNAL
constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
constexpr int SEND_FLAGS = 0;
#endif

bool set_nonblocking(sock_t s)
{
	int flags = ::fcntl(s, F_GETFL, 0);
	return flags != -1 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}

void shutdown_socket(sock_t s) { ::shutdown(s, SHUT_RDWR); }
#endif

std::string socket_error_text(int err)
{
#ifdef _WIN32
	return "error " + std::to_string(err);
#else
	return std::strerror(err);
#endif
}

// Wait until the socket is readable (or writable, while connecting).
int wait_for(sock_t s, int64_t remain_ms, bool for_write)
{
	if (remain_ms <= 0)
		return 0;

	fd_set set;
	FD_ZERO(&set);
	FD_SET(s, &set);
	// Winsock reports a FAILED non-blocking connect in the exception set, not
	// the write set: without it a refused connection waits out the whole
	// connect timeout. POSIX reports it as writable, and SO_ERROR sees it
	// either way.
	fd_set exc = set;

	timeval tv = {};
	tv.tv_sec = decltype(tv.tv_sec)(remain_ms / 1000);
	tv.tv_usec = decltype(tv.tv_usec)((remain_ms % 1000) * 1000);

	return ::select(int(s) + 1, for_write ? nullptr : &set, for_write ? &set : nullptr, for_write ? &exc : nullptr, &tv);
}

int64_t ms_since(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

} // anonymous namespace


a78_fujilink::~a78_fujilink()
{
	close();
}

bool a78_fujilink::fail(const std::string &what)
{
	{
		std::lock_guard<std::mutex> lock(m_error_lock);
		m_last_error = what;
	}
	osd_printf_verbose("fujinet: %s\n", what);
	return false;
}

std::string a78_fujilink::last_error()
{
	std::lock_guard<std::mutex> lock(m_error_lock);
	return m_last_error;
}

bool a78_fujilink::open(const std::string &host, int port)
{
	close();
	init_sockets();

	std::string host_name = host.empty() ? "127.0.0.1" : host;
	std::string port_name = std::to_string(port);
	std::string endpoint = host_name + ":" + port_name;

	addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo *results = nullptr;
	if (::getaddrinfo(host_name.c_str(), port_name.c_str(), &hints, &results) != 0 || !results)
		return fail("cannot resolve " + endpoint);

	int last_err = 0;
	intptr_t opened = INVALID;
	for (const addrinfo *ai = results; ai; ai = ai->ai_next)
	{
		sock_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s == sock_t(INVALID))
			continue;

#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
		int on = 1;
		::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif

		if (!set_nonblocking(s))
		{
			close_socket(s);
			continue;
		}

		// A loopback listener answers at once or is not there. Windows retries a
		// refused connect for about two seconds before reporting it, which would
		// hold up every redial while FujiNet is not (yet) listening.
		bool loopback = false;
		if (ai->ai_family == AF_INET)
			loopback = (ntohl(reinterpret_cast<const sockaddr_in *>(ai->ai_addr)->sin_addr.s_addr) >> 24) == 127;
		else if (ai->ai_family == AF_INET6)
			loopback = IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6 *>(ai->ai_addr)->sin6_addr);

		bool connected = ::connect(s, ai->ai_addr, socklen_arg(ai->ai_addrlen)) == 0;
		if (!connected && in_progress(last_socket_error()))
		{
			if (wait_for(s, loopback ? LOOPBACK_CONNECT_TIMEOUT_MS : CONNECT_TIMEOUT_MS, true) > 0)
			{
				int err = 0;
				socklen_arg len = socklen_arg(sizeof err);
				connected = ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &len) == 0 && err == 0;
				if (!connected)
					last_err = err;
			}
		}
		else if (!connected)
		{
			last_err = last_socket_error();
		}

		if (connected)
		{
			int one = 1;
			::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof one);
			opened = intptr_t(s);
			break;
		}
		close_socket(s);
	}
	::freeaddrinfo(results);

	if (opened == INVALID)
		return fail("cannot connect to " + endpoint + (last_err ? " (" + socket_error_text(last_err) + ")" : ""));

	m_rx_pend_len = m_rx_pend_pos = 0;
	{
		std::lock_guard<std::mutex> lock(m_error_lock);
		m_last_error.clear();
	}
	m_socket.store(opened, std::memory_order_release);
	osd_printf_verbose("fujinet: connected to %s\n", endpoint);
	return true;
}

void a78_fujilink::close()
{
	intptr_t s = m_socket.exchange(INVALID, std::memory_order_acq_rel);
	if (s == INVALID)
		return;

	// Shut down before closing so a worker parked in select() wakes at once
	// rather than serving out a mount's 60-second deadline.
	shutdown_socket(sock_t(s));
	close_socket(sock_t(s));
}

fb_status_t a78_fujilink::read_frame(size_t &out_len, uint32_t timeout_ms)
{
	auto start = std::chrono::steady_clock::now();
	size_t n = 0;
	int ends = 0;

	for (;;)
	{
		// Drain what the last recv() over-read first: one read can straddle two
		// frames, and the remainder belongs to the next one.
		while (m_rx_pend_pos < m_rx_pend_len)
		{
			if (n >= m_rx_raw.size())
				return FB_ETOOBIG;

			uint8_t c = m_rx_pend[m_rx_pend_pos++];
			m_rx_raw[n++] = c;
			if (c == 0xc0 && ++ends == 2)
			{
				out_len = n;
				return FB_OK;
			}
		}

		intptr_t sock = m_socket.load(std::memory_order_acquire);
		if (sock == INVALID)
			return FB_ENOLINK;

		sock_t s = sock_t(sock);
		if (wait_for(s, int64_t(timeout_ms) - ms_since(start), false) <= 0)
			return FB_ETIMEOUT;

		auto r = ::recv(s, reinterpret_cast<char *>(m_rx_pend.data()), int(m_rx_pend.size()), 0);
		if (r > 0)
		{
			m_rx_pend_len = size_t(r);
			m_rx_pend_pos = 0;
		}
		else if (r == 0 || !would_block(last_socket_error()))
		{
			// fujinet-pc closed the connection, or close() shut it down
			return FB_ENOLINK;
		}
	}
}

fb_status_t a78_fujilink::transact(uint8_t device, uint8_t command, const fb_param_t *params, unsigned nparams,
		const uint8_t *payload, uint16_t payload_len, uint32_t timeout_ms, fb_reply_t *reply)
{
	intptr_t sock = m_socket.load(std::memory_order_acquire);
	if (sock == INVALID)
		return FB_ENOLINK;

	size_t req_len = fujibus_build_request(device, command, params, nparams, payload, payload_len, m_tx_raw.data(), m_tx_raw.size());
	if (!req_len)
		return FB_ETOOBIG;

	if (::send(sock_t(sock), reinterpret_cast<const char *>(m_tx_raw.data()), int(req_len), SEND_FLAGS) != int(req_len))
		return FB_ENOLINK;

	for (;;)
	{
		size_t raw_len = 0;
		fb_status_t status = read_frame(raw_len, timeout_ms);
		if (status != FB_OK)
			return status;

		if (!fujibus_parse_reply(m_rx_raw.data(), raw_len, reply))
			return FB_EBADFRAME;

		// Push frames arrive interleaved with the reply we are waiting for;
		// consuming one proves the link is alive, so the deadline restarts.
		if (!m_inbound || !m_inbound(*reply))
			return FB_OK;
	}
}

void a78_fujilink::send_bare(uint8_t device, uint8_t command, const uint8_t *payload, uint16_t payload_len)
{
	intptr_t sock = m_socket.load(std::memory_order_acquire);
	if (sock == INVALID)
		return;

	size_t n = fujibus_build_request(device, command, nullptr, 0, payload, payload_len, m_tx_raw.data(), m_tx_raw.size());
	if (n)
		::send(sock_t(sock), reinterpret_cast<const char *>(m_tx_raw.data()), int(n), SEND_FLAGS);
}
