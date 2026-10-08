// license:BSD-3-Clause
// copyright-holders:Thomas Cherryhomes
#ifndef MAME_BUS_A7800_FUJILINK_H
#define MAME_BUS_A7800_FUJILINK_H

#pragma once

#include "fujins.h"
#include "fujibus.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// ======================> a78_fujilink
//
// SLIP-framed FujiBus over a TCP socket, to fujinet-pc's "Bus over IP"
// listener: the cartridge's link to the FujiNet, standing in for the
// RP2354B's USB CDC connection to the ESP32-S3. A port of the transport the
// FujiNet Go NES cartridge uses (MesenCE FujiNetLink), POSIX and Winsock.
//
// Two facts about that listener shape everything here:
//  - It accepts ONE client (listen backlog 1). A stale connection starves the
//    next one, and the symptom is a hang rather than an error.
//  - It binds 127.0.0.1 only, while "localhost" usually resolves to ::1
//    first, so open() walks every getaddrinfo result.
//
// Every call blocks up to its deadline, so all of them belong on the
// cartridge's worker thread. close() is the exception: it is safe from any
// thread and wakes a worker parked in select() at once.

class a78_fujilink
{
public:
	typedef bool (*inbound_handler)(const fb_reply_t &frame);

	a78_fujilink() = default;
	~a78_fujilink();

	a78_fujilink(const a78_fujilink &) = delete;
	a78_fujilink &operator=(const a78_fujilink &) = delete;

	bool open(const std::string &host, int port);
	void close();
	bool is_open() const { return m_socket.load(std::memory_order_acquire) != INVALID; }
	std::string last_error();

	fb_status_t transact(uint8_t device, uint8_t command, const fb_param_t *params, unsigned nparams,
			const uint8_t *payload, uint16_t payload_len, uint32_t timeout_ms, fb_reply_t *reply);
	void send_bare(uint8_t device, uint8_t command, const uint8_t *payload, uint16_t payload_len);

	void set_inbound_handler(inbound_handler handler) { m_inbound = handler; }

private:
	// The largest SLIP-encoded frame: a 6-byte header plus a 1K reply
	// (FUJIMAIL_RX_MAX), doubled for SLIP's worst case, plus two delimiters
	// (fujinet-firmware 03b925719). Undersizing this silently truncates.
	static constexpr size_t RX_RAW_MAX = 2 * (6 + 1024) + 2;
	// fujibus_build_request() assembles at most 384 decoded bytes; SLIP can
	// double each, plus two delimiters.
	static constexpr size_t TX_RAW_MAX = 2 * 384 + 2;
	static constexpr int CONNECT_TIMEOUT_MS = 3000;
	static constexpr int LOOPBACK_CONNECT_TIMEOUT_MS = 500;
	static constexpr intptr_t INVALID = -1;

	fb_status_t read_frame(size_t &out_len, uint32_t timeout_ms);
	bool fail(const std::string &what);

	std::atomic<intptr_t> m_socket { INVALID };
	std::mutex m_error_lock;
	std::string m_last_error;
	inbound_handler m_inbound = nullptr;

	std::array<uint8_t, RX_RAW_MAX> m_rx_raw = {};
	std::array<uint8_t, TX_RAW_MAX> m_tx_raw = {};
	std::array<uint8_t, 4096> m_rx_pend = {};
	size_t m_rx_pend_len = 0;
	size_t m_rx_pend_pos = 0;
};

#endif // MAME_BUS_A7800_FUJILINK_H
