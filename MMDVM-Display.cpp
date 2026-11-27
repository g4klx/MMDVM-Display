/*
 *   Copyright (C) 2015-2023,2025,2026 by Jonathan Naylor G4KLX
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

#include "MMDVM-Display.h"
#include "MQTTConnection.h"
#include "UARTController.h"
#include "TFTSurenoo.h"
// The FrameBuffer display drives a Linux framebuffer, so it is built on Linux
// only (the Windows project and other systems get the other displays)
#if !defined(__linux__) && !defined(NO_FRAMEBUFFER)
#define NO_FRAMEBUFFER
#endif
#if !defined(NO_FRAMEBUFFER)
#include "FrameBuffer.h"
#endif
#include "StopWatch.h"
#include "LCDproc.h"
#include "Nextion.h"
#include "Version.h"
#include "Defines.h"
#include "Dummy.h"
#include "Thread.h"
#include "Utils.h"
#include "Conf.h"
#include "Log.h"
#include "GitVersion.h"

#if defined(USE_HD44780)
#include "HD44780.h"
#endif

#if defined(USE_OLED)
#include "OLED.h"
#endif

#include <cstdio>
#include <vector>

#include <cstdlib>
#include <cerrno>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <pwd.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
const char* DEFAULT_INI_FILE = "MMDVM-Display.ini";
#else
const char* DEFAULT_INI_FILE = "/etc/MMDVM-Display.ini";
#endif

static bool m_killed = false;
static int  m_signal = 0;
static bool m_reload = false;

// In Log.cpp
extern CMQTTConnection* m_mqtt;

CMMDVMDisplay* driver = nullptr;

#if !defined(_WIN32) && !defined(_WIN64)
static void sigHandler1(int signum)
{
	m_killed = true;
	m_signal = signum;
}

static void sigHandler2(int signum)
{
	m_reload = true;
}
#endif

int main(int argc, char** argv)
{
	const char* iniFile = DEFAULT_INI_FILE;
	if (argc > 1) {
 		for (int currentArg = 1; currentArg < argc; ++currentArg) {
			std::string arg = argv[currentArg];
			if ((arg == "-v") || (arg == "--version")) {
				::fprintf(stdout, "MMDVM-Display version %s git #%.7s\n", VERSION, gitversion);
				return 0;
			} else if (arg.substr(0,1) == "-") {
				::fprintf(stderr, "Usage: MMDVM-Display [-v|--version] [filename]\n");
				return 1;
			} else {
				iniFile = argv[currentArg];
			}
		}
	}

#if !defined(_WIN32) && !defined(_WIN64)
	::signal(SIGINT,  sigHandler1);
	::signal(SIGTERM, sigHandler1);
	::signal(SIGHUP,  sigHandler1);
	::signal(SIGUSR1, sigHandler2);
#endif

	int ret = 0;

	do {
		m_signal = 0;

		driver = new CMMDVMDisplay(std::string(iniFile));
		ret = driver->run();
		delete driver;

		switch (m_signal) {
			case 2:
				::LogInfo("MMDVM-Display-%s exited on receipt of SIGINT", VERSION);
				break;
			case 15:
				::LogInfo("MMDVM-Display-%s exited on receipt of SIGTERM", VERSION);
				break;
			case 1:
				::LogInfo("MMDVM-Display-%s exited on receipt of SIGHUP", VERSION);
				break;
			case 10:
				::LogInfo("MMDVM-Display-%s is restarting on receipt of SIGUSR1", VERSION);
				break;
			default:
				::LogInfo("MMDVM-Display-%s exited on receipt of an unknown signal", VERSION);
				break;
		}
	} while (m_signal == 10);

	::LogFinalise();

	return ret;
}

CMMDVMDisplay::CMMDVMDisplay(const std::string& confFile) :
m_conf(confFile),
m_display(nullptr),
m_msp(nullptr),
m_hostConfName(),
m_mqttInfoName(),
m_temperatureInF(false),
m_modeSeen(false),
m_confTimer(1000U, 15U),
m_location("?"),
m_locationTries(0U),
m_hostComplete(false),
m_rxFrequency(0U),
m_txFrequency(0U),
m_addrTimer(1000U, 15U)
{
}

CMMDVMDisplay::~CMMDVMDisplay()
{
}

int CMMDVMDisplay::run()
{
	bool ret = m_conf.read();
	if (!ret) {
		::fprintf(stderr, "MMDVM-Display: cannot read the .ini file\n");
		return 1;
	}

#if !defined(_WIN32) && !defined(_WIN64)
	bool m_daemon = m_conf.getDaemon();
	if (m_daemon) {
		// Create new process
		pid_t pid = ::fork();
		if (pid == -1) {
			::fprintf(stderr, "Couldn't fork() , exiting\n");
			return -1;
		} else if (pid != 0) {
			exit(EXIT_SUCCESS);
		}

		// Create new session and process group
		if (::setsid() == -1){
			::fprintf(stderr, "Couldn't setsid(), exiting\n");
			return -1;
		}

		// Set the working directory to the root directory
		if (::chdir("/") == -1){
			::fprintf(stderr, "Couldn't cd /, exiting\n");
			return -1;
		}

#if !defined(USE_HD44780) && !defined(USE_OLED) && !defined(_OPENWRT)
		// If we are currently root...
		if (getuid() == 0) {
			struct passwd* user = ::getpwnam("mmdvm");
			if (user == nullptr) {
				::fprintf(stderr, "Could not get the mmdvm user, exiting\n");
				return -1;
			}

			uid_t mmdvm_uid = user->pw_uid;
			gid_t mmdvm_gid = user->pw_gid;

			// Set user and group ID's to mmdvm:mmdvm
			if (::setgid(mmdvm_gid) != 0) {
				::fprintf(stderr, "Could not set mmdvm GID, exiting\n");
				return -1;
			}

			if (::setuid(mmdvm_uid) != 0) {
				::fprintf(stderr, "Could not set mmdvm UID, exiting\n");
				return -1;
			}

			// Double check it worked (AKA Paranoia)
			if (::setuid(0) != -1){
				::fprintf(stderr, "It's possible to regain root - something is wrong!, exiting\n");
				return -1;
			}
		}
	}
#else
	::fprintf(stderr, "Dropping root permissions in daemon mode is disabled.\n");
	}
#endif
#endif
	::LogInitialise(m_conf.getLogDisplayLevel(), m_conf.getLogMQTTLevel());

	m_mqttInfoName   = m_conf.getMQTTInfoName();
	m_hostConfName   = m_conf.getHostConfName();
	m_temperatureInF = m_conf.getTemperatureInF();

	const std::string displayName = m_conf.getMQTTHostName() + "/display-out";
	const std::string hostName    = m_conf.getMQTTHostName() + "/json";
	const std::string infoName    = m_conf.getMQTTInfoName() + "/json";
	const std::string nextionName = m_conf.getMQTTName() + "/nextion-in";

	std::vector<std::pair<std::string, void (*)(const unsigned char*, unsigned int)>> subscriptions;
	subscriptions.push_back(std::make_pair(displayName, CMMDVMDisplay::onDisplay));
	subscriptions.push_back(std::make_pair(hostName,    CMMDVMDisplay::onHost));
	subscriptions.push_back(std::make_pair(infoName,    CMMDVMDisplay::onInfo));
	subscriptions.push_back(std::make_pair(nextionName, CMMDVMDisplay::onNextion));

	m_mqtt = new CMQTTConnection(m_conf.getMQTTAddress(), m_conf.getMQTTPort(), m_conf.getMQTTName(), m_conf.getMQTTAuthEnabled(), m_conf.getMQTTUsername(), m_conf.getMQTTPassword(), subscriptions, m_conf.getMQTTKeepalive());
	ret = m_mqtt->open();
	if (!ret) {
		::fprintf(stderr, "MMDVM-Display: unable to start the MQTT Publisher\n");
		delete m_mqtt;
		return 1;
	}

	// Pump the MQTT loop until connected (CONNACK received) so that
	// the display startup commands don't get silently dropped.
	for (unsigned int i = 0U; i < 50U && !m_mqtt->isConnected(); i++) {
		m_mqtt->loop();
		CThread::sleep(100U);
	}

#if !defined(_WIN32) && !defined(_WIN64)
	if (m_daemon) {
		::close(STDIN_FILENO);
		::close(STDOUT_FILENO);
	}
#endif
	LogInfo("MMDVM-Display-%s is starting", VERSION);
	LogInfo("Built %s %s (GitID #%.7s)", __TIME__, __DATE__, gitversion);

	writeJSONMessage("MMDVM-Display is starting");

	ret = createDisplay();
	if (!ret)
		return 1;

	CStopWatch stopWatch;
	stopWatch.start();

	m_confTimer.start();
	m_addrTimer.start();

	pollHostConfig();
	pollHostAddresses();

	while (!m_killed) {
		unsigned int ms = stopWatch.elapsed();
		stopWatch.start();

		m_display->clock(ms);

		// Drive MQTT I/O from the main loop (non-threaded) to
		// avoid the auto-reconnect race that causes client ID
		// collisions and a connect/disconnect loop.
		if (m_mqtt != nullptr)
			m_mqtt->loop();

		m_confTimer.clock(ms);
		if (m_confTimer.isRunning() && m_confTimer.hasExpired()) {
			pollHostConfig();
			m_confTimer.start();
		}

		m_addrTimer.clock(ms);
		if (m_addrTimer.isRunning() && m_addrTimer.hasExpired()) {
			pollHostAddresses();
			m_addrTimer.start();
		}

		if (ms < 10U)
			CThread::sleep(10U);
	}

	LogInfo("MMDVM-Display is stopping");
	writeJSONMessage("MMDVM-Display is stopping");

	m_display->close();
	delete m_display;

	return 0;
}

bool CMMDVMDisplay::createDisplay()
{
	std::string type = m_conf.getDisplay();

	LogInfo("Display Parameters");
	LogInfo("    Type: %s", type.c_str());

	if (type == "TFTSurenoo") {
		std::string port          = m_conf.getTFTSurenooPort();
		unsigned int brightness   = m_conf.getTFTSurenooBrightness();
		unsigned int screenLayout = m_conf.getTFTSurenooScreenLayout();

		LogInfo("    Port: %s", port.c_str());
		LogInfo("    Brightness: %u", brightness);
		LogInfo("    Screen Layout: %u", screenLayout);

		ISerialPort* serial = nullptr;
		if (port == "modem")
			serial = m_msp = new CModemSerialPort(m_conf.getMQTTHostName());
		else
			serial = new CUARTController(port, 115200U);

		m_display = new CTFTSurenoo(serial, brightness, screenLayout);
#if !defined(NO_FRAMEBUFFER)
	} else if (type == "FrameBuffer") {
		std::string device        = m_conf.getFrameBufferDevice();
		unsigned int rotation     = m_conf.getFrameBufferRotation();
		unsigned int brightness   = m_conf.getFrameBufferBrightness();
		unsigned int idleBright   = m_conf.getFrameBufferIdleBrightness();
		unsigned int idleTimeout  = m_conf.getFrameBufferIdleTimeout();
		std::string overlayDir    = m_conf.getFrameBufferOverlayDir();
		std::string overlay       = m_conf.getFrameBufferOverlay();
		std::string vt            = m_conf.getFrameBufferVT();

		// Resolve the overlay (layout) file: OverlayDir/Overlay, unless Overlay
		// is empty/"default" (built-in) or already an absolute path.
		std::string layoutPath = "default";
		if (!overlay.empty() && overlay != "default") {
			if (overlay[0] == '/' || overlayDir.empty())
				layoutPath = overlay;
			else
				layoutPath = overlayDir + "/" + overlay;
		}

		LogInfo("    Device: %s", device.c_str());
		LogInfo("    Rotation: %u", rotation);
		LogInfo("    Brightness: %u, idle %u after %us", brightness, idleBright, idleTimeout);
		LogInfo("    Overlay: %s", layoutPath.c_str());
		LogInfo("    VT: %s", vt.c_str());

		m_display = new CFrameBuffer(device, rotation, brightness, idleBright, idleTimeout, layoutPath, vt);
#endif
	} else if (type == "Nextion") {
		std::string port            = m_conf.getNextionPort();
		unsigned int speed          = m_conf.getNextionSpeed();
		unsigned int brightness     = m_conf.getNextionBrightness();
		bool displayClock           = m_conf.getNextionDisplayClock();
		bool utc                    = m_conf.getNextionUTC();
		unsigned int idleBrightness = m_conf.getNextionIdleBrightness();
		unsigned int screenLayout   = m_conf.getNextionScreenLayout();
		bool tempInF                = m_conf.getTemperatureInF();
		std::string dateFormat      = m_conf.getDateFormat();
		std::string timeFormat      = m_conf.getTimeFormat();
		std::string groupsFile      = m_conf.getNextionGroupsFile();
		unsigned int sleepWhenIdle  = m_conf.getNextionSleepWhenIdle();
		std::string dmrIdFile       = m_conf.getNextionDMRIdFile();
		std::string nxdnIdFile      = m_conf.getNextionNXDNIdFile();

		LogInfo("    Port: %s", port.c_str());
		LogInfo("    Speed: %u", speed);
		LogInfo("    Brightness: %u", brightness);
		LogInfo("    Clock Display: %s", displayClock ? "yes" : "no");
		if (displayClock) {
			LogInfo("    Display UTC: %s", utc ? "yes" : "no");
			LogInfo("    Date Format: %s", dateFormat.c_str());
			LogInfo("    Time Format: %s", timeFormat.c_str());
		}
		LogInfo("    Idle Brightness: %u", idleBrightness);
		LogInfo("    Temperature in Fahrenheit: %s ", tempInF ? "yes" : "no");
 
		switch (screenLayout) {
		case 0U:
			LogInfo("    Screen Layout: G4KLX (Default)");
			break;
		case 2U:
			LogInfo("    Screen Layout: ON7LDS");
			break;
		case 3U:
			LogInfo("    Screen Layout: DIY by ON7LDS");
			if (!groupsFile.empty())
				LogInfo("    Groups File: %s", groupsFile.c_str());
			LogInfo("    Sleep When Idle: %u s", sleepWhenIdle);
			if (!dmrIdFile.empty())
				LogInfo("    DMR Id File: %s", dmrIdFile.c_str());
			if (!nxdnIdFile.empty())
				LogInfo("    NXDN Id File: %s", nxdnIdFile.c_str());
			break;
		default:
			LogError("    Screen Layout: %u (Unknown)", screenLayout);
			return false;
		}

		ISerialPort* serial = nullptr;
		if (port == "modem")
			serial = m_msp = new CModemSerialPort(m_conf.getMQTTHostName());
		else
			serial = new CUARTController(port, speed);

		CNextion* nextion = new CNextion(serial, brightness, displayClock, utc, dateFormat, timeFormat, idleBrightness, screenLayout, tempInF);
		// The disk whose free space is shown: the one holding the
		// groups file, or else this program's config
		nextion->setDIY(groupsFile, sleepWhenIdle, groupsFile.empty() ? m_conf.getFile() : groupsFile, dmrIdFile, nxdnIdFile);
		m_display = nextion;
	} else if (type == "LCDproc") {
		std::string address       = m_conf.getLCDprocAddress();
		unsigned int port         = m_conf.getLCDprocPort();
		unsigned int localPort    = m_conf.getLCDprocLocalPort();
		bool displayClock         = m_conf.getLCDprocDisplayClock();
		bool utc                  = m_conf.getLCDprocUTC();
		bool dimOnIdle            = m_conf.getLCDprocDimOnIdle();
		std::string dateFormat    = m_conf.getDateFormat();
		std::string timeFormat    = m_conf.getTimeFormat();

		LogInfo("    Address: %s", address.c_str());
		LogInfo("    Port: %u", port);

		if (localPort == 0U)
			LogInfo("    Local Port: random");
		else
			LogInfo("    Local Port: %u", localPort);

		LogInfo("    Dim Display on Idle: %s", dimOnIdle ? "yes" : "no");
		LogInfo("    Clock Display: %s", displayClock ? "yes" : "no");

		if (displayClock) {
			LogInfo("    Display UTC: %s", utc ? "yes" : "no");
			LogInfo("    Date Format: %s", dateFormat.c_str());
			LogInfo("    Time Format: %s", timeFormat.c_str());
		}

		m_display = new CLCDproc(address, port, localPort, displayClock, utc, dateFormat, timeFormat, dimOnIdle);
#if defined(USE_HD44780)
	} else if (type == "HD44780") {
		unsigned int rows              = m_conf.getHD44780Rows();
		unsigned int columns           = m_conf.getHD44780Columns();
		std::vector<unsigned int> pins = m_conf.getHD44780Pins();
		unsigned int i2cAddress        = m_conf.getHD44780i2cAddress();
		bool pwm                       = m_conf.getHD44780PWM();
		unsigned int pwmPin            = m_conf.getHD44780PWMPin();
		unsigned int pwmBright         = m_conf.getHD44780PWMBright();
		unsigned int pwmDim            = m_conf.getHD44780PWMDim();
		bool displayClock              = m_conf.getHD44780DisplayClock();
		bool utc                       = m_conf.getHD44780UTC();
		std::string dateFormat         = m_conf.getDateFormat();
		std::string timeFormat         = m_conf.getTimeFormat();

		if (pins.size() == 6U) {
			LogInfo("    Rows: %u", rows);
			LogInfo("    Columns: %u", columns);

#if defined(USE_ADAFRUIT_DISPLAY) || defined(USE_PCF8574_DISPLAY)
			LogInfo("    Device Address: %#x", i2cAddress);
#else
			LogInfo("    Pins: %u,%u,%u,%u,%u,%u", pins.at(0U), pins.at(1U), pins.at(2U), pins.at(3U), pins.at(4U), pins.at(5U));
#endif

			LogInfo("    PWM Backlight: %s", pwm ? "yes" : "no");
			if (pwm) {
				LogInfo("    PWM Pin: %u", pwmPin);
				LogInfo("    PWM Bright: %u", pwmBright);
				LogInfo("    PWM Dim: %u", pwmDim);
			}

			LogInfo("    Clock Display: %s", displayClock ? "yes" : "no");
			if (displayClock) {
				LogInfo("    Display UTC: %s", utc ? "yes" : "no");
				LogInfo("    Date Format: %s", dateFormat.c_str());
				LogInfo("    Time Format: %s", timeFormat.c_str());
			}

			m_display = new CHD44780(rows, columns, pins, i2cAddress, pwm, pwmPin, pwmBright, pwmDim, displayClock, utc, dateFormat, timeFormat);
		}
#endif
#if defined(USE_OLED)
	} else if (type == "OLED") {
		unsigned char type       = m_conf.getOLEDType();
		unsigned char brightness = m_conf.getOLEDBrightness();
		bool          invert     = m_conf.getOLEDInvert();
		bool          scroll     = m_conf.getOLEDScroll();
		bool          rotate     = m_conf.getOLEDRotate();
		bool          logosaver  = m_conf.getOLEDLogoScreensaver();

		m_display = new COLED(type, brightness, invert, scroll, rotate, logosaver);
#endif
	} else if (type == "Dummy") {
		m_display = new CDummy;
	} else {
		LogError("No valid display found");
		return false;
	}

	bool ret = m_display->open();
	if (!ret) {
		delete m_display;
		m_display = nullptr;
		// A missing/undetected framebuffer is not a fatal error: gate the
		// FrameBuffer display and keep the service running (a dummy display), so
		// a supervisor does not restart-loop over a box with no screen attached.
		if (type == "FrameBuffer") {
			LogWarning("No screen detected; the FrameBuffer display is disabled, the service will keep running");
			m_display = new CDummy;
			if (!m_display->open()) {
				delete m_display;
				m_display = nullptr;
				return false;
			}
			return true;
		}
		return false;
	}

	return true;
}

void CMMDVMDisplay::writeJSONMessage(const std::string& message)
{
	nlohmann::json json;

	json["timestamp"] = CUtils::createTimestamp();
	json["message"]   = message;

	WriteJSON("status", json);
}

void CMMDVMDisplay::readDisplay(const unsigned char* data, unsigned int length)
{
	assert(data != nullptr);
	assert(length > 0U);

	if (m_msp != nullptr)
		m_msp->readData(data, length);
}

// A Nextion instruction from another program, e.g. an answer to a command
// from the screen, without its FF FF FF.
void CMMDVMDisplay::readNextion(const unsigned char* data, unsigned int length)
{
	assert(data != nullptr);
	assert(length > 0U);

	if (m_display != nullptr)
		m_display->writeCommand(std::string((char*)data, length));
}

void CMMDVMDisplay::readHost(const std::string& text)
{
	LogDebug("Incoming Host JSON - \"%s\"", text.c_str());

	try {
		nlohmann::json j = nlohmann::json::parse(text);

		if (j.contains("MMDVM") && j["MMDVM"].is_object()) {
			LogDebug("Identified as an MMDVM message");
			parseMMDVM(j["MMDVM"]);
			return;
		}

		if (j.contains("RSSI") && j["RSSI"].is_object()) {
			LogDebug("Identified as an RSSI message");
			parseRSSI(j["RSSI"]);
			return;
		}

		if (j.contains("BER") && j["BER"].is_object()) {
			LogDebug("Identified as a BER message");
			parseBER(j["BER"]);
			return;
		}

		if (j.contains("Text") && j["Text"].is_object()) {
			LogDebug("Identified as a Text message");
			parseText(j["Text"]);
			return;
		}

		if (j.contains("D-Star") && j["D-Star"].is_object()) {
			LogDebug("Identified as a D-Star message");
			parseDStar(j["D-Star"]);
			return;
		}

		if (j.contains("DMR") && j["DMR"].is_object()) {
			LogDebug("Identified as a DMR message");
			parseDMR(j["DMR"]);
			return;
		}

		if (j.contains("YSF") && j["YSF"].is_object()) {
			LogDebug("Identified as a YSF message");
			parseYSF(j["YSF"]);
			return;
		}

		if (j.contains("P25") && j["P25"].is_object()) {
			LogDebug("Identified as a P25 message");
			parseP25(j["P25"]);
			return;
		}

		if (j.contains("NXDN") && j["NXDN"].is_object()) {
			LogDebug("Identified as an NXDN message");
			parseNXDN(j["NXDN"]);
			return;
		}

		if (j.contains("POCSAG") && j["POCSAG"].is_object()) {
			LogDebug("Identified as a POCSAG message");
			parsePOCSAG(j["POCSAG"]);
			return;
		}

		if (j.contains("FM") && j["FM"].is_object()) {
			LogDebug("Identified as an FM message");
			parseFM(j["FM"]);
			return;
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing: \"%s\" - %s", text.c_str(), ex.what());
	}
}

void CMMDVMDisplay::readInfo(const std::string& text)
{
	LogDebug("Incoming Info JSON - \"%s\"", text.c_str());

	try {
		nlohmann::json j = nlohmann::json::parse(text);

		if (j.contains("CPU") && j["CPU"].is_object()) {
			LogDebug("Identified as a CPU message");
			parseCPU(j["CPU"]);
			return;
		}

		// No program running comes as null
		if (j.contains("Programs") && (j["Programs"].is_array() || j["Programs"].is_null())) {
			LogDebug("Identified as a Programs message");
			parsePrograms(j["Programs"]);
			return;
		}

		if (j.contains("Addresses") && j["Addresses"].is_array()) {
			LogDebug("Identified as an Addresses message");
			parseAddresses(j["Addresses"]);
			return;
		}

		if (j.contains("DMRGateway") && j["DMRGateway"].is_object()) {
			LogDebug("Identified as a DMRGateway Config message");
			parseGatewayConfig(j["DMRGateway"]);
			return;
		}

		if (j.contains(m_hostConfName) && j[m_hostConfName].is_object()) {
			LogDebug("Identified as an MMDVM-Host Config message");
			parseHostConfig(j[m_hostConfName]);
			return;
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing: \"%s\" - %s", text.c_str(), ex.what());
	}
}

void CMMDVMDisplay::parseMMDVM(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		// We're only interested in the mode messages
		bool hasMode = json.contains("mode");
		if (hasMode) {
			std::string mode = json["mode"];
			m_modeSeen = true;

			if (mode == "idle")
				m_display->setIdle();
			else if (mode == "CW")
				m_display->writeCW();
			else if (mode == "lockout") 
				m_display->setLockout();
			else if (mode == "error")
				m_display->setError();
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing MMDVM - %s", ex.what());
	}
}

void CMMDVMDisplay::parseRSSI(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		if (!json.contains("mode") || !json.contains("value"))
			return;

		std::string mode = json["mode"];
		int value        = json["value"];

		if (mode == "D-Star") {
			m_display->writeDStarRSSI(value);
		} else if (mode == "DMR") {
			if (!json.contains("slot"))
				return;
			int slot = json["slot"];
			m_display->writeDMRRSSI(slot, value);
		} else if (mode == "YSF") {
			m_display->writeFusionRSSI(value);
		} else if (mode == "P25") {
			m_display->writeP25RSSI(value);
		} else if (mode == "NXDN") {
			m_display->writeNXDNRSSI(value);
		} else if (mode == "FM") {
			m_display->writeFMRSSI(value);
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing RSSI - %s", ex.what());
	}
}

void CMMDVMDisplay::parseBER(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		if (!json.contains("mode") || !json.contains("value"))
			return;

		std::string mode = json["mode"];
		float value      = json["value"];

		if (mode == "D-Star") {
			m_display->writeDStarBER(value);
		} else if (mode == "DMR") {
			if (!json.contains("slot"))
				return;
			int slot = json["slot"];
			m_display->writeDMRBER(slot, value);
		} else if (mode == "YSF") {
			m_display->writeFusionBER(value);
		} else if (mode == "P25") {
			m_display->writeP25BER(value);
		} else if (mode == "NXDN") {
			m_display->writeNXDNBER(value);
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing BER - %s", ex.what());
	}
}

void CMMDVMDisplay::parseText(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		if (!json.contains("mode") || !json.contains("value"))
			return;

		std::string mode = json["mode"];
		if (mode == "D-Star") {
			std::string value = json["value"];
			m_display->writeDStarText(value);
		} else if (mode == "DMR") {
			if (!json.contains("slot"))
				return;
			int slot          = json["slot"];
			std::string value = json["value"];
			m_display->writeDMRTA(slot, value);
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing Text - %s", ex.what());
	}
}

void CMMDVMDisplay::parseDStar(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		if (!json.contains("action"))
			return;

		std::string action = json["action"];
		if (action == "start" || action == "late_entry") {
			if (!json.contains("src_callsign") || !json.contains("src_ext") ||
			    !json.contains("dst_callsign") || !json.contains("source"))
				return;

			std::string src_callsign = json["src_callsign"];
			std::string src_ext      = json["src_ext"];
			std::string dst_callsign = json["dst_callsign"];
			std::string reflector    = json.value("reflector", std::string());
			std::string source       = json["source"] == "rf" ? "R" : "N";

			m_display->writeDStar(src_callsign, src_ext, dst_callsign, source, reflector);
		} else if (action == "end" || action == "lost") {
			m_display->clearDStar();
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing D-Star - %s", ex.what());
	}
}

static bool getUnsigned(const nlohmann::json& j, const char* key, unsigned int& value);

void CMMDVMDisplay::parseDMR(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		if (!json.contains("action"))
			return;

		std::string action = json["action"];
		if (action == "start" || action == "late_entry") {
			if (!json.contains("slot")  || !json.contains("dst_id") ||
			    !json.contains("group") || !json.contains("source"))
				return;

			int slot             = json["slot"];
			std::string src_info = json.value("src_info", std::string());
			int dst_id           = json["dst_id"];
			std::string group    = json["group"];
			std::string source   = json["source"] == "rf" ? "R" : "N";

			// The caller's details first, for a display that shows them
			// with the call (0: not known)
			unsigned int src_id = 0U;
			getUnsigned(json, "src_id", src_id);
			m_display->writeDMRCaller(slot, src_id);

			m_display->writeDMR(slot, src_info, group == "yes", dst_id, source);
		} else if (action == "end" || action == "lost") {
			if (!json.contains("slot"))
				return;
			int slot = json["slot"];
			m_display->clearDMR(slot);
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing DMR - %s", ex.what());
	}
}

void CMMDVMDisplay::parseYSF(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		if (!json.contains("action"))
			return;

		std::string action = json["action"];
		if (action == "start" || action == "late_entry") {
			if (!json.contains("src_callsign") || !json.contains("dg-id") ||
			    !json.contains("source"))
				return;

			std::string src_callsign = json["src_callsign"];
			int dgId                 = json["dg-id"];
			std::string reflector    = json.value("reflector", std::string());
			std::string source       = json["source"] == "rf" ? "R" : "N";

			m_display->writeFusion(src_callsign, "ALL", dgId, source, reflector);
		} else if (action == "end" || action == "lost") {
			m_display->clearFusion();
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing YSF - %s", ex.what());
	}
}

void CMMDVMDisplay::parseP25(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		if (!json.contains("action"))
			return;

		std::string action = json["action"];
		if (action == "start" || action == "late_entry") {
			if (!json.contains("dst_id") || !json.contains("group") ||
			    !json.contains("source"))
				return;

			std::string src_info = json.value("src_info", std::string());
			int dst_id           = json["dst_id"];
			std::string group    = json["group"];
			std::string source   = json["source"] == "rf" ? "R" : "N";

			m_display->writeP25(src_info, group == "yes", dst_id, source);
		} else if (action == "end" || action == "lost") {
			m_display->clearP25();
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing P25 - %s", ex.what());
	}
}

void CMMDVMDisplay::parseNXDN(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		if (!json.contains("action"))
			return;

		std::string action = json["action"];
		if (action == "start" || action == "late_entry") {
			if (!json.contains("dst_id") || !json.contains("group") ||
			    !json.contains("source"))
				return;

			std::string src_info = json.value("src_info", std::string());
			int dst_id           = json["dst_id"];
			std::string group    = json["group"];
			std::string source   = json["source"] == "rf" ? "R" : "N";

			unsigned int src_id = 0U;
			getUnsigned(json, "src_id", src_id);
			m_display->writeNXDNCaller(src_id);

			m_display->writeNXDN(src_info, group == "yes", dst_id, source);
		} else if (action == "end" || action == "lost") {
			m_display->clearNXDN();
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing NXDN - %s", ex.what());
	}
}

void CMMDVMDisplay::parsePOCSAG(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		if (!json.contains("functional"))
			return;

		std::string functional = json["functional"];
		if (functional == "end") {
			m_display->clearPOCSAG();
		} else {
			if (!json.contains("ric"))
				return;
			int ric             = json["ric"];
			std::string message = json.value("message", std::string());
			m_display->writePOCSAG(ric, message);
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing POCSAG - %s", ex.what());
	}
}

void CMMDVMDisplay::parseFM(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		if (!json.contains("state"))
			return;

		std::string state = json["state"];

		if (state == "listening")
			m_display->clearFM();
		else if (state == "kerchunk_rf")
			m_display->writeFM("R: Kerchunk");
		else if (state == "relaying_rf")
			m_display->writeFM("R: Relaying");
		else if (state == "relaying_wait_rf" || state == "timeout_wait_rf" || state == "hang")
			m_display->writeFM("R: Wait");
		else if (state == "timeout_rf")
			m_display->writeFM("R: Timeout");
		else if (state == "kerchunk_ext")
			m_display->writeFM("N: Kerchunk");
		else if (state == "relaying_ext")
			m_display->writeFM("N: Relaying");
		else if (state == "relaying_wait_ext" || state == "timeout_wait_ext")
			m_display->writeFM("N: Wait");
		else if (state == "timeout_ext")
			m_display->writeFM("N: Timeout");
		else
			m_display->writeFM(state);
	}
	catch (std::exception& ex) {
		LogError("Error parsing FM - %s", ex.what());
	}
}

void CMMDVMDisplay::parseCPU(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		float temperature = -1.0F;
		float frequency   = -1.0F;
		float load        = -1.0F;

		if (json.contains("temperature"))
			temperature = json["temperature"];

		if (json.contains("frequency"))
			frequency = json["frequency"];

		if (json.contains("load"))
			load = json["load"];

		m_display->writeCPU(temperature, frequency, load);
	}
	catch (std::exception& ex) {
		LogError("Error parsing CPU - %s", ex.what());
	}
}

void CMMDVMDisplay::parsePrograms(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	std::vector<std::string> running;

	try {
		if (json.is_array()) {
			for (const auto& it : json) {
				if (it.is_object() && it.contains("name") && it["name"].is_string())
					running.push_back(it["name"].get<std::string>());
			}
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing Programs - %s", ex.what());
		return;
	}

	m_display->writePrograms(running);
}

void CMMDVMDisplay::parseAddresses(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		// We only take the first external interface
		for (const auto& it : json.items()) {
			const nlohmann::json obj = it.value();
			if (!obj.is_object())
				continue;

			// value(), not [] on a const object: a missing key would be
			// undefined behaviour
			std::string name = obj.value("name", std::string());
			std::string ipV4 = obj.value("ipv4", std::string());
			std::string ipV6 = obj.value("ipv6", std::string());

			m_display->writeIP(name, ipV4, ipV6);

			// Restarted while MMDVM-Host keeps running, the display hears
			// no mode until the next call, so the OLED would show
			// "Initializing" (and an HD44780 nothing) until then. The first
			// answer about the addresses means the system is up: show idle.
			if (!m_modeSeen) {
				m_modeSeen = true;
				m_display->setIdle();
			}

			return;
		}
	}
	catch (std::exception& ex) {
		LogError("Error parsing Addresses - %s", ex.what());
	}
}

// Read an unsigned number from a Host Config field, which may be a string or
// a number. A missing, blank or non-numeric field (an unconfigured host) gives
// false and leaves value alone.
static bool getUnsigned(const nlohmann::json& j, const char* key, unsigned int& value)
{
	if (!j.contains(key))
		return false;

	const nlohmann::json& field = j[key];

	if (field.is_number_unsigned()) {
		value = field.get<unsigned int>();
		return true;
	}

	if (!field.is_string())
		return false;

	const std::string text = field.get<std::string>();
	if (text.empty())
		return false;

	char* end = nullptr;
	errno = 0;
	unsigned long n = ::strtoul(text.c_str(), &end, 10);
	if ((errno != 0) || (*end != '\0') || (text[0] == '-') || (n > 0xFFFFFFFFUL))
		return false;

	value = (unsigned int)n;
	return true;
}

static bool isEnabled(const nlohmann::json& json, const char* section)
{
	assert(section != nullptr);

	if (!json.contains(section) || !json[section].is_object())
		return false;

	unsigned int enable = 0U;
	return getUnsigned(json[section], "Enable", enable) && (enable == 1U);
}

void CMMDVMDisplay::parseHostConfig(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	// Until the host has an Id and its frequencies, keep polling, so that the
	// display picks them up once the host is configured.
	bool complete = true;

	try {
		if (json.contains("General") && json["General"].is_object()) {
			std::string callsign;
			unsigned int id = 0U;
			unsigned int duplex = 0U;

			const nlohmann::json j = json["General"];

			if (j.contains("Callsign") && j["Callsign"].is_string())
				callsign = j["Callsign"].get<std::string>();

			if (!getUnsigned(j, "Id", id) || (id == 0U)) {
				LogDebug("The host has no Id yet");
				complete = false;
			}

			getUnsigned(j, "Duplex", duplex);

			m_display->writeGeneral(callsign, id, duplex == 1U);
		}

		if (json.contains("Modem") && json["Modem"].is_object()) {
			unsigned int rxFrequency = 0U;
			unsigned int txFrequency = 0U;

			const nlohmann::json j = json["Modem"];

			// Frequencies are in Hz
			if (!getUnsigned(j, "RXFrequency", rxFrequency) || (rxFrequency == 0U) ||
			    !getUnsigned(j, "TXFrequency", txFrequency) || (txFrequency == 0U)) {
				LogDebug("The host has no frequencies yet");
				complete = false;
			}

			m_rxFrequency = rxFrequency;
			m_txFrequency = txFrequency;
			m_display->writeInfo(rxFrequency, txFrequency, m_location);
		}

		// The modes and networks enabled
		CHostModes modes;
		modes.dstar    = isEnabled(json, "D-Star");
		modes.dmr      = isEnabled(json, "DMR");
		modes.ysf      = isEnabled(json, "System Fusion");
		modes.p25      = isEnabled(json, "P25");
		modes.nxdn     = isEnabled(json, "NXDN");
		modes.dstarNet = isEnabled(json, "D-Star Network");
		modes.dmrNet   = isEnabled(json, "DMR Network");
		modes.ysfNet   = isEnabled(json, "System Fusion Network");
		modes.p25Net   = isEnabled(json, "P25 Network");
		modes.nxdnNet  = isEnabled(json, "NXDN Network");
		m_display->writeModes(modes);
	}
	catch (std::exception& ex) {
		LogError("Error parsing Host Config - %s", ex.what());
		return;
	}

	m_hostComplete = complete;
	stopConfigPolling();
}

void CMMDVMDisplay::parseGatewayConfig(const nlohmann::json& json)
{
	assert(m_display != nullptr);

	try {
		if (!json.contains("Info") || !json["Info"].is_object())
			return;

		const nlohmann::json j = json["Info"];
		if (!j.contains("Location") || !j["Location"].is_string())
			return;

		std::string location = j["Location"].get<std::string>();
		if (location.empty() || (location == m_location))
			return;

		m_location = location;
		m_display->writeInfo(m_rxFrequency, m_txFrequency, m_location);
	}
	catch (std::exception& ex) {
		LogError("Error parsing DMRGateway Config - %s", ex.what());
		return;
	}

	stopConfigPolling();
}

// Polling stops once the host's config is complete and the location is
// known, or not to be had (no DMRGateway)
void CMMDVMDisplay::stopConfigPolling()
{
	if (m_hostComplete && ((m_location != "?") || (m_locationTries >= 8U)))
		m_confTimer.stop();
}

void CMMDVMDisplay::pollHostConfig()
{
	std::string   topic = m_mqttInfoName + "/command";
	std::string command = "Config " + m_hostConfName;

	m_mqtt->publish(topic, command);

	if ((m_location == "?") && (m_locationTries < 8U)) {
		m_mqtt->publish(topic, "Config DMRGateway");
		m_locationTries++;
	}

	stopConfigPolling();
}

void CMMDVMDisplay::pollHostAddresses()
{
	std::string   topic = m_mqttInfoName + "/command";
	std::string command = "Addresses";

	m_mqtt->publish(topic, command);
}

void CMMDVMDisplay::onDisplay(const unsigned char* data, unsigned int length)
{
	// An empty message (mosquitto passes no data for one) or one before
	// the driver exists is ignored: it is not ours to abort on
	if ((data == nullptr) || (length == 0U) || (driver == nullptr))
		return;

	driver->readDisplay(data, length);
}

void CMMDVMDisplay::onHost(const unsigned char* data, unsigned int length)
{
	// An empty message (mosquitto passes no data for one) or one before
	// the driver exists is ignored: it is not ours to abort on
	if ((data == nullptr) || (length == 0U) || (driver == nullptr))
		return;

	driver->readHost(std::string((char*)data, length));
}

void CMMDVMDisplay::onInfo(const unsigned char* data, unsigned int length)
{
	// An empty message (mosquitto passes no data for one) or one before
	// the driver exists is ignored: it is not ours to abort on
	if ((data == nullptr) || (length == 0U) || (driver == nullptr))
		return;

	driver->readInfo(std::string((char*)data, length));
}

void CMMDVMDisplay::onNextion(const unsigned char* data, unsigned int length)
{
	// An empty message (mosquitto passes no data for one) or one before
	// the driver exists is ignored: it is not ours to abort on
	if ((data == nullptr) || (length == 0U) || (driver == nullptr))
		return;

	driver->readNextion(data, length);
}
