/*
 *   Copyright (C) 2026 by Andy Taylor MW0MWZ
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
 */

#include "DisplayScreen.h"
#include "Log.h"

#include <cstring>
#include <cmath>
#include <new>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/vt.h>
#include <linux/kd.h>
#include <csignal>
#include <cstdlib>
#include <cstdio>

// VT switch handling. Under VT_SETMODE/VT_PROCESS the kernel signals us when
// another VT takes over (relsig) and when ours comes back (acqsig); the
// handlers acknowledge the switch and flip g_vtActive, which blitToScreen uses
// to decide whether to touch the framebuffer. One screen per process, so a
// file-scope state is enough, and the handlers stay async-signal small.
namespace {
	volatile sig_atomic_t g_vtActive     = 1;
	volatile sig_atomic_t g_vtReacquired = 0;
	int                   g_vtFd         = -1;

	void vtRelease(int) {            // another VT is taking the screen
		if (g_vtFd >= 0)
			(void)::ioctl(g_vtFd, VT_RELDISP, 1);
		g_vtActive = 0;
	}
	void vtAcquire(int) {            // our VT is active again
		if (g_vtFd >= 0)
			(void)::ioctl(g_vtFd, VT_RELDISP, VT_ACKACQ);
		g_vtActive     = 1;
		g_vtReacquired = 1;
	}
}

