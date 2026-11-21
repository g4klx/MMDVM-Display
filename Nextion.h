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
#include "IdLookup.h"
#include "Defines.h"
#include "RingBuffer.h"
#include "SerialPort.h"
#include "Mutex.h"
#include "Timer.h"
#include "Thread.h"

#include <ctime>
#include <deque>
#include <map>
#include <string>
#include <vector>

// The pages of the ON7LDS layouts, numbered as the screen asks for them
const unsigned int NEXTION_PAGES = 9U;

class CNextion : public CDisplay
{
public:
	CNextion(ISerialPort* serial, unsigned int brightness, bool displayClock, bool utc, const std::string& dateFormat, const std::string& timeFormat, unsigned int idleBrightness, unsigned int screenLayout, bool displayTempInF);
	virtual ~CNextion();

	virtual bool open() override;

	virtual void close() override;

	// For the ON7LDS DIY layout (3) only: the talkgroup names file, the
	// seconds idle before the screen sleeps (0 = never), and a file on the
	// disk whose free space is shown
	void setDIY(const std::string& groupsFile, unsigned int sleepWhenIdle, const std::string& diskPath, const std::string& dmrIdFile, const std::string& nxdnIdFile);

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

	virtual void writeGeneralInt(const std::string& callsign, unsigned int id, bool duplex) override;
	virtual void writeCPUInt(float temperature, float frequency, float load) override;
	virtual void writeInfoInt(unsigned int rxFrequency, unsigned int txFrequency, const std::string& location) override;
	virtual void writeIPInt(const std::string& name, const std::string& ipV4, const std::string& ipV6) override;
	virtual void writeModesInt(const CHostModes& modes) override;
	virtual void writeProgramsInt(const std::vector<std::string>& running) override;
	virtual void writeDMRCallerInt(unsigned int slotNo, unsigned int id) override;
	virtual void writeNXDNCallerInt(unsigned int id) override;

	virtual void writeCommandInt(const std::string& command) override;

	virtual void clockInt(unsigned int ms) override;

private:
	std::string    m_callsign;
	unsigned int   m_id;
	bool           m_duplex;
	std::string    m_ipAddress;
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
	std::string    m_dateFormat;
	std::string    m_timeFormat;
	unsigned int   m_idleBrightness;
	unsigned int   m_screenLayout;
	CTimer         m_clockDisplayTimer;
	bool           m_displayTempInF;
	CRingBuffer<unsigned char> m_output;
	CMutex         m_mutex;
	unsigned char* m_reply;
	unsigned int   m_replyLength;
	bool           m_waiting;
	unsigned int   m_pending;
	bool           m_redrawIdle;
	CTimer         m_waitingTimer;

	// The ON7LDS DIY layout (3): what the screen asks for, and what it
	// shows beyond the other layouts
	typedef std::map<std::string, std::string> CFields;	// "t4.txt" -> its instruction
	unsigned int   m_page;					// the page last shown
	CFields        m_fields[NEXTION_PAGES];			// each page's fields as last sent
	std::deque<CFields> m_heard[NEXTION_PAGES];		// each page's last calls, newest first
	bool           m_inCall[NEXTION_PAGES];
	bool           m_held;					// the screen is on its own pages
	bool           m_asleep;
	unsigned int   m_sleepWhenIdle;
	CTimer         m_sleepTimer;
	CHostModes     m_modes;
	bool           m_modesKnown;
	std::vector<std::string> m_running;
	bool           m_programsKnown;
	std::string    m_groupsFile;
	std::map<unsigned int, std::string> m_groups;
	time_t         m_groupsTime;
	CTimer         m_groupsTimer;
	std::string    m_diskPath;
	std::string    m_model;
	std::string    m_serialNo;
	CIdLookup*     m_dmrIds;
	CIdLookup*     m_nxdnIds;
	CTimer         m_rtcTimer;
	CIdEntry       m_dmrCaller;				// looked up, to send with the call
	bool           m_dmrCallerSet;
	CIdEntry       m_nxdnCaller;
	bool           m_nxdnCallerSet;

	void sendCommand(const std::string& command);
	void queueCommand(const std::string& command);
	void track(const std::string& command);
	void callStatus(unsigned int status);
	void screenCommand(const unsigned char* data, unsigned int length);
	void sendScreenData(unsigned int page);
	void sendLastHeard(unsigned int page, unsigned int count);
	void sendModel();
	void sendModes(bool direct);
	bool isRunning(const char* name1, const char* name2 = nullptr) const;
	void sendTGName(unsigned int slotNo, bool group, unsigned int dst);
	void readGroups();
	void sendDiskFree();
	static std::string clean(const std::string& text);
	void sendRTC();
	void sendDMRCaller();
	void sendNXDNCaller();
	void wake();
	void idleTouched();
	void processReply(const unsigned char* data, unsigned int length);
	void sendCommandAction(unsigned int status);
	void sendCPU();
	void redrawIdle();
	void sendClock();
};

#endif
