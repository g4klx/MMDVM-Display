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

#if !defined(OLED_H)
#define	OLED_H

#if defined(USE_OLED)

#define OLED_STATUSBAR 0
#define OLED_LINE1 8 //16
#define OLED_LINE2 18 //26 
#define OLED_LINE3 28 //36
#define OLED_LINE4 37 //46
#define OLED_LINE5 47 //56
#define OLED_LINE6 57

#include "Display.h"
#include "Defines.h"

#include <string>
#include <fstream>
#include <sstream>
#include <iostream> // for cpu temp value extraction
#include <cmath>    // for cpu temp value rounding

#include "ArduiPi_OLED_lib.h"
#include "Adafruit_GFX.h"
#include "ArduiPi_OLED.h"

class COLED : public CDisplay 
{
public:
	COLED(const std::string& callsign, unsigned int id, bool duplex, unsigned char displayType, unsigned char displayBrighness, bool displayInvert, bool displayScroll, bool displayRotate, bool displayLogoScreensaver);
	virtual ~COLED();

	virtual bool open() override;

	virtual void close() override;

protected:
	virtual void setIdleInt() override;
	virtual void setErrorInt() override;
	virtual void setLockoutInt() override;
	virtual void setQuitInt() override;
  
	virtual void writeDStarInt(const std::string& my1, const std::string& my2, const std::string& your, const std::string& type, const std::string& reflector) override;
	virtual void clearDStarInt() override;

	virtual void writeDMRInt(unsigned int slotNo, const std::string& src, bool group, unsigned int dst, const std::string& type) override;
	virtual void clearDMRInt(unsigned int slotNo) override;

	virtual void writeFusionInt(const std::string& source, const std::string& dest, unsigned char dgid, const std::string& type, const std::string& origin) override;
	virtual void clearFusionInt() override;

	virtual void writeP25Int(const std::string& source, bool group, unsigned int dest, const std::string& type) override;
	virtual void clearP25Int() override;

	virtual void writeNXDNInt(const std::string& source, bool group, unsigned int dest, const std::string& type) override;
	virtual void clearNXDNInt() override;

	virtual void writePOCSAGInt(uint32_t ric, const std::string& message) override;
	virtual void clearPOCSAGInt() override;

	virtual void writeFMInt(const std::string& state) override;
	virtual void clearFMInt() override;

	virtual void writeCWInt() override;
	virtual void clearCWInt() override;

	virtual void writeCPUInt(float temperature, float frequency, float load) override;
	virtual void writeIPInt(const std::string& ipV4, const std::string& ipV6) override;

private:
	std::string   m_callsign;
	unsigned int  m_id;
	bool          m_duplex;
	std::string   m_slot1_state;
	std::string   m_slot2_state;
	unsigned char m_mode;
	unsigned char m_displayType;
	unsigned char m_displayBrightness;
	bool          m_displayInvert;
	bool          m_displayScroll;
	bool          m_displayRotate;
	bool          m_displayLogoScreensaver;
	std::string   m_ipaddress;
	float         m_temperature;
	ArduiPi_OLED  m_display;

	float readTemperature(const std::string& filePath);

	void OLED_statusbar();
};

#endif

#endif
