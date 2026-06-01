/*
 *   Copyright (C) 2016,2017,2018,2020,2023,2026 by Jonathan Naylor G4KLX
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

#if !defined(NEXTION_H)
#define	NEXTION_H

#include "Display.h"
#include "Defines.h"
#include "RingBuffer.h"
#include "SerialPort.h"
#include "Mutex.h"
#include "Timer.h"
#include "Thread.h"
#include <string>

class CNextion : public CDisplay
{
public:
	CNextion(const std::string& callsign, unsigned int id, bool duplex, ISerialPort* serial, unsigned int brightness, bool displayClock, bool utc, unsigned int idleBrightness, unsigned int screenLayout, bool displayTempInF);
	virtual ~CNextion();

	virtual bool open() override;

	virtual void close() override;

protected:
	virtual void setIdleInt() override;
	virtual void setErrorInt() override;
	virtual void setLockoutInt() override;
	virtual void setQuitInt() override;

	virtual void writeDStarInt(const std::string& my1, const std::string& my2, const std::string& your, const std::string& type, const std::string& reflector) override;
	virtual void writeDStarRSSIInt(int rssi) override;
	virtual void writeDStarBERInt(float ber) override;
	virtual void writeDStarTextInt(const std::string& text) override;
	virtual void clearDStarInt() override;

	virtual void writeDMRInt(unsigned int slotNo, const std::string& src, bool group, unsigned int dst, const std::string& type) override;
	virtual void writeDMRRSSIInt(unsigned int slotNo, int rssi) override;
	virtual void writeDMRTAInt(unsigned int slotNo, const std::string& talkerAlias) override;
	virtual void writeDMRBERInt(unsigned int slotNo, float ber) override;
	virtual void clearDMRInt(unsigned int slotNo) override;

	virtual void writeFusionInt(const std::string& source, const std::string& dest, unsigned char dgid, const std::string& type, const std::string& origin) override;
	virtual void writeFusionRSSIInt(int rssi) override;
	virtual void writeFusionBERInt(float ber) override;
	virtual void clearFusionInt() override;

	virtual void writeP25Int(const std::string& source, bool group, unsigned int dest, const std::string& type) override;
	virtual void writeP25RSSIInt(int rssi) override;
	virtual void writeP25BERInt(float ber) override;
	virtual void clearP25Int() override;

	virtual void writeNXDNInt(const std::string& source, bool group, unsigned int dest, const std::string& type) override;
	virtual void writeNXDNRSSIInt(int rssi) override;
	virtual void writeNXDNBERInt(float ber) override;
	virtual void clearNXDNInt() override;

	virtual void writePOCSAGInt(uint32_t ric, const std::string& message) override;
	virtual void clearPOCSAGInt() override;

	virtual void writeFMInt(const std::string& state) override;
	virtual void writeFMRSSIInt(int rssi) override;
	virtual void clearFMInt() override;

	virtual void writeCWInt() override;
	virtual void clearCWInt() override;

	virtual void writeCPUInt(float temperature, float frequency, float load) override;
	virtual void writeInfoInt(float rxFrequency, float txFrequency, const std::string& location) override;
	virtual void writeIPInt(const std::string& ipV4, const std::string& ipV6) override;

	virtual void clockInt(unsigned int ms) override;

private:
	std::string    m_callsign;
	unsigned int   m_id;
	bool           m_duplex;
	std::string    m_ipV4;
	std::string    m_ipV6;
	float          m_temperature;
	float          m_frequency;
	float          m_load;
	float          m_rxFrequency;
	float          m_txFrequency;
	std::string    m_location;
	ISerialPort*   m_serial;
	unsigned int   m_brightness;
	unsigned char  m_mode;
	bool           m_displayClock;
	bool           m_utc;
	unsigned int   m_idleBrightness;
	unsigned int   m_screenLayout;
	CTimer         m_clockDisplayTimer;
	bool           m_displayTempInF;
	CRingBuffer<unsigned char> m_output;
	CMutex         m_mutex;
	unsigned char* m_reply;
	bool           m_waiting;
	CTimer         m_waitingTimer;

	void sendCommand(const std::string& command);
	void sendCommandAction(unsigned int status);
};

#endif
