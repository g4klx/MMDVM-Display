/*
 *   Copyright (C) 2016,2017,2018,2020,2021,2023,2026 by Jonathan Naylor G4KLX
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

#include "Display.h"
#include "Defines.h"
#include "Log.h"

#include <cstdio>
#include <cassert>
#include <cstring>

// The longest each piece of text from MMDVM-Host or MMDVM-Info is taken
// to be: longer is cut here, so no display driver is handed more than it
// was written for. Generous for real traffic (a D-Star text is 20
// characters, a DMR talker alias at most 31, a DAPNET message 80).
static const std::string::size_type MAX_CALLSIGN = 16U;  // callsigns, suffixes, types
static const std::string::size_type MAX_SOURCE   = 40U;  // a caller with its name
static const std::string::size_type MAX_DSTAR    = 20U;  // D-Star slow data text
static const std::string::size_type MAX_ALIAS    = 40U;  // DMR talker alias
static const std::string::size_type MAX_POCSAG   = 160U; // a paging message
static const std::string::size_type MAX_STATUS   = 32U;  // FM status
static const std::string::size_type MAX_LOCATION = 50U;
static const std::string::size_type MAX_IFNAME   = 16U;
static const std::string::size_type MAX_ADDRESS  = 46U;  // an IPv6 address

// Text as every display takes it: no control characters (a newline would
// end an LCDproc command early, and no display shows one), never 0xFF
// (three end a Nextion command; valid UTF-8 never holds one), and no
// longer than length.
static std::string limit(const std::string& text, std::string::size_type length)
{
	std::string out;
	for (char c : text) {
		unsigned char u = (unsigned char)c;
		if ((u >= 0x20U) && (u != 0x7FU) && (u != 0xFFU))
			out += c;
		if (out.size() >= length)
			break;
	}
	return out;
}

// DMR has two slots: anything else is ignored, as the drivers index
// their per-slot state by it
static bool validSlot(unsigned int slotNo)
{
	return (slotNo == 1U) || (slotNo == 2U);
}

CDisplay::CDisplay() :
m_timer1(3000U, 3U),
m_timer2(3000U, 3U),
m_mode1(MODE_IDLE),
m_mode2(MODE_IDLE)
{
}

CDisplay::~CDisplay()
{
}

void CDisplay::setIdle()
{
	m_timer1.stop();
	m_timer2.stop();

	m_mode1 = MODE_IDLE;
	m_mode2 = MODE_IDLE;

	setIdleInt();
}

void CDisplay::setLockout()
{
	m_timer1.stop();
	m_timer2.stop();

	m_mode1 = MODE_IDLE;
	m_mode2 = MODE_IDLE;

	setLockoutInt();
}

void CDisplay::setError()
{
	m_timer1.stop();
	m_timer2.stop();

	m_mode1 = MODE_IDLE;
	m_mode2 = MODE_IDLE;

	setErrorInt();
}

void CDisplay::setQuit()
{
	m_timer1.stop();
	m_timer2.stop();

	m_mode1 = MODE_QUIT;
	m_mode2 = MODE_QUIT;

	setQuitInt();
}

void CDisplay::writeDStar(const std::string& my1, const std::string& my2, const std::string& your, const std::string& type, const std::string& reflector)
{
	m_timer1.start();
	m_mode1 = MODE_IDLE;

	writeDStarInt(limit(my1, MAX_CALLSIGN), limit(my2, MAX_CALLSIGN), limit(your, MAX_CALLSIGN), limit(type, MAX_CALLSIGN), limit(reflector, MAX_CALLSIGN));
}

void CDisplay::writeDStarRSSI(int rssi)
{
	if (rssi != 0)
		writeDStarRSSIInt(rssi);
}

void CDisplay::writeDStarBER(float ber)
{
	writeDStarBERInt(ber);
}

void CDisplay::writeDStarText(const std::string& text)
{
	writeDStarTextInt(limit(text, MAX_DSTAR));
}

void CDisplay::clearDStar()
{
	if (m_timer1.hasExpired()) {
		clearDStarInt();
		m_timer1.stop();
		m_mode1 = MODE_IDLE;
	} else {
		m_mode1 = MODE_DSTAR;
	}
}

void CDisplay::writeDMR(unsigned int slotNo, const std::string& src, bool group, unsigned int dst, const std::string& type)
{
	if (!validSlot(slotNo))
		return;

	if (slotNo == 1U) {
		m_timer1.start();
		m_mode1 = MODE_IDLE;
	} else {
		m_timer2.start();
		m_mode2 = MODE_IDLE;
	}

	writeDMRInt(slotNo, limit(src, MAX_SOURCE), group, dst, limit(type, MAX_CALLSIGN));
}

void CDisplay::writeDMRRSSI(unsigned int slotNo, int rssi)
{
	if (validSlot(slotNo) && (rssi != 0))
		writeDMRRSSIInt(slotNo, rssi);
}

void CDisplay::writeDMRTA(unsigned int slotNo, const std::string& talkerAlias)
{
	if (validSlot(slotNo))
		writeDMRTAInt(slotNo, limit(talkerAlias, MAX_ALIAS));
}

void CDisplay::writeDMRBER(unsigned int slotNo, float ber)
{
	if (validSlot(slotNo))
		writeDMRBERInt(slotNo, ber);
}

void CDisplay::clearDMR(unsigned int slotNo)
{
	if (!validSlot(slotNo))
		return;

	if (slotNo == 1U) {
		if (m_timer1.hasExpired()) {
			clearDMRInt(slotNo);
			m_timer1.stop();
			m_mode1 = MODE_IDLE;
		} else {
			m_mode1 = MODE_DMR;
		}
	} else {
		if (m_timer2.hasExpired()) {
			clearDMRInt(slotNo);
			m_timer2.stop();
			m_mode2 = MODE_IDLE;
		} else {
			m_mode2 = MODE_DMR;
		}
	}
}

void CDisplay::writeFusion(const std::string& source, const std::string& dest, unsigned char dgid, const std::string& type, const std::string& origin)
{
	m_timer1.start();
	m_mode1 = MODE_IDLE;

	writeFusionInt(limit(source, MAX_CALLSIGN), limit(dest, MAX_CALLSIGN), dgid, limit(type, MAX_CALLSIGN), limit(origin, MAX_CALLSIGN));
}

void CDisplay::writeFusionRSSI(int rssi)
{
	if (rssi != 0)
		writeFusionRSSIInt(rssi);
}

void CDisplay::writeFusionBER(float ber)
{
	writeFusionBERInt(ber);
}

void CDisplay::clearFusion()
{
	if (m_timer1.hasExpired()) {
		clearFusionInt();
		m_timer1.stop();
		m_mode1 = MODE_IDLE;
	} else {
		m_mode1 = MODE_YSF;
	}
}

void CDisplay::writeP25(const std::string& source, bool group, unsigned int dest, const std::string& type)
{
	m_timer1.start();
	m_mode1 = MODE_IDLE;

	writeP25Int(limit(source, MAX_SOURCE), group, dest, limit(type, MAX_CALLSIGN));
}

void CDisplay::writeP25RSSI(int rssi)
{
	if (rssi != 0)
		writeP25RSSIInt(rssi);
}

void CDisplay::writeP25BER(float ber)
{
	writeP25BERInt(ber);
}

void CDisplay::clearP25()
{
	if (m_timer1.hasExpired()) {
		clearP25Int();
		m_timer1.stop();
		m_mode1 = MODE_IDLE;
	} else {
		m_mode1 = MODE_P25;
	}
}

void CDisplay::writeNXDN(const std::string& source, bool group, unsigned int dest, const std::string& type)
{
	m_timer1.start();
	m_mode1 = MODE_IDLE;

	writeNXDNInt(limit(source, MAX_SOURCE), group, dest, limit(type, MAX_CALLSIGN));
}

void CDisplay::writeNXDNRSSI(int rssi)
{
	if (rssi != 0)
		writeNXDNRSSIInt(rssi);
}

void CDisplay::writeNXDNBER(float ber)
{
	writeNXDNBERInt(ber);
}

void CDisplay::clearNXDN()
{
	if (m_timer1.hasExpired()) {
		clearNXDNInt();
		m_timer1.stop();
		m_mode1 = MODE_IDLE;
	} else {
		m_mode1 = MODE_NXDN;
	}
}

void CDisplay::writePOCSAG(uint32_t ric, const std::string& message)
{
	m_timer1.start();
	m_mode1 = MODE_POCSAG;

	writePOCSAGInt(ric, limit(message, MAX_POCSAG));
}

void CDisplay::clearPOCSAG()
{
	if (m_timer1.hasExpired()) {
		clearPOCSAGInt();
		m_timer1.stop();
		m_mode1 = MODE_IDLE;
	} else {
		m_mode1 = MODE_POCSAG;
	}
}

void CDisplay::writeFM(const std::string& status)
{
	m_timer1.start();
	m_mode1 = MODE_FM;

	writeFMInt(limit(status, MAX_STATUS));
}

void CDisplay::writeFMRSSI(int rssi)
{
	if (rssi != 0U)
		writeFMRSSIInt(rssi);
}

void CDisplay::clearFM()
{
	if (m_timer1.hasExpired()) {
		clearFMInt();
		m_timer1.stop();
		m_mode1 = MODE_IDLE;
	} else {
		m_mode1 = MODE_FM;
	}
}

void CDisplay::writeCW()
{
	m_timer1.start();
	m_mode1 = MODE_CW;

	writeCWInt();
}

void CDisplay::writeGeneral(const std::string& callsign, unsigned int id, bool duplex)
{
	writeGeneralInt(limit(callsign, MAX_CALLSIGN), id, duplex);
}

void CDisplay::writeCPU(float temperature, float frequency, float load)
{
	writeCPUInt(temperature, frequency, load);
}

void CDisplay::writeInfo(unsigned int rxFrequency, unsigned int txFrequency, const std::string& location)
{
	writeInfoInt(rxFrequency, txFrequency, limit(location, MAX_LOCATION));
}

void CDisplay::writeIP(const std::string& name, const std::string& ipV4, const std::string& ipV6)
{
	writeIPInt(limit(name, MAX_IFNAME), limit(ipV4, MAX_ADDRESS), limit(ipV6, MAX_ADDRESS));
}

void CDisplay::writeModes(const CHostModes& modes)
{
	writeModesInt(modes);
}

void CDisplay::writePrograms(const std::vector<std::string>& running)
{
	writeProgramsInt(running);
}

void CDisplay::writeDMRCaller(unsigned int slotNo, unsigned int id)
{
	if (validSlot(slotNo))
		writeDMRCallerInt(slotNo, id);
}

void CDisplay::writeNXDNCaller(unsigned int id)
{
	writeNXDNCallerInt(id);
}

void CDisplay::writeCommand(const std::string& command)
{
	writeCommandInt(command);
}

void CDisplay::clock(unsigned int ms)
{
	m_timer1.clock(ms);
	if (m_timer1.isRunning() && m_timer1.hasExpired()) {
		switch (m_mode1) {
		case MODE_DSTAR:
			clearDStarInt();
			m_mode1 = MODE_IDLE;
			m_timer1.stop();
			break;
		case MODE_DMR:
			clearDMRInt(1U);
			m_mode1 = MODE_IDLE;
			m_timer1.stop();
			break;
		case MODE_YSF:
			clearFusionInt();
			m_mode1 = MODE_IDLE;
			m_timer1.stop();
			break;
		case MODE_P25:
			clearP25Int();
			m_mode1 = MODE_IDLE;
			m_timer1.stop();
			break;
		case MODE_NXDN:
			clearNXDNInt();
			m_mode1 = MODE_IDLE;
			m_timer1.stop();
			break;
		case MODE_POCSAG:
			clearPOCSAGInt();
			m_mode1 = MODE_IDLE;
			m_timer1.stop();
			break;
		case MODE_FM:
			clearFMInt();
			m_mode1 = MODE_IDLE;
			m_timer1.stop();
			break;
		case MODE_CW:
			clearCWInt();
			m_mode1 = MODE_IDLE;
			m_timer1.stop();
			break;
		default:
			break;
		}
	}

	// Timer/mode 2 are only used for DMR
	m_timer2.clock(ms);
	if (m_timer2.isRunning() && m_timer2.hasExpired()) {
		if (m_mode2 == MODE_DMR) {
			clearDMRInt(2U);
			m_mode2 = MODE_IDLE;
			m_timer2.stop();
		}
	}

	clockInt(ms);
}

void CDisplay::clockInt(unsigned int ms)
{
}

void CDisplay::writeDStarRSSIInt(int rssi)
{
}

void CDisplay::writeDStarBERInt(float ber)
{
}

void CDisplay::writeDStarTextInt(const std::string& text)
{
}

void CDisplay::writeDMRRSSIInt(unsigned int slotNo, int rssi)
{
}

void CDisplay::writeDMRTAInt(unsigned int slotNo, const std::string& talkerAlias)
{
}

void CDisplay::writeDMRBERInt(unsigned int slotNo, float ber)
{
}

void CDisplay::writeFusionRSSIInt(int rssi)
{
}

void CDisplay::writeFusionBERInt(float ber)
{
}

void CDisplay::writeP25RSSIInt(int rssi)
{
}

void CDisplay::writeP25BERInt(float ber)
{
}

void CDisplay::writeNXDNRSSIInt(int rssi)
{
}

void CDisplay::writeNXDNBERInt(float ber)
{
}

void CDisplay::writeFMRSSIInt(int rssi)
{
}

void CDisplay::writeGeneralInt(const std::string& callsign, unsigned int id, bool duplex)
{
}

void CDisplay::writeCPUInt(float temperature, float frequency, float load)
{
}

void CDisplay::writeInfoInt(unsigned int rxFrequency, unsigned int txFrequency, const std::string& location)
{
}

void CDisplay::writeIPInt(const std::string& name, const std::string& ipV4, const std::string& ipV6)
{
}

void CDisplay::writeModesInt(const CHostModes& modes)
{
}

void CDisplay::writeProgramsInt(const std::vector<std::string>& running)
{
}

void CDisplay::writeDMRCallerInt(unsigned int slotNo, unsigned int id)
{
}

void CDisplay::writeNXDNCallerInt(unsigned int id)
{
}

void CDisplay::writeCommandInt(const std::string& command)
{
}