// Read the picture's size once (helper for the two statics below): the
// framebuffer's, with width and height swapped when it is turned 90 or 270.
static bool readFbGeom(const std::string& device, unsigned int rotation, unsigned int& w, unsigned int& h)
{
	int fd = ::open(device.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	struct fb_var_screeninfo v;
	bool ok = (::ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0 && v.xres > 0 && v.yres > 0);
	if (ok) { w = v.xres; h = v.yres; }
	::close(fd);
	rotation = CDisplayScreen::validRotation(rotation);
	if (ok && (rotation == 90U || rotation == 270U)) { unsigned int t = w; w = h; h = t; }
	return ok;
}

// The design HEIGHT is a fixed 240 canonical units; the design WIDTH takes the
// screen's aspect ratio (clamped to a sane landscape range), so the layout
// fills the panel natively instead of stretching. A screen narrower than the
// clamp (portrait) is letterboxed: see canvasScale.
double CDisplayScreen::designWidth(const std::string& device, unsigned int rotation)
{
	unsigned int w = 0, h = 0;
	if (!readFbGeom(device, rotation, w, h) || h == 0U)
		return 320.0;                      // 4:3 fallback
	double aspect = (double)w / (double)h;
	if (aspect < 1.20) aspect = 1.20;      // near-square: mild side margins
	if (aspect > 2.40) aspect = 2.40;      // ultra-wide: cap the width
	return 240.0 * aspect;                 // kept fractional for an exact fit
}

// Scale so the layout fills the screen, rendered 1:1 at the panel's native
// resolution: the 240-unit design height fills the screen height, or, on a
// screen narrower than the narrowest layout (portrait), the layout's width
// fills the screen width and it is centred top to bottom (letterboxed), so
// none of it is cut off. Never below half the design size.
double CDisplayScreen::canvasScale(const std::string& device, unsigned int rotation)
{
	unsigned int w = 0, h = 0;
	if (!readFbGeom(device, rotation, w, h) || h == 0U)
		return 1.0;
	double scale = (double)h / 240.0;         // fill the height
	if ((double)w < 240.0 * 1.20 * scale)     // too narrow for the layout:
		scale = (double)w / (240.0 * 1.20); // fill the width instead
	if (scale < 0.5) scale = 0.5;
	return scale;
}

CDisplayScreen::CDisplayScreen(const std::string& device, unsigned int rotation, unsigned int brightness,
			   unsigned int idleBrightness, unsigned int idleTimeout, const std::string& layoutPath,
			   const std::string& vt) :
CScreenCanvas(designWidth(device, rotation), 240.0, canvasScale(device, rotation), brightness, idleBrightness, idleTimeout, layoutPath),
m_device(device),
m_rotation(validRotation(rotation)),
m_fd(-1),
m_map(nullptr),
m_mapLen(0U),
m_fbW(0U), m_fbH(0U), m_viewW(0U), m_viewH(0U), m_bpp(0U), m_stride(0U),
m_rOff(11U), m_rLen(5U), m_gOff(5U), m_gLen(6U), m_bOff(0U), m_bLen(5U),
m_offX(0), m_offY(0),
m_fastRGB565(false),
m_refreshHz(0.0),
m_curBright(brightness),
m_vtConfig(vt),
m_useVT(false),
m_consoleFd(-1),
m_vtFd(-1),
m_vtNum(-1),
m_origVT(-1),
m_origKDMode(KD_TEXT)
{
	::memset(&m_vinfo, 0, sizeof(m_vinfo));
	if (m_rotation != rotation)
		LogWarning("Screen: Rotation=%u is not 0, 90, 180 or 270; using 0", rotation);
}

CDisplayScreen::~CDisplayScreen()
{
}

bool CDisplayScreen::openOutput()
{
	m_fd = ::open(m_device.c_str(), O_RDWR | O_CLOEXEC);
	if (m_fd < 0) {
		LogWarning("Screen: no framebuffer at %s (is a screen attached?)", m_device.c_str());
		return false;
	}

	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	if (::ioctl(m_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
	    ::ioctl(m_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
		LogError("Screen: cannot query %s", m_device.c_str());
		::close(m_fd); m_fd = -1;
		return false;
	}

	m_vinfo  = vinfo;
	m_fbW    = vinfo.xres;
	m_fbH    = vinfo.yres;
	m_bpp    = vinfo.bits_per_pixel;
	m_stride = finfo.line_length;
	m_rOff = vinfo.red.offset;    m_rLen = vinfo.red.length;
	m_gOff = vinfo.green.offset;  m_gLen = vinfo.green.length;
	m_bOff = vinfo.blue.offset;   m_bLen = vinfo.blue.length;

	// Validate the geometry and pixel format so the packer can never shift by
	// a bad amount or write past a row (a misbehaving or exotic mode).
	const unsigned int bytes = m_bpp / 8U;
	if ((m_bpp != 16U && m_bpp != 32U) || vinfo.grayscale != 0U ||
	    m_fbW == 0U || m_fbH == 0U || m_fbW > 8192U || m_fbH > 8192U ||
	    m_rLen < 1U || m_rLen > 8U || m_gLen < 1U || m_gLen > 8U || m_bLen < 1U || m_bLen > 8U ||
	    m_rOff + m_rLen > m_bpp || m_gOff + m_gLen > m_bpp || m_bOff + m_bLen > m_bpp ||
	    m_stride < (size_t)m_fbW * bytes) {
		LogError("Screen: unsupported framebuffer format on %s (%ux%u %ubpp, stride %u)",
			 m_device.c_str(), m_fbW, m_fbH, m_bpp, m_stride);
		::close(m_fd); m_fd = -1;
		return false;
	}

	// Detect the refresh rate from the mode timings. pixclock is picoseconds
	// per pixel; refresh = 1 / (pixclock * htotal * vtotal). It is 0 on DRM
	// fbdev emulation, which does not fill the timing fields.
	m_refreshHz = 0.0;
	if (vinfo.pixclock > 0U) {
		double htot = (double)vinfo.xres + vinfo.left_margin + vinfo.right_margin + vinfo.hsync_len;
		double vtot = (double)vinfo.yres + vinfo.upper_margin + vinfo.lower_margin + vinfo.vsync_len;
		if (htot > 0.0 && vtot > 0.0)
			m_refreshHz = 1e12 / ((double)vinfo.pixclock * htot * vtot);
	}
	if (m_refreshHz > 0.0)
		LogMessage("Screen: detected %ux%u @ %.1fHz, %ubpp", m_fbW, m_fbH, m_refreshHz, m_bpp);
	else
		LogMessage("Screen: detected %ux%u (refresh unknown), %ubpp", m_fbW, m_fbH, m_bpp);

	// Fast path: plain little-endian RGB565 lets us memcpy canvas rows.
	m_fastRGB565 = (m_bpp == 16U && m_rOff == 11U && m_rLen == 5U &&
			m_gOff == 5U && m_gLen == 6U && m_bOff == 0U && m_bLen == 5U);

	// The picture: the screen, or the screen on its side at 90 and 270.
	bool sideways = (m_rotation == 90U || m_rotation == 270U);
	m_viewW = sideways ? m_fbH : m_fbW;
	m_viewH = sideways ? m_fbW : m_fbH;

	// The canvas is rendered at the panel's native resolution, so it maps 1:1
	// onto the picture. For a normal aspect it equals the picture and the
	// offsets are zero (fills edge to edge); only a clamped extreme aspect
	// leaves a centred margin, the border cleared black once below.
	m_offX = ((int)m_viewW - (int)m_width) / 2;
	m_offY = ((int)m_viewH - (int)m_height) / 2;
	if (m_offX < 0) m_offX = 0;
	if (m_offY < 0) m_offY = 0;

	m_mapLen = (size_t)m_stride * m_fbH;

	void* m = ::mmap(nullptr, m_mapLen, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
	if (m == MAP_FAILED) {
		LogError("Screen: cannot mmap %s", m_device.c_str());
		::close(m_fd); m_fd = -1;
		return false;
	}
	m_map = (uint8_t*)m;

	// Take over a VT (grab-at-startup, like X) so the status screen does not
	// draw over the text console. Falls back to drawing directly when there is
	// no console, or when VT=off.
	setupVT();

	::memset(m_map, 0, m_mapLen);

	LogMessage("Screen: %ux%u %ubpp on %s, rotation %u (render %ux%u at +%d,+%d%s)", m_fbW, m_fbH, m_bpp,
		   m_device.c_str(), m_rotation, m_width, m_height, m_offX, m_offY,
		   (m_fastRGB565 && m_rotation == 0U) ? ", fast" : "");
	return true;
}

void CDisplayScreen::closeOutput()
{
	teardownVT();                 // restore the console before releasing the fb
	if (m_map != nullptr) {
		::munmap(m_map, m_mapLen);
		m_map = nullptr;
	}
	if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
}

void CDisplayScreen::applyBrightness(unsigned int pct)
{
	if (pct > 100U) pct = 100U;
	m_curBright = pct;
	if (m_map != nullptr)
		blitToScreen();
}

void CDisplayScreen::present()
{
	if (m_map == nullptr)
		return;
	blitToScreen();
}

void CDisplayScreen::blitToScreen()
{
	// Another VT has the screen (the user switched to the console): leave the
	// framebuffer alone until ours is active again.
	if (m_useVT && !g_vtActive)
		return;

	const unsigned int bytes  = m_bpp / 8U;
	const unsigned int bright = m_curBright;

	// Pack 8-bit r,g,b into the framebuffer's native format (with idle dim).
	auto put = [&](uint8_t* dpx, unsigned int r, unsigned int g, unsigned int b) {
		if (bright < 100U) { r = r * bright / 100U; g = g * bright / 100U; b = b * bright / 100U; }
		uint32_t px = (((uint32_t)(r >> (8U - m_rLen))) << m_rOff) |
			      (((uint32_t)(g >> (8U - m_gLen))) << m_gOff) |
			      (((uint32_t)(b >> (8U - m_bLen))) << m_bOff);
		dpx[0] = (uint8_t)(px & 0xFFU);
		dpx[1] = (uint8_t)((px >> 8) & 0xFFU);
		if (bytes == 4U) { dpx[2] = (uint8_t)((px >> 16) & 0xFFU); dpx[3] = (uint8_t)((px >> 24) & 0xFFU); }
	};
	// Visible canvas columns/rows (equal to the canvas for a normal aspect; a
	// clamped extreme aspect is cropped to the picture).
	unsigned int vis  = ((unsigned int)m_offX + m_width  > m_viewW) ? m_viewW - (unsigned int)m_offX : m_width;
	unsigned int rows = ((unsigned int)m_offY + m_height > m_viewH) ? m_viewH - (unsigned int)m_offY : m_height;

	// Turned: each pixel to its place on the panel, then flush it all.
	if (m_rotation != 0U) {
		const bool direct = m_fastRGB565 && bright >= 100U;
		for (unsigned int cy = 0U; cy < rows; cy++) {
			const uint16_t* srow = &m_fb[(size_t)cy * m_width];
			for (unsigned int cx = 0U; cx < vis; cx++) {
				unsigned int px, py;
				toPanel(m_rotation, m_fbW, m_fbH, (unsigned int)m_offX + cx, (unsigned int)m_offY + cy, px, py);
				uint8_t* dpx = m_map + (size_t)py * m_stride + (size_t)px * bytes;
				if (direct) {
					dpx[0] = (uint8_t)(srow[cx] & 0xFFU);
					dpx[1] = (uint8_t)(srow[cx] >> 8);
					continue;
				}
				unsigned int r5 = (srow[cx] >> 11) & 0x1FU, g6 = (srow[cx] >> 5) & 0x3FU, b5 = srow[cx] & 0x1FU;
				put(dpx, (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2));
			}
		}
		::msync(m_map, m_mapLen, MS_SYNC);
		m_vinfo.xoffset  = 0U;
		m_vinfo.yoffset  = 0U;
		m_vinfo.activate = FB_ACTIVATE_NOW;
		(void)::ioctl(m_fd, FBIOPAN_DISPLAY, &m_vinfo);
		return;
	}

	// Copy the canvas straight onto the framebuffer, 1:1, centred.
	for (unsigned int cy = 0U; cy < rows; cy++) {
		const uint16_t* srow = &m_fb[(size_t)cy * m_width];
		uint8_t* drow = m_map + (size_t)(m_offY + (int)cy) * m_stride + (size_t)m_offX * bytes;
		if (m_fastRGB565 && bright >= 100U) {
			::memcpy(drow, srow, (size_t)vis * 2U);
			continue;
		}
		for (unsigned int cx = 0U; cx < vis; cx++) {
			unsigned int r5 = (srow[cx] >> 11) & 0x1FU, g6 = (srow[cx] >> 5) & 0x3FU, b5 = srow[cx] & 0x1FU;
			put(drow + (size_t)cx * bytes, (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2));
		}
	}

	// Flush the picture band. msync + pan pushes it out under DRM fbdev.
	size_t bandOff = (size_t)m_offY * m_stride;
	size_t bandLen = (size_t)rows * m_stride;
	::msync(m_map + bandOff, bandLen, MS_SYNC);
	m_vinfo.xoffset  = 0U;
	m_vinfo.yoffset  = 0U;
	m_vinfo.activate = FB_ACTIVATE_NOW;
	(void)::ioctl(m_fd, FBIOPAN_DISPLAY, &m_vinfo);
}

// Grab a VT and put it in graphics mode (like X), so the kernel console stops
// drawing on the framebuffer. Returns false - and we just draw directly - when
// VT=off, there is no console, or no VT is available.
bool CDisplayScreen::setupVT()
{
	if (m_vtConfig == "off" || m_vtConfig == "Off" || m_vtConfig == "OFF")
		return false;

	m_consoleFd = ::open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (m_consoleFd < 0) {
		LogMessage("Screen: no text console (/dev/tty0); drawing directly");
		return false;
	}

	// Which VT: a given number, or a free one (VT_OPENQRY, as X does).
	if (m_vtConfig == "auto" || m_vtConfig.empty()) {
		if (::ioctl(m_consoleFd, VT_OPENQRY, &m_vtNum) < 0 || m_vtNum <= 0) {
			LogWarning("Screen: no free VT; drawing on the current console");
			::close(m_consoleFd); m_consoleFd = -1;
			return false;
		}
	} else {
		m_vtNum = (int)::strtol(m_vtConfig.c_str(), nullptr, 10);
		if (m_vtNum <= 0) {
			LogWarning("Screen: invalid VT '%s'; drawing on the current console", m_vtConfig.c_str());
			::close(m_consoleFd); m_consoleFd = -1;
			return false;
		}
	}

	// The VT to return to on close (the one active now).
	struct vt_stat vts;
	m_origVT = (::ioctl(m_consoleFd, VT_GETSTATE, &vts) == 0) ? (int)vts.v_active : 1;

	char ttyPath[32];
	::snprintf(ttyPath, sizeof(ttyPath), "/dev/tty%d", m_vtNum);
	m_vtFd = ::open(ttyPath, O_RDWR | O_CLOEXEC);
	if (m_vtFd < 0) {
		LogWarning("Screen: cannot open %s; drawing on the current console", ttyPath);
		::close(m_consoleFd); m_consoleFd = -1;
		return false;
	}

	// Switch to our VT and put it in graphics mode (console stops drawing).
	(void)::ioctl(m_vtFd, VT_ACTIVATE, m_vtNum);
	(void)::ioctl(m_vtFd, VT_WAITACTIVE, m_vtNum);
	if (::ioctl(m_vtFd, KDGETMODE, &m_origKDMode) < 0)
		m_origKDMode = KD_TEXT;
	(void)::ioctl(m_vtFd, KDSETMODE, KD_GRAPHICS);

	// Have the kernel signal us around VT switches, so we release the screen to
	// another VT and reclaim it cleanly (Ctrl+Alt+Fn).
	struct sigaction sa;
	::memset(&sa, 0, sizeof(sa));
	sa.sa_handler = vtRelease; (void)::sigaction(SIGUSR1, &sa, nullptr);
	sa.sa_handler = vtAcquire; (void)::sigaction(SIGUSR2, &sa, nullptr);

	struct vt_mode vm;
	::memset(&vm, 0, sizeof(vm));
	vm.mode   = VT_PROCESS;
	vm.relsig = SIGUSR1;
	vm.acqsig = SIGUSR2;
	if (::ioctl(m_vtFd, VT_SETMODE, &vm) < 0)
		LogWarning("Screen: VT_SETMODE failed; a switch away may fight the console");

	g_vtFd         = m_vtFd;
	g_vtActive     = 1;
	g_vtReacquired = 0;
	m_useVT        = true;
	LogMessage("Screen: on VT %d (Ctrl+Alt+F%d to return here, Ctrl+Alt+F1 for the console)", m_vtNum, m_vtNum);
	return true;
}

// Give the VT back: auto switching, text mode, and return to the console.
void CDisplayScreen::teardownVT()
{
	if (!m_useVT)
		return;

	struct vt_mode vm;
	::memset(&vm, 0, sizeof(vm));
	vm.mode = VT_AUTO;
	(void)::ioctl(m_vtFd, VT_SETMODE, &vm);
	(void)::ioctl(m_vtFd, KDSETMODE, m_origKDMode);

	struct sigaction sa;
	::memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	(void)::sigaction(SIGUSR1, &sa, nullptr);
	(void)::sigaction(SIGUSR2, &sa, nullptr);
	g_vtFd = -1;

	if (m_origVT > 0 && m_consoleFd >= 0) {
		(void)::ioctl(m_consoleFd, VT_ACTIVATE, m_origVT);
		(void)::ioctl(m_consoleFd, VT_WAITACTIVE, m_origVT);
	}

	if (m_vtFd >= 0)      { ::close(m_vtFd);      m_vtFd = -1; }
	if (m_consoleFd >= 0) { ::close(m_consoleFd); m_consoleFd = -1; }
	m_useVT = false;
}

// Our VT may have just come back after the user switched away; the console
// could have drawn over the framebuffer, so repaint the current frame at once
// rather than waiting for the next 1 fps flush.
void CDisplayScreen::backendTick()
{
	if (g_vtReacquired) {
		g_vtReacquired = 0;
		if (m_map != nullptr)
			blitToScreen();
	}
}
