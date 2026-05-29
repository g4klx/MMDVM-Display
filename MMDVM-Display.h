/*
 *   Copyright (C) 2015-2021,2023,2026 by Jonathan Naylor G4KLX
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

#if !defined(MMDVM_Display_H)
#define	MMDVM_Display_H

#include "ModemSerialPort.h"
#include "Display.h"
#include "Timer.h"
#include "Conf.h"

#include <nlohmann/json.hpp>

#include <string>

class CMMDVMDisplay
{
public:
	CMMDVMDisplay(const std::string& confFile);
	~CMMDVMDisplay();

	int run();

private:
	CConf             m_conf;
	CDisplay*         m_display;
	CModemSerialPort* m_msp;
	std::string       m_hostConfName;
	std::string       m_mqttInfoName;
	bool              m_temperatureInF;
	CTimer            m_confTimer;

	bool createDisplay();

	void writeJSONMessage(const std::string& message);

	void readHost(const std::string& text);
	void readInfo(const std::string& text);
	void readDisplay(const unsigned char* data, unsigned int length);

	void parseMMDVM(const nlohmann::json& json);
	void parseRSSI(const nlohmann::json& json);
	void parseBER(const nlohmann::json& json);
	void parseText(const nlohmann::json& json);
	void parseDStar(const nlohmann::json& json);
	void parseDMR(const nlohmann::json& json);
	void parseYSF(const nlohmann::json& json);
	void parseP25(const nlohmann::json& json);
	void parseNXDN(const nlohmann::json& json);
	void parsePOCSAG(const nlohmann::json& json);
	void parseFM(const nlohmann::json& json);

	void parseCPU(const nlohmann::json& json);
	void parsePrograms(const nlohmann::json& json);
	void parseAddresses(const nlohmann::json& json);
	void parseHostConfig(const nlohmann::json& json);

	void pollHostConfig();

	static void onDisplay(const unsigned char* data, unsigned int length);
	static void onHost(const unsigned char* data, unsigned int length);
	static void onInfo(const unsigned char* data, unsigned int length);
};

#endif
