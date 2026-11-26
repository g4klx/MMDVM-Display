/*
 *   Copyright (C) 2016,2017,2018,2020,2023,2025,2026 by Jonathan Naylor G4KLX
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "MQTTConnection.h"
#include "Nextion.h"
#include "Utils.h"
#include "Log.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <ctime>
#include <clocale>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include <sys/stat.h>
#if defined(_WIN32) || defined(_WIN64)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

extern CMQTTConnection* m_mqtt;

#define LAYOUT_TA_ENABLE	(1 << 4) // enable Talker Alias (TA) display
#define LAYOUT_TA_COLOUR	(1 << 5) // TA display with font colour change
#define LAYOUT_TA_FONTSIZE	(1 << 6) // TA display with font size change
#define LAYOUT_DIY			(1 << 7) // use ON7LDS-DIY layout

// The pages, by the numbers the ON7LDS layouts use (NextionDriver's)
static const char* PAGE_NAMES[NEXTION_PAGES] = {"MMDVM", "DStar", "DMR", "YSF", "P25", "NXDN", "POCSAG", "M17", "FM"};
const unsigned int PAGE_MMDVM = 0U;

// The calls the last heard lists keep, for each page
const unsigned int LAST_HEARD = 20U;

// The colours of the DIY layout's mode and network fields
const unsigned int BCO_ENABLED  = 1472U;
const unsigned int BCO_DISABLED = 25356U;
const unsigned int PCO_ENABLED  = 0U;
const unsigned int PCO_DISABLED = 46486U;

// The longest message from the screen kept, without its FF FF FF
const unsigned int MAX_REPLY_LENGTH = 250U;

// Instructions sent together, in bytes: well inside the buffers of the
// Nextion and of a modem passing them on.
const unsigned int MAX_BATCH_LENGTH = 120U;

// A byte a text field may hold, from a file or another program: not a
// quote or backslash (they end or escape the text), not a control byte
// or DEL, and never 0xFF, three of which end a Nextion command (a UTF-8
// name, from the ID lists, never holds one).
static bool fieldChar(char c)
{
	unsigned char u = (unsigned char)c;
	return (c != '"') && (c != '\\') && (u >= 0x20U) && (u != 0x7FU) && (u != 0xFFU);
}

CNextion::CNextion(ISerialPort* serial, unsigned int brightness, bool displayClock, bool utc, const std::string& dateFormat, const std::string& timeFormat, unsigned int idleBrightness, unsigned int screenLayout, bool displayTempInF) :
CDisplay(),
m_callsign(),
m_id(0U),
m_duplex(false),
m_ipAddress(),
m_temperature(-1.0F),
m_frequency(-1.0F),
m_load(-1.0F),
m_rxFrequency(-1.0),
m_txFrequency(-1.0),
m_location("?"),
m_serial(serial),
m_brightness(brightness),
m_mode(MODE_IDLE),
m_displayClock(displayClock),
m_utc(utc),
m_dateFormat(dateFormat),
m_timeFormat(timeFormat),
m_idleBrightness(idleBrightness),
m_screenLayout(0),
m_clockDisplayTimer(1000U, 0U, 400U),
m_displayTempInF(displayTempInF),
m_output(4000U, "Nextion buffer"),
m_mutex(),
m_reply(nullptr),
m_replyLength(0U),
m_replyFFs(0U),
m_waiting(false),
m_pending(0U),
m_redrawIdle(false),
m_waitingTimer(1000U, 0U, 500U),
m_page(PAGE_MMDVM),
m_fields(),
m_heard(),
m_inCall(),
m_held(false),
m_asleep(false),
m_sleepWhenIdle(0U),
m_sleepTimer(1000U),
m_modes(),
m_modesKnown(false),
m_running(),
m_programsKnown(false),
m_groupsFile(),
m_groups(),
m_groupsTime(0),
m_groupsTimer(1000U, 60U),
m_diskPath(),
m_model(),
m_serialNo(),
m_dmrIds(nullptr),
m_nxdnIds(nullptr),
m_rtcTimer(1000U, 3600U),
m_dmrCaller(),
m_dmrCallerSet(false),
m_nxdnCaller(),
m_nxdnCallerSet(false)
{
	assert(serial != nullptr);
	assert((brightness >= 0U) && (brightness <= 100U));
	assert((screenLayout == 0U) || (screenLayout == 2U) || (screenLayout == 3U));

	static const unsigned int feature_set[] = {
		0,				// 0: G4KLX
		0,				// 1: unused
						// 2: ON7LDS
		LAYOUT_TA_ENABLE | LAYOUT_TA_COLOUR | LAYOUT_TA_FONTSIZE,
		LAYOUT_TA_ENABLE | LAYOUT_DIY	// 3: ON7LDS-DIY
	};

	m_screenLayout = feature_set[screenLayout];

	m_reply = new unsigned char[MAX_REPLY_LENGTH];
}

CNextion::~CNextion()
{
	delete   m_serial;
	delete[] m_reply;
	delete   m_dmrIds;
	delete   m_nxdnIds;
}

bool CNextion::open()
{
	bool ret = m_serial->open();
	if (!ret) {
		LogError("Cannot open the port for the Nextion display");
		// Freed once: the destructor deletes m_serial too
		delete m_serial;
		m_serial = nullptr;
		return false;
	}

	sendCommand("bkcmd=3");
	sendCommandAction(0U);

	// The screen's model and serial number, which the DIY layout shows,
	// and its clock (K series) set: designs show the date from it
	if (m_screenLayout & LAYOUT_DIY) {
		queueCommand("connect");
		sendRTC();
		m_rtcTimer.start();
	}

	setIdle();

	return true;
}

void CNextion::setIdleInt()
{
	m_redrawIdle = false;

	// a few bits borrowed from Lieven De Samblanx ON7LDS, NextionDriver
	char command[100U];

	sendCommand("page MMDVM");
	sendCommandAction(1U);

	// The clock and the status at once, with the page: layouts show a
	// start-up text in the clock until it comes, and over a modem the
	// instructions after the first few arrive a few tenths of a second
	// later.
	if (m_displayClock)
		sendClock();

	sendCommand("t1.txt=\"MMDVM IDLE\"");
	sendCommandAction(11U);

	if (m_brightness > 0U) {
		::snprintf(command, sizeof(command), "dim=%u", m_idleBrightness);
		sendCommand(command);
	}

	if (m_id != 0U)
		::snprintf(command, sizeof(command), "t0.txt=\"%s/%u\"", m_callsign.c_str(), m_id);
	else
		::snprintf(command, sizeof(command), "t0.txt=\"%s\"", m_callsign.c_str());
	sendCommand(command);

	if (m_screenLayout & LAYOUT_DIY) {
		::snprintf(command, sizeof(command), "t4.txt=\"%s\"", m_callsign.c_str());
		sendCommand(command);
		if (m_id != 0U)
			::snprintf(command, sizeof(command), "t5.txt=\"%u\"", m_id);
		else
			::snprintf(command, sizeof(command), "t5.txt=\"\"");
		sendCommand(command);
		sendCommandAction(17U);
	} else {
		sendCommand("t20.txt=\"?\"");
		sendCommandAction(17U);
	}

	// The address next: a layout shows its own start-up texts until it
	// has the status and the address.
	if (!m_ipAddress.empty())
		::snprintf(command, sizeof(command), "t3.txt=\"%s\"", m_ipAddress.c_str());
	else
		::snprintf(command, sizeof(command), "t3.txt=\"(ip unknown)\"");
	sendCommand(command);
	sendCommandAction(16U);

	sendCPU();

	// Layout 3 takes the frequency as MMDVMHost and NextionDriver sent it:
	// MHz to six places, with no unit ("438.600000"), as its screens show
	// "MHz" themselves; the other layouts show "438.6000 MHz"
	if (m_rxFrequency != -1.0) {
		if (m_screenLayout & LAYOUT_DIY)
			::snprintf(command, sizeof(command), "t30.txt=\"%3.6f\"", m_rxFrequency);
		else
			::snprintf(command, sizeof(command), "t30.txt=\"%.4f MHz\"", m_rxFrequency);
		sendCommand(command);
	} else {
		sendCommand("t30.txt=\"?\"");
	}
	sendCommandAction(20U);

	if (m_txFrequency != -1.0) {
		if (m_screenLayout & LAYOUT_DIY)
			::snprintf(command, sizeof(command), "t32.txt=\"%3.6f\"", m_txFrequency);
		else
			::snprintf(command, sizeof(command), "t32.txt=\"%.4f MHz\"", m_txFrequency);
		sendCommand(command);
	} else {
		sendCommand("t32.txt=\"?\"");
	}
	sendCommandAction(21U);

	sendCommand("t31.txt=\"" + m_location + "\"");
	sendCommandAction(23U);

	m_clockDisplayTimer.start();

	m_mode = MODE_IDLE;

	if (m_screenLayout & LAYOUT_DIY) {
		sendModes(false);

		if (m_sleepWhenIdle > 0U)
			m_sleepTimer.start(m_sleepWhenIdle);
	}
}

void CNextion::setErrorInt()
{
	// Shown whether the screen was asleep or not
	wake();

	sendCommand("page MMDVM");
	sendCommandAction(1U);

	char command[20];
	if (m_brightness>0) {
		::snprintf(command, sizeof(command), "dim=%u", m_brightness);
		sendCommand(command);
	}

	::snprintf(command, sizeof(command), "t0.txt=\"ERROR\"");
	sendCommandAction(13U);

	m_clockDisplayTimer.stop();

	m_mode = MODE_ERROR;
}

void CNextion::setLockoutInt()
{
	// Shown whether the screen was asleep or not
	wake();

	sendCommand("page MMDVM");
	sendCommandAction(1U);

	char command[20];
	if (m_brightness>0) {
		::snprintf(command, sizeof(command), "dim=%u", m_brightness);
		sendCommand(command);
	}

	sendCommand("t0.txt=\"LOCKOUT\"");
	sendCommandAction(15U);

	m_clockDisplayTimer.stop();

	m_mode = MODE_LOCKOUT;
}

void CNextion::setQuitInt()
{
	// Shown whether the screen was asleep or not
	wake();

	sendCommand("page MMDVM");
	sendCommandAction(1U);

	char command[100];
	if (m_brightness>0) {
		::snprintf(command, sizeof(command), "dim=%u", m_idleBrightness);
		sendCommand(command);
	}

	if (!m_ipAddress.empty())
		::snprintf(command, sizeof(command), "t3.txt=\"%s\"", m_ipAddress.c_str());
	else
		::snprintf(command, sizeof(command), "t3.txt=\"(ip unknown)\"");
	sendCommand(command);
	sendCommandAction(16U);

	sendCommand("t0.txt=\"MMDVM STOPPED\"");
	sendCommandAction(19U);

	m_clockDisplayTimer.stop();

	m_mode = MODE_QUIT;
}

void CNextion::writeDStarInt(const std::string& my1, const std::string& my2, const std::string& your, const std::string& type, const std::string& reflector)
{
	if (m_mode != MODE_DSTAR) {
		sendCommand("page DStar");
		sendCommandAction(2U);
	}

	char text[50U];
	if (m_brightness>0) {
		::snprintf(text, sizeof(text), "dim=%u", m_brightness);
		sendCommand(text);
	}

	::snprintf(text, sizeof(text), "t0.txt=\"%s %.8s/%4.4s\"", type.c_str(), my1.c_str(), my2.c_str());
	sendCommand(text);
	sendCommandAction(42U);

	::snprintf(text, sizeof(text), "t1.txt=\"%.8s\"", your.c_str());
	sendCommand(text);
	sendCommandAction(45U);

	if (reflector != "        ") {
		::snprintf(text, sizeof(text), "t2.txt=\"via %.8s\"", reflector.c_str());
		sendCommand(text);
		sendCommandAction(46U);
	}

	m_clockDisplayTimer.stop();

	m_mode = MODE_DSTAR;
}

void CNextion::writeDStarRSSIInt(int rssi)
{
	char text[25U];
	::snprintf(text, sizeof(text), "t3.txt=\"%ddBm\"", rssi);
	sendCommand(text);
	sendCommandAction(47U);
}

void CNextion::writeDStarBERInt(float ber)
{
	char text[25U];
	::snprintf(text, sizeof(text), "t4.txt=\"%.1f%%\"", ber);
	sendCommand(text);
	sendCommandAction(48U);
}

void CNextion::writeDStarTextInt(const std::string& text)
{
	char buffer[40U];
	::snprintf(buffer, sizeof(buffer), "t5.txt=\"%s\"", text.c_str());

	sendCommand(buffer);
	sendCommandAction(49U);
}

void CNextion::clearDStarInt()
{
	sendCommand("t0.txt=\"Listening\"");
	sendCommandAction(41U);
	sendCommand("t1.txt=\"\"");
	sendCommand("t2.txt=\"\"");
	sendCommand("t3.txt=\"\"");
	sendCommand("t4.txt=\"\"");
	sendCommand("t5.txt=\"\"");
}

void CNextion::writeDMRInt(unsigned int slotNo, const std::string& src, bool group, unsigned int dst, const std::string& type)
{
	if (m_mode != MODE_DMR) {
		sendCommand("page DMR");
		sendCommandAction(3U);


		if (slotNo == 1U) {
			if (m_screenLayout & LAYOUT_TA_ENABLE) {
				if (m_screenLayout & LAYOUT_TA_COLOUR)
					sendCommand("t2.pco=0");
				if (m_screenLayout & LAYOUT_TA_FONTSIZE)
					sendCommand("t2.font=4");
			}

			sendCommand("t2.txt=\"2 Listening\"");
			sendCommandAction(69U);
		} else {
			if (m_screenLayout & LAYOUT_TA_ENABLE) {
				if (m_screenLayout & LAYOUT_TA_COLOUR)
					sendCommand("t0.pco=0");
				if (m_screenLayout & LAYOUT_TA_FONTSIZE)
					sendCommand("t0.font=4");
			}

			sendCommand("t0.txt=\"1 Listening\"");
			sendCommandAction(61U);
		}
	}

	char text[100U];
	if (m_brightness>0) {
		::snprintf(text, sizeof(text), "dim=%u", m_brightness);
		sendCommand(text);
	}

	if (slotNo == 1U) {
		::snprintf(text, sizeof(text), "t0.txt=\"1 %s %s\"", type.c_str(), src.c_str());

		if (m_screenLayout & LAYOUT_TA_ENABLE) {
			if (m_screenLayout & LAYOUT_TA_COLOUR)
				sendCommand("t0.pco=0");
			if (m_screenLayout & LAYOUT_TA_FONTSIZE)
				sendCommand("t0.font=4");
		}

		sendCommand(text);

		// Layout 3: the caller's details with them, and the slot's talker
		// alias (t8) cleared until one comes
		if (m_screenLayout & LAYOUT_DIY) {
			sendDMRCaller(slotNo);
			sendCommand("t8.txt=\"\"");
		}

		sendCommandAction(62U);

		::snprintf(text, sizeof(text), "t1.txt=\"%s%u\"", group ? "TG" : "", dst);
		sendCommand(text);
		// Before the status: the layout shows the name if there is one
		sendTGName(slotNo, group, dst);
		sendCommandAction(65U);
	} else {
		::snprintf(text, sizeof(text), "t2.txt=\"2 %s %s\"", type.c_str(), src.c_str());

		if (m_screenLayout & LAYOUT_TA_ENABLE) {
			if (m_screenLayout & LAYOUT_TA_COLOUR)
				sendCommand("t2.pco=0");
			if (m_screenLayout & LAYOUT_TA_FONTSIZE)
				sendCommand("t2.font=4");
		}

		sendCommand(text);

		if (m_screenLayout & LAYOUT_DIY) {
			sendDMRCaller(slotNo);
			sendCommand("t9.txt=\"\"");
		}

		sendCommandAction(70U);

		::snprintf(text, sizeof(text), "t3.txt=\"%s%u\"", group ? "TG" : "", dst);
		sendCommand(text);
		sendTGName(slotNo, group, dst);
		sendCommandAction(73U);
	}

	m_clockDisplayTimer.stop();

	m_mode = MODE_DMR;
}

void CNextion::writeDMRRSSIInt(unsigned int slotNo, int rssi)
{
	if (slotNo == 1U) {
		char text[25U];
		::snprintf(text, sizeof(text), "t4.txt=\"%ddBm\"", rssi);
		sendCommand(text);
		sendCommandAction(66U);
	} else {
		char text[25U];
		::snprintf(text, sizeof(text), "t5.txt=\"%ddBm\"", rssi);
		sendCommand(text);
		sendCommandAction(74U);
	}
}

void CNextion::writeDMRTAInt(unsigned int slotNo, const std::string& talkerAlias)
{
	if (!(m_screenLayout & LAYOUT_TA_ENABLE))
		return;

	if (slotNo == 1U) {
		char text[64U];
		::snprintf(text, sizeof(text), "t8.txt=\"%s\"", talkerAlias.c_str());

		if (m_screenLayout & LAYOUT_TA_FONTSIZE) {
			if (talkerAlias.size() > (16U-4U))
				sendCommand("t8.font=3");
			if (talkerAlias.size() > (20U-4U))
				sendCommand("t8.font=2");
			if (talkerAlias.size() > (24U-4U))
				sendCommand("t8.font=1");
		}

		if (m_screenLayout & LAYOUT_TA_COLOUR)
			sendCommand("t8.pco=1024");

		sendCommand(text);
		sendCommandAction(63U);
	} else {
		char text[64U];
		::snprintf(text, sizeof(text), "t9.txt=\"%s\"", talkerAlias.c_str());

		if (m_screenLayout & LAYOUT_TA_FONTSIZE) {
			if (talkerAlias.size() > (16U-4U))
				sendCommand("t9.font=3");
			if (talkerAlias.size() > (20U-4U))
				sendCommand("t9.font=2");
			if (talkerAlias.size() > (24U-4U))
				sendCommand("t9.font=1");
		}

		if (m_screenLayout & LAYOUT_TA_COLOUR)
			sendCommand("t9.pco=1024");

		sendCommand(text);
		sendCommandAction(71U);
	}
}

void CNextion::writeDMRBERInt(unsigned int slotNo, float ber)
{
	if (slotNo == 1U) {
		char text[25U];
		::snprintf(text, sizeof(text), "t6.txt=\"%.1f%%\"", ber);
		sendCommand(text);
		sendCommandAction(67U);
	} else {
		char text[25U];
		::snprintf(text, sizeof(text), "t7.txt=\"%.1f%%\"", ber);
		sendCommand(text);
		sendCommandAction(75U);
	}
}

void CNextion::clearDMRInt(unsigned int slotNo)
{
	// The call's end, while its fields still hold it: a layout keeping
	// its own last heard list copies the call then
	sendCommandAction(slotNo == 1U ? 64U : 72U);

	if (slotNo == 1U) {
		sendCommand("t0.txt=\"1 Listening\"");
		sendCommandAction(61U);

		if (m_screenLayout & LAYOUT_TA_ENABLE) {
			if (m_screenLayout & LAYOUT_TA_COLOUR)
				sendCommand("t0.pco=0");
			if (m_screenLayout & LAYOUT_TA_FONTSIZE)
				sendCommand("t0.font=4");
		}

		sendCommand("t1.txt=\"\"");
		sendCommand("t4.txt=\"\"");
		sendCommand("t6.txt=\"\"");
		sendCommand("t8.txt=\"\"");
	} else {
		sendCommand("t2.txt=\"2 Listening\"");
		sendCommandAction(69U);

		if (m_screenLayout & LAYOUT_TA_ENABLE) {
			if (m_screenLayout & LAYOUT_TA_COLOUR)
				sendCommand("t2.pco=0");
			if (m_screenLayout & LAYOUT_TA_FONTSIZE)
				sendCommand("t2.font=4");
		}

		sendCommand("t3.txt=\"\"");
		sendCommand("t5.txt=\"\"");
		sendCommand("t7.txt=\"\"");
		sendCommand("t9.txt=\"\"");
	}
}

void CNextion::writeFusionInt(const std::string& source, const std::string& dest, unsigned char dgid, const std::string& type, const std::string& origin)
{
	if (m_mode != MODE_YSF) {
		sendCommand("page YSF");
		sendCommandAction(4U);
	}

	char text[50U];
	if (m_brightness>0) {
		::snprintf(text, sizeof(text), "dim=%u", m_brightness);
		sendCommand(text);
	}

	::snprintf(text, sizeof(text), "t0.txt=\"%s %.10s\"", type.c_str(), source.c_str());
	sendCommand(text);
	sendCommandAction(82U);

	::snprintf(text, sizeof(text), "t1.txt=\"DG-ID %u\"", dgid);
	sendCommand(text);
	sendCommandAction(83U);

	if (origin != "          ") {
		::snprintf(text, sizeof(text), "t2.txt=\"at %.10s\"", origin.c_str());
		sendCommand(text);
		sendCommandAction(84U);
	}

	m_clockDisplayTimer.stop();

	m_mode = MODE_YSF;
}

void CNextion::writeFusionRSSIInt(int rssi)
{
	char text[25U];
	::snprintf(text, sizeof(text), "t3.txt=\"%ddBm\"", rssi);
	sendCommand(text);
	sendCommandAction(85U);
}

void CNextion::writeFusionBERInt(float ber)
{
	char text[25U];
	::snprintf(text, sizeof(text), "t4.txt=\"%.1f%%\"", ber);
	sendCommand(text);
	sendCommandAction(86U);
}

void CNextion::clearFusionInt()
{
	sendCommand("t0.txt=\"Listening\"");
	sendCommandAction(81U);
	sendCommand("t1.txt=\"\"");
	sendCommand("t2.txt=\"\"");
	sendCommand("t3.txt=\"\"");
	sendCommand("t4.txt=\"\"");
}

void CNextion::writeP25Int(const std::string& source, bool group, unsigned int dest, const std::string& type)
{
	if (m_mode != MODE_P25) {
		sendCommand("page P25");
		sendCommandAction(5U);
	}

	char text[50U];
	if (m_brightness>0) {
		::snprintf(text, sizeof(text), "dim=%u", m_brightness);
		sendCommand(text);
	}

	::snprintf(text, sizeof(text), "t0.txt=\"%s %.10s\"", type.c_str(), source.c_str());
	sendCommand(text);
	sendCommandAction(102U);

	::snprintf(text, sizeof(text), "t1.txt=\"%s%u\"", group ? "TG" : "", dest);
	sendCommand(text);
	sendCommandAction(103U);

	m_clockDisplayTimer.stop();

	m_mode = MODE_P25;
}

void CNextion::writeP25RSSIInt(int rssi)
{
	char text[25U];
	::snprintf(text, sizeof(text), "t2.txt=\"%ddBm\"", rssi);
	sendCommand(text);
	sendCommandAction(104U);
}

void CNextion::writeP25BERInt(float ber)
{
	char text[25U];
	::snprintf(text, sizeof(text), "t3.txt=\"%.1f%%\"", ber);
	sendCommand(text);
	sendCommandAction(105U);
}

void CNextion::clearP25Int()
{
	sendCommand("t0.txt=\"Listening\"");
	sendCommandAction(101U);
	sendCommand("t1.txt=\"\"");
	sendCommand("t2.txt=\"\"");
	sendCommand("t3.txt=\"\"");
}

void CNextion::writeNXDNInt(const std::string& source, bool group, unsigned int dest, const std::string& type)
{
	if (m_mode != MODE_NXDN) {
		sendCommand("page NXDN");
		sendCommandAction(6U);
	}

	char text[50U];
	if (m_brightness>0) {
		::snprintf(text, sizeof(text), "dim=%u", m_brightness);
		sendCommand(text);
	}

	::snprintf(text, sizeof(text), "t0.txt=\"%s %.10s\"", type.c_str(), source.c_str());
	sendCommand(text);
	sendNXDNCaller();
	sendCommandAction(122U);

	::snprintf(text, sizeof(text), "t1.txt=\"%s%u\"", group ? "TG" : "", dest);
	sendCommand(text);
	sendCommandAction(123U);

	m_clockDisplayTimer.stop();

	m_mode = MODE_NXDN;
}

void CNextion::writeNXDNRSSIInt(int rssi)
{
	char text[25U];
	::snprintf(text, sizeof(text), "t2.txt=\"%ddBm\"", rssi);
	sendCommand(text);
	sendCommandAction(124U);
}

void CNextion::writeNXDNBERInt(float ber)
{
	char text[25U];
	::snprintf(text, sizeof(text), "t3.txt=\"%.1f%%\"", ber);
	sendCommand(text);
	sendCommandAction(125U);
}

void CNextion::clearNXDNInt()
{
	sendCommand("t0.txt=\"Listening\"");
	sendCommandAction(121U);
	sendCommand("t1.txt=\"\"");
	sendCommand("t2.txt=\"\"");
	sendCommand("t3.txt=\"\"");
}

void CNextion::writeFMInt(const std::string& status)
{
	if (m_mode != MODE_FM) {
		sendCommand("page FM");
		sendCommandAction(9U);
	}

	char text[100U];
	if (m_brightness > 0) {
		::snprintf(text, sizeof(text), "dim=%u", m_brightness);
		sendCommand(text);
	}

	::snprintf(text, sizeof(text), "t0.txt=\"%s\"", status.c_str());
	sendCommand(text);
	sendCommandAction(147U);

	m_clockDisplayTimer.stop();

	m_mode = MODE_FM;
}

void CNextion::writeFMRSSIInt(int rssi)
{
	char text[25U];
	::snprintf(text, sizeof(text), "t2.txt=\"%ddBm\"", rssi);
	sendCommand(text);
	sendCommandAction(148U);
}

void CNextion::clearFMInt()
{
	sendCommand("t0.txt=\"Listening\"");
	sendCommandAction(149U);
	sendCommand("t2.txt=\"\"");
}

void CNextion::writePOCSAGInt(uint32_t ric, const std::string& message)
{
	if (m_mode != MODE_POCSAG) {
		sendCommand("page POCSAG");
		sendCommandAction(7U);
	}

	char text[200U];
	if (m_brightness>0) {
		::snprintf(text, sizeof(text), "dim=%u", m_brightness);
		sendCommand(text);
	}

	::snprintf(text, sizeof(text), "t0.txt=\"RIC: %u\"", ric);
	sendCommand(text);
	sendCommandAction(132U);

	::snprintf(text, sizeof(text), "t1.txt=\"%s\"", message.c_str());
	sendCommand(text);
	sendCommandAction(133U);

	m_clockDisplayTimer.stop();

	m_mode = MODE_POCSAG;
}

void CNextion::clearPOCSAGInt()
{
	sendCommand("t0.txt=\"Waiting\"");
	sendCommandAction(134U);
	sendCommand("t1.txt=\"\"");
}

void CNextion::writeCWInt()
{
	sendCommand("t1.txt=\"Sending CW Ident\"");
	sendCommandAction(12U);
	m_clockDisplayTimer.start();

	m_mode = MODE_CW;
}

void CNextion::clearCWInt()
{
	sendCommand("t1.txt=\"MMDVM IDLE\"");
	sendCommandAction(11U);
}

void CNextion::writeGeneralInt(const std::string& callsign, unsigned int id, bool duplex)
{
	if ((callsign != m_callsign) || (id != m_id)) {
		m_callsign = callsign;
		m_id       = id;
		m_duplex   = duplex;

		redrawIdle();
	}
}

void CNextion::writeCPUInt(float temperature, float frequency, float load)
{
	m_temperature = temperature;
	m_frequency   = frequency;
	m_load        = load;

	// Only the CPU fields: drawing the whole idle page again would show
	// the layout's start-up texts until every field has come again.
	if ((m_screenLayout & LAYOUT_DIY) && (m_mode == MODE_IDLE))
		sendCPU();
}

void CNextion::sendCPU()
{
	char command[100U];

	// CPU temperature
	if ((m_screenLayout & LAYOUT_DIY) && (m_temperature != -1.0F)) {
		if (m_displayTempInF) {
			float val = (1.8F * m_temperature) + 32.0F;
			::snprintf(command, sizeof(command), "t20.txt=\"%2.1f %cF\"", val, 176);
		} else {
			::snprintf(command, sizeof(command), "t20.txt=\"%2.1f %cC\"", m_temperature, 176);
		}

		sendCommand(command);
		sendCommandAction(22U);
	}

	if (m_frequency != -1.0F) {
		// MMDVM-Info gives it in MHz
		::snprintf(command, sizeof(command), "t21.txt=\"%0.0f MHz\"", m_frequency);
		sendCommand(command);
	} else {
		sendCommand("t21.txt=\"?\"");
	}

	// The 1 minute load average, and it x 100 as a whole number for the
	// layouts' bar (as NextionDriver sent them)
	if (m_load != -1.0F) {
		::snprintf(command, sizeof(command), "t22.txt=\"%0.2f\"", m_load);
		sendCommand(command);

		::snprintf(command, sizeof(command), "cpuload.val=%d", int(m_load * 100.0F));
		sendCommand(command);
	} else {
		sendCommand("t22.txt=\"?\"");
		sendCommand("cpuload.val=0");
	}

	if (m_screenLayout & LAYOUT_DIY)
		sendDiskFree();
}

void CNextion::writeInfoInt(unsigned int rxFrequency, unsigned int txFrequency, const std::string& location)
{
	// An unknown frequency (0) is shown as "?"
	// double: a float holds 7 digits, and layout 3 shows 9 (438.600000)
	double rx = (rxFrequency != 0U) ? double(rxFrequency) / 1000000.0 : -1.0;
	double tx = (txFrequency != 0U) ? double(txFrequency) / 1000000.0 : -1.0;

	if ((rx != m_rxFrequency) || (tx != m_txFrequency) || (location != m_location)) {
		m_rxFrequency = rx;
		m_txFrequency = tx;
		m_location    = location;

		redrawIdle();
	}
}

void CNextion::writeIPInt(const std::string& name, const std::string& ipV4, const std::string& ipV6)
{
	std::string address;

	if (!ipV4.empty())
		address = ipV4;
	else if (!ipV6.empty())
		address = ipV6;

	// Layout 3: "<interface>:<address>", as MMDVM-Host and NextionDriver
	// sent it (layouts show the part after the colon)
	if ((m_screenLayout & LAYOUT_DIY) && !address.empty() && !name.empty())
		address = name + ":" + address;

	if (address != m_ipAddress) {
		m_ipAddress = address;

		redrawIdle();
	}
}

void CNextion::clockInt(unsigned int ms)
{
	assert(m_serial != nullptr);

	if (m_screenLayout & LAYOUT_DIY) {
		// Asleep after a while idle; a touch wakes it (thup), a call too
		m_sleepTimer.clock(ms);
		if (m_sleepTimer.isRunning() && m_sleepTimer.hasExpired()) {
			m_sleepTimer.stop();
			if (!m_asleep && !m_held && (m_mode == MODE_IDLE)) {
				LogDebug("Nextion going to sleep");
				queueCommand("thup=1");
				queueCommand("sleep=1");
				m_asleep = true;
			}
		}

		// The screen's clock kept right
		m_rtcTimer.clock(ms);
		if (m_rtcTimer.isRunning() && m_rtcTimer.hasExpired()) {
			sendRTC();
			m_rtcTimer.start();
		}

		// A new talkgroup names file
		m_groupsTimer.clock(ms);
		if (m_groupsTimer.isRunning() && m_groupsTimer.hasExpired()) {
			readGroups();
			m_groupsTimer.start();
		}
	}

	// Update the clock display in IDLE mode every 400ms
	m_clockDisplayTimer.clock(ms);
	if (m_displayClock && (m_mode == MODE_IDLE || m_mode == MODE_CW) && m_clockDisplayTimer.isRunning() && m_clockDisplayTimer.hasExpired()) {
		// Skip clock update if the output buffer is more than half full.
		// Over MQTT the drain rate is much slower than direct serial, so
		// piling on timer commands can cause buffer overflow.
		if (m_output.freeSpace() > 100U)
			sendClock();

		m_clockDisplayTimer.start(); // restart the clock display timer
	}

	// Settings that came together are drawn together.
	if (m_redrawIdle) {
		m_redrawIdle = false;
		if (m_mode == MODE_IDLE)
			setIdle();
	}

	// Timeout stale waits — over MQTT, responses can be lost or delayed.
	// Without this, m_waiting stays true forever and the queue never drains.
	m_waitingTimer.clock(ms);
	if (m_waiting && m_waitingTimer.isRunning() && m_waitingTimer.hasExpired()) {
		LogDebug("Nextion response timeout, resuming queue");
		m_pending = 0U;
		m_waiting = false;
		m_waitingTimer.stop();
	}

	// Collect each message from the screen up to its FF FF FF.
	unsigned char c;
	while (m_serial->read(&c, 1U) == 1U) {
		if (m_replyLength < MAX_REPLY_LENGTH)
			m_reply[m_replyLength] = c;
		m_replyLength++;

		// The FF FF FF at its end, counted as they come: past
		// MAX_REPLY_LENGTH the bytes are not kept, so not read back
		m_replyFFs = (c == 0xFFU) ? m_replyFFs + 1U : 0U;

		if ((m_replyLength >= 3U) && (m_replyFFs >= 3U)) {
			unsigned int length = m_replyLength - 3U;
			m_replyLength = 0U;
			m_replyFFs = 0U;

			if (length > MAX_REPLY_LENGTH) {
				LogWarning("Nextion message too long - %u bytes", length);
				continue;
			}

			processReply(m_reply, length);

			// The last reply to what was sent: send more
			if ((length == 1U) && !m_waiting)
				break;
		}
	}

	// Send the instructions waiting, as many as fit in a batch, then wait
	// for a reply to each. Over a modem each write and reply takes about
	// 100ms however short it is, so one at a time a page took seconds.
	if (!m_waiting) {
		if (!m_output.isEmpty()) {
			unsigned char buffer[MAX_BATCH_LENGTH + 200U];
			unsigned int length = 0U;
			unsigned int count  = 0U;

			m_mutex.lock();

			while (!m_output.isEmpty()) {
				unsigned char len = 0U;
				m_output.peek(&len, 1U);
				if ((count > 0U) && ((length + len) > MAX_BATCH_LENGTH))
					break;

				m_output.getData(&len, 1U);
				m_output.getData(buffer + length, len);
				const unsigned char* instruction = buffer + length;
				length += len;
				count++;

				// After waking the screen, or asking it who it is, it
				// drops what follows at once: the rest after its reply.
				// After a status's click, its script must see the fields
				// as they are, before the next call's change them (a
				// layout's own last heard list copies them at a call's
				// end, and a quick key's start and end come together)
				if (((len >= 10U) && (::memcmp(instruction, "sleep=0", 7U) == 0)) ||
				    ((len >= 10U) && (::memcmp(instruction, "connect", 7U) == 0)) ||
				    ((len >= 13U) && (::memcmp(instruction, "click S0,1", 10U) == 0)))
					break;
			}

			m_mutex.unlock();

			// CUtils::dump(1U, "Nextion output", buffer, length);

			m_serial->write(buffer, length);

			m_pending = count;
			m_waiting = true;
			m_waitingTimer.start();
		}
	}
}

void CNextion::processReply(const unsigned char* data, unsigned int length)
{
	if ((length == 1U) && (data[0U] != 0x70U)) {
		switch (data[0U]) {
			case 0x00U:	// Invalid instruction
			case 0x02U:	// Invalid component ID
			case 0x03U:	// Invalid page ID
			case 0x04U:	// Invalid picture ID
			case 0x05U:	// Invalid font ID
			case 0x06U:	// Invalid file operation
			case 0x09U:	// Invalid CRC
			case 0x11U:	// Invalid baud rate setting
			case 0x12U:	// Invalid waveform ID or channel number
			case 0x1BU:	// Invalid variable operation
			case 0x1CU:	// Assignment failed to assign
			case 0x1DU:	// EEPROM operation failed
			case 0x1EU:	// Invalid quantity of parameters
			case 0x1FU:	// IO operation failed
			case 0x20U:	// Escape character invalid
			case 0x23U:	// Variable name too long
			case 0x24U:	// Serial buffer overflow
				LogWarning("Nextion error response - 0x%02X", data[0U]);
				break;
			case 0x1AU:	// Invalid variable name or operation
				LogDebug("Nextion error response - 0x%02X", data[0U]);
				break;
			case 0x01U:	// Instruction successful
				break;
			case 0x86U:	// Gone to sleep by itself
				m_asleep = true;
				return;
			case 0x87U:	// Woken by itself (a touch)
				m_asleep = false;
				if (m_screenLayout & LAYOUT_DIY) {
					// What it missed asleep
					sendScreenData(m_page);
					idleTouched();
				}
				return;
			default:	// Unhandled response
				LogWarning("Unknown Nextion response - 0x%02X", data[0U]);
				break;
		}

		if (m_pending > 0U)
			m_pending--;
		if (m_pending == 0U) {
			m_waiting = false;
			m_waitingTimer.stop();
		}
		return;
	}

	// A command from the screen (ON7LDS layouts): 0x2A, a command byte
	// and its parameters
	if ((length >= 2U) && (data[0U] == 0x2AU)) {
		screenCommand(data, length);
		return;
	}

	// The reply to connect: its model and serial number
	// ("comok 1,30614-0,NX4832K035_011R,163,61699,E46A...,16777216")
	if ((length > 6U) && (::memcmp(data, "comok ", 6U) == 0)) {
		std::vector<std::string> parts;
		std::stringstream ss(std::string((const char*)data + 6U, length - 6U));
		std::string part;
		while (std::getline(ss, part, ','))
			parts.push_back(part);

		if (parts.size() >= 6U) {
			m_model    = parts[2U];
			m_serialNo = parts[5U];
			LogMessage("Nextion model %s, serial number %s", m_model.c_str(), m_serialNo.c_str());
		}

		// It answers connect in place of the usual reply
		if (m_pending > 0U)
			m_pending--;
		if (m_pending == 0U) {
			m_waiting = false;
			m_waitingTimer.stop();
		}
		return;
	}

	// The answer to a get (0x70 text, 0x71 number), sent over MQTT: it
	// takes the place of the usual reply
	if ((data[0U] == 0x70U) || (data[0U] == 0x71U)) {
		if (data[0U] == 0x70U)
			LogDebug("Nextion text \"%.*s\"", int(length - 1U), data + 1U);
		else if (length >= 5U)
			LogDebug("Nextion number %u", (unsigned int)(data[1U] | (data[2U] << 8) | (data[3U] << 16) | (data[4U] << 24)));

		if (m_pending > 0U)
			m_pending--;
		if (m_pending == 0U) {
			m_waiting = false;
			m_waitingTimer.stop();
		}
		return;
	}

	// Touch events, page numbers, start-up notices
	if (data[0U] == 0x65U)
		idleTouched();

	LogDebug("Nextion message 0x%02X, %u bytes", data[0U], length);
}

void CNextion::writeCommandInt(const std::string& command)
{
	if (command.empty() || (command.size() > 196U)) {
		LogWarning("Invalid Nextion command from MQTT, %u bytes", (unsigned int)command.size());
		return;
	}

	sendCommand(command);
}

void CNextion::close()
{
	// Freed once: MMDVM-Display calls close() and then deletes the display,
	// whose destructor deletes m_serial too
	if (m_serial != nullptr) {
		m_serial->close();
		delete m_serial;
		m_serial = nullptr;
	}
}

void CNextion::sendCommandAction(unsigned int status)
{
	if (!(m_screenLayout & LAYOUT_DIY))
		return;

	char text[30U];
	::snprintf(text, sizeof(text), "MMDVM.status.val=%u", status);

	sendCommand(text);
	sendCommand("click S0,1");
}

void CNextion::sendClock()
{
	time_t currentTime;
	struct tm *Time;
	::time(&currentTime);                   // Get the current time

	if (m_utc)
		Time = ::gmtime(&currentTime);
	else
		Time = ::localtime(&currentTime);

	::setlocale(LC_TIME,"");
	char dateText[50U], timeText[50U];
	// strftime() leaves the buffer undefined when the result does not fit
	if (::strftime(dateText, 50, m_dateFormat.c_str(), Time) == 0U)
		dateText[0] = '\0';
	if (::strftime(timeText, 50, m_timeFormat.c_str(), Time) == 0U)
		timeText[0] = '\0';

	char text[120U];
	::snprintf(text, sizeof(text), "t2.txt=\"%s %s\"", dateText, timeText);
	sendCommand(text);
}

void CNextion::redrawIdle()
{
	if (m_mode == MODE_IDLE)
		m_redrawIdle = true;
}

void CNextion::sendCommand(const std::string& command)
{
	if (m_screenLayout & LAYOUT_DIY) {
		track(command);

		// On its own pages, or asleep: kept for when it asks for them
		if (m_held || m_asleep)
			return;
	}

	queueCommand(command);
}

void CNextion::queueCommand(const std::string& command)
{
	unsigned char len = (unsigned char)command.size();
	unsigned char c = len + 3U;

	m_mutex.lock();

	// A whole instruction or none: a part would garble every one after it.
	if (!m_output.hasSpace(len + 4U)) {
		m_mutex.unlock();
		LogWarning("Nextion buffer full, instruction dropped");
		return;
	}

	m_output.addData(&c, 1U);
	m_output.addData((unsigned char*)command.c_str(), len);
	m_output.addData((unsigned char*)"\xFF\xFF\xFF", 3U);

	m_mutex.unlock();
}

// The ON7LDS DIY layout (3). The screen asks for what it shows beyond the
// other layouts with commands of its own (0x2A 0xFC-0xFE), as it did of
// NextionDriver: a page's fields again (after its own pages, or waking),
// a page's last heard list, its model; and it shows the modes and
// networks enabled, the disk's free space and talkgroup names.

void CNextion::setDIY(const std::string& groupsFile, unsigned int sleepWhenIdle, const std::string& diskPath, const std::string& dmrIdFile, const std::string& nxdnIdFile)
{
	if (!(m_screenLayout & LAYOUT_DIY))
		return;

	m_groupsFile    = groupsFile;
	m_sleepWhenIdle = sleepWhenIdle;
	m_diskPath      = diskPath;

	if (!m_groupsFile.empty()) {
		readGroups();
		m_groupsTimer.start();
	}

	if (!dmrIdFile.empty())
		m_dmrIds = new CIdLookup(dmrIdFile);
	if (!nxdnIdFile.empty())
		m_nxdnIds = new CIdLookup(nxdnIdFile);
}

void CNextion::track(const std::string& command)
{
	if (command.compare(0U, 5U, "page ") == 0) {
		const std::string name = command.substr(5U);
		for (unsigned int i = 0U; i < NEXTION_PAGES; i++) {
			if (name == PAGE_NAMES[i]) {
				m_page = i;
				// The screen shows a page as designed until told more
				m_fields[i].clear();
				break;
			}
		}

		// A call wakes the screen; it sleeps only when idle
		if (m_page != PAGE_MMDVM) {
			m_sleepTimer.stop();
			wake();
		}
		return;
	}

	static const std::string STATUS = "MMDVM.status.val=";
	if (command.compare(0U, STATUS.size(), STATUS) == 0) {
		callStatus((unsigned int)::atoi(command.c_str() + STATUS.size()));
		return;
	}

	// A text field: t<n>.txt=...
	std::string::size_type eq = command.find(".txt=");
	if ((eq != std::string::npos) && (eq > 1U) && (command[0U] == 't') &&
	    (command.find_first_not_of("0123456789", 1U) == eq)) {
		const std::string field = command.substr(0U, eq + 4U);
		m_fields[m_page][field] = command;
		if (m_inCall[m_page] && !m_heard[m_page].empty())
			m_heard[m_page].front()[field] = command;
	}
}

void CNextion::callStatus(unsigned int status)
{
	switch (status) {
	// A call starts: kept as the page shows it, and as it goes on
	case 42U:	// D-Star
	case 62U:	// DMR slot 1
	case 70U:	// DMR slot 2
	case 82U:	// YSF
	case 102U:	// P25
	case 122U:	// NXDN
	case 132U:	// POCSAG
		m_heard[m_page].push_front(m_fields[m_page]);
		if (m_heard[m_page].size() > LAST_HEARD)
			m_heard[m_page].pop_back();
		m_inCall[m_page] = true;
		break;
	// The call's end, or listening again
	case 64U:
	case 72U:
	case 41U:
	case 61U:
	case 69U:
	case 81U:
	case 101U:
	case 121U:
	case 134U:
		m_inCall[m_page] = false;
		break;
	default:
		break;
	}
}

void CNextion::screenCommand(const unsigned char* data, unsigned int length)
{
	assert(data != nullptr);
	assert(length >= 2U);

	LogMessage("Nextion command 0x%02X", data[1U]);

	if (m_screenLayout & LAYOUT_DIY) {
		idleTouched();

		// The screen sends 0xFE for 0
		switch (data[1U]) {
		case 0xFEU:	// A page's fields again
			if (length == 3U) {
				sendScreenData((data[2U] == 0xFEU) ? 0U : data[2U]);
				return;
			}
			break;
		case 0xFDU:	// A page's last heard list, so many lines
			if (length == 4U) {
				sendLastHeard((data[2U] == 0xFEU) ? 0U : data[2U], data[3U]);
				return;
			}
			break;
		case 0xFCU:
			if (length == 3U) {
				if (data[2U] == 2U) {
					sendModel();
				} else if ((data[2U] == 0U) || (data[2U] == 0xFEU)) {
					// Back from its own pages: a call that came
					// meanwhile is shown as it is now
					bool held = m_held;
					m_held = false;
					LogDebug("Nextion updates resumed");
					if (held && (m_page != PAGE_MMDVM)) {
						queueCommand(std::string("page ") + PAGE_NAMES[m_page]);
						sendScreenData(m_page);
					}
				} else {
					// On its own pages: no updates until it is back
					m_held = true;
					LogDebug("Nextion updates held");
				}
				return;
			}
			break;
		default:
			break;
		}
	}

	// Anything else is passed on as it came, without the 0x2A, for another
	// program to act on; the screen's buttons run nothing here.
	if (m_mqtt != nullptr)
		m_mqtt->publish("nextion-out", data + 1U, length - 1U);
}

void CNextion::sendScreenData(unsigned int page)
{
	if (page >= NEXTION_PAGES) {
		LogWarning("Nextion asked for page %u, which does not exist", page);
		return;
	}

	for (const auto& it : m_fields[page])
		queueCommand(it.second);

	if (page == PAGE_MMDVM)
		sendModes(true);
}

void CNextion::sendLastHeard(unsigned int page, unsigned int count)
{
	if (page >= NEXTION_PAGES) {
		LogWarning("Nextion asked for the last heard of page %u, which does not exist", page);
		return;
	}

	// Line n's field t<f> is LH<n>t<f>, the newest call first
	unsigned int n = 0U;
	for (const auto& call : m_heard[page]) {
		if (n >= count)
			break;

		char prefix[10U];
		::snprintf(prefix, sizeof(prefix), "LH%u", n);
		for (const auto& it : call) {
			// Empty fields are left out
			if (it.second.compare(it.second.size() - 2U, 2U, "\"\"") != 0)
				queueCommand(prefix + it.second);
		}

		n++;
	}
}

void CNextion::sendModel()
{
	if (m_model.empty()) {
		queueCommand("msg1.txt=\"Model Unknown\"");
		queueCommand("msg2.txt=\"\"");
	} else {
		queueCommand("msg1.txt=\"" + m_model + "\"");
		queueCommand("msg2.txt=\"" + m_serialNo + "\"");
	}
}

void CNextion::writeModesInt(const CHostModes& modes)
{
	bool changed = !m_modesKnown || (modes != m_modes);

	m_modes      = modes;
	m_modesKnown = true;

	if (changed && (m_screenLayout & LAYOUT_DIY) && (m_mode == MODE_IDLE))
		sendModes(false);
}

void CNextion::writeProgramsInt(const std::vector<std::string>& running)
{
	bool changed = !m_programsKnown || (running != m_running);

	m_running       = running;
	m_programsKnown = true;

	if (changed && (m_screenLayout & LAYOUT_DIY) && (m_mode == MODE_IDLE))
		sendModes(false);
}

// Whether a program is running, by MMDVM-Info's list (any case); until
// the list comes, it is taken to be.
bool CNextion::isRunning(const char* name1, const char* name2) const
{
	if (!m_programsKnown)
		return true;

	for (const auto& it : m_running) {
		std::string program;
		for (char c : it)
			program += char(::tolower((unsigned char)c));

		for (const char* name : {name1, name2}) {
			if (name == nullptr)
				continue;

			std::string wanted;
			for (const char* p = name; *p != '\0'; p++)
				wanted += char(::tolower((unsigned char)*p));

			if (program.find(wanted) != std::string::npos)
				return true;
		}
	}

	return false;
}

// The modes MMDVM-Host has enabled (A1-A6), and its networks with their
// gateways running, and the internet (an address) (N0-N6), green or grey.
void CNextion::sendModes(bool direct)
{
	if (!m_modesKnown)
		return;

	const struct {
		const char* field;
		bool        on;
	} fields[] = {
		{"A1", m_modes.dstar},
		{"A2", m_modes.dmr},
		{"A3", m_modes.ysf},
		{"A4", m_modes.p25},
		{"A6", m_modes.nxdn},
		{"N0", !m_ipAddress.empty()},
		{"N1", m_modes.dstarNet && isRunning("DStarGateway", "ircDDBGateway")},
		{"N2", m_modes.dmrNet && isRunning("DMRGateway")},
		{"N3", m_modes.ysfNet && isRunning("YSFGateway", "DGIdGateway")},
		{"N4", m_modes.p25Net && isRunning("P25Gateway")},
		{"N6", m_modes.nxdnNet && isRunning("NXDNGateway")}
	};

	for (const auto& it : fields) {
		char text[30U];

		::snprintf(text, sizeof(text), "%s.bco=%u", it.field, it.on ? BCO_ENABLED : BCO_DISABLED);
		if (direct) queueCommand(text); else sendCommand(text);

		::snprintf(text, sizeof(text), "%s.pco=%u", it.field, it.on ? PCO_ENABLED : PCO_DISABLED);
		if (direct) queueCommand(text); else sendCommand(text);
	}

	if (direct) {
		queueCommand("MMDVM.status.val=24");
		queueCommand("click S0,1");
	} else {
		sendCommandAction(24U);
	}
}

// A talkgroup's name: slot 1's in t9, slot 2's in t8 (as NextionDriver).
void CNextion::sendTGName(unsigned int slotNo, bool group, unsigned int dst)
{
	if (!(m_screenLayout & LAYOUT_DIY) || m_groupsFile.empty())
		return;

	std::string name;
	if (group) {
		auto it = m_groups.find(dst);
		if (it != m_groups.end())
			name = it->second;
	}

	sendCommand(std::string(slotNo == 1U ? "t8" : "t9") + ".txt=\"" + name + "\"");
}

// The talkgroup names: a JSON object ({"91": "Worldwide", ...}, as
// BrandMeister's), or a line each, the number then the name ("91 Worldwide",
// "91,Worldwide"); read again when the file changes.
void CNextion::readGroups()
{
	struct stat st;
	if (::stat(m_groupsFile.c_str(), &st) != 0) {
		if (m_groupsTime != (time_t)-1)
			LogWarning("Cannot read the groups file %s", m_groupsFile.c_str());
		m_groupsTime = (time_t)-1;
		return;
	}

	if (st.st_mtime == m_groupsTime)
		return;
	m_groupsTime = st.st_mtime;

	std::ifstream file(m_groupsFile.c_str());
	std::stringstream ss;
	ss << file.rdbuf();
	const std::string text = ss.str();

	std::map<unsigned int, std::string> groups;

	// A name as the screen can show it
	auto add = [&groups](unsigned long id, const std::string& raw) {
		std::string name;
		for (char c : raw) {
			if (fieldChar(c))
				name += c;
		}
		name.erase(0U, name.find_first_not_of(" \t,;:"));
		name.erase(name.find_last_not_of(" \t,;:") + 1U);
		if (name.size() > 40U)
			name.resize(40U);
		if ((id > 0UL) && (id <= 0xFFFFFFUL) && !name.empty())
			groups[(unsigned int)id] = name;
	};

	std::string::size_type first = text.find_first_not_of(" \t\r\n");
	if ((first != std::string::npos) && (text[first] == '{')) {
		nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
		if (j.is_discarded() || !j.is_object()) {
			LogWarning("The groups file %s is not valid JSON", m_groupsFile.c_str());
			return;
		}

		for (const auto& it : j.items()) {
			char* end = nullptr;
			unsigned long id = ::strtoul(it.key().c_str(), &end, 10);
			if ((end != it.key().c_str()) && (*end == '\0') && it.value().is_string())
				add(id, it.value().get<std::string>());
		}
	} else {
		std::istringstream lines(text);
		std::string line;
		while (std::getline(lines, line)) {
			const char* p = line.c_str();
			while ((*p == ' ') || (*p == '\t') || (*p == '"'))
				p++;
			if (*p == '#')
				continue;

			char* end = nullptr;
			unsigned long id = ::strtoul(p, &end, 10);
			if (end != p)
				add(id, std::string(end));
		}
	}

	m_groups.swap(groups);
	LogMessage("Read %u talkgroup names from %s", (unsigned int)m_groups.size(), m_groupsFile.c_str());
}

// The free space (%) of the disk holding m_diskPath, in t23.
void CNextion::sendDiskFree()
{
	if (m_diskPath.empty())
		return;

	int percent = -1;

#if defined(_WIN32) || defined(_WIN64)
	std::string dir = m_diskPath;
	std::string::size_type slash = dir.find_last_of("\\/");
	dir = (slash == std::string::npos) ? "." : dir.substr(0U, slash + 1U);

	ULARGE_INTEGER available, total;
	if (::GetDiskFreeSpaceExA(dir.c_str(), &available, &total, nullptr) && (total.QuadPart > 0U))
		percent = int((available.QuadPart * 100U) / total.QuadPart);
#else
	struct statvfs st;
	if ((::statvfs(m_diskPath.c_str(), &st) == 0) && (st.f_blocks > 0U))
		percent = int(((unsigned long long)st.f_bavail * 100ULL) / (unsigned long long)st.f_blocks);
#endif

	char text[30U];
	if (percent < 0)
		::snprintf(text, sizeof(text), "t23.txt=\"??\"");
	else
		::snprintf(text, sizeof(text), "t23.txt=\"%d\"", percent);
	sendCommand(text);
}

// The caller's details, looked up as a call starts (before it is written:
// writeDMRInt sends them with the caller, as NextionDriver did). Fields not
// known are sent empty, so none of the last caller's stays.
void CNextion::writeDMRCallerInt(unsigned int slotNo, unsigned int id)
{
	m_dmrCaller = CIdEntry();
	m_dmrCallerSet = (m_screenLayout & LAYOUT_DIY) && (m_dmrIds != nullptr);
	if (!m_dmrCallerSet || (id == 0U))
		return;

	if (!m_dmrIds->find(id, m_dmrCaller))
		m_dmrCaller.callsign = std::to_string(id);

	// The list may not have the country: the ID says it
	if (m_dmrCaller.country.empty())
		m_dmrCaller.country = CIdLookup::countryOf(id);
}

void CNextion::writeNXDNCallerInt(unsigned int id)
{
	m_nxdnCaller = CIdEntry();
	m_nxdnCallerSet = (m_screenLayout & LAYOUT_DIY) && (m_nxdnIds != nullptr);
	if (!m_nxdnCallerSet || (id == 0U))
		return;

	if (!m_nxdnIds->find(id, m_nxdnCaller))
		m_nxdnCaller.callsign = std::to_string(id);
}

// On the DMR page t13 callsign, t15 city, t16 state, t17 country, and the
// caller's name in the slot's own field so the two timeslots do not overwrite
// each other: TS1 t19, TS2 t14 (status 78).
void CNextion::sendDMRCaller(unsigned int slotNo)
{
	if (!m_dmrCallerSet)
		return;
	m_dmrCallerSet = false;

	const char* nameObj = (slotNo == 1U) ? "t19" : "t14";

	sendCommand("t13.txt=\"" + clean(m_dmrCaller.callsign) + "\"");
	sendCommand(std::string(nameObj) + ".txt=\"" + clean(m_dmrCaller.name) + "\"");
	sendCommand("t15.txt=\"" + clean(m_dmrCaller.city) + "\"");
	sendCommand("t16.txt=\"" + clean(m_dmrCaller.state) + "\"");
	sendCommand("t17.txt=\"" + clean(m_dmrCaller.country) + "\"");
	sendCommandAction(78U);
}

// On the other pages t18 callsign, t19 name (status 68).
void CNextion::sendNXDNCaller()
{
	if (!m_nxdnCallerSet)
		return;
	m_nxdnCallerSet = false;

	sendCommand("t18.txt=\"" + clean(m_nxdnCaller.callsign) + "\"");
	sendCommand("t19.txt=\"" + clean(m_nxdnCaller.name) + "\"");
	sendCommandAction(68U);
}

// Text as a field can hold it: no quotes or backslashes, not too long.
std::string CNextion::clean(const std::string& text)
{
	std::string out;
	for (char c : text) {
		if (fieldChar(c))
			out += c;
	}

	if (out.size() > 40U)
		out.resize(40U);

	return out;
}

// The screen's real-time clock (K and Intelligent series; others have
// none, and refuse it harmlessly), as the host's: local time, or UTC.
void CNextion::sendRTC()
{
	time_t now;
	::time(&now);

	struct tm* t = m_utc ? ::gmtime(&now) : ::localtime(&now);
	if (t == nullptr)
		return;

	const int values[] = {t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec};
	for (unsigned int i = 0U; i < 6U; i++) {
		char text[20U];
		::snprintf(text, sizeof(text), "rtc%u=%d", i, values[i]);
		sendCommand(text);
	}
}

void CNextion::wake()
{
	if (!(m_screenLayout & LAYOUT_DIY) || !m_asleep)
		return;

	LogDebug("Nextion waking");
	m_asleep = false;

	queueCommand("sleep=0");
}

// A touch, or a command from the screen: idle a while longer.
void CNextion::idleTouched()
{
	if ((m_screenLayout & LAYOUT_DIY) && (m_sleepWhenIdle > 0U) && (m_mode == MODE_IDLE) && !m_asleep)
		m_sleepTimer.start(m_sleepWhenIdle);
}
