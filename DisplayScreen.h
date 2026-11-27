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
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

// A backend of CScreenCanvas that draws the status screens to a Linux
// framebuffer device (/dev/fbN). It is not tied to any particular board or
// output: an HDMI or DSI panel, composite out, an fbtft SPI TFT, or a plain
// framebuffer on any Linux machine all work the same way. The canvas is
// rendered at a scale chosen to fill the screen at its native resolution
// (crisp TrueType text), converted to the framebuffer's
// pixel format, and dimmed in software on idle.

#if !defined(DISPLAYSCREEN_H)
#define DISPLAYSCREEN_H

#include "ScreenCanvas.h"

#include <string>
#include <cstdint>
#include <vector>
#include <linux/fb.h>

class CDisplayScreen : public CScreenCanvas {
public:
	CDisplayScreen(const std::string& device, unsigned int rotation, unsigned int brightness,
		     unsigned int idleBrightness, unsigned int idleTimeout, const std::string& layoutPath = "default",
		     const std::string& vt = "auto");
	virtual ~CDisplayScreen();

	// Detected display geometry (valid after open()). Refresh is 0 if the
	// driver does not report mode timings (common on DRM fbdev emulation).
	unsigned int detectedWidth()  const { return m_fbW; }
	unsigned int detectedHeight() const { return m_fbH; }
	double       detectedRefreshHz() const { return m_refreshHz; }

	// Rotation is 0, 90, 180 or 270 degrees clockwise; anything else is 0.
	static unsigned int validRotation(unsigned int r)
	{
		return (r == 90U || r == 180U || r == 270U) ? r : 0U;
	}

	// Where the picture's pixel (x, y) lands on a panel fbW x fbH pixels in
	// size, the picture turned rot degrees clockwise. The picture is fbW x fbH
	// at 0 and 180, and fbH x fbW (its width and height swapped) at 90 and 270.
	static void toPanel(unsigned int rot, unsigned int fbW, unsigned int fbH,
			    unsigned int x, unsigned int y, unsigned int& px, unsigned int& py)
	{
		switch (rot) {
		case 90U:  px = fbW - 1U - y; py = x;            break;
		case 180U: px = fbW - 1U - x; py = fbH - 1U - y; break;
		case 270U: px = y;            py = fbH - 1U - x; break;
		default:   px = x;            py = y;            break;
		}
	}

protected:
	virtual bool openOutput() override;
	virtual void closeOutput() override;
	virtual void present() override;
	virtual void applyBrightness(unsigned int pct) override;
	virtual void backendTick() override;   // redraw after regaining the VT

private:
	std::string   m_device;
	unsigned int  m_rotation;

	int           m_fd;
	uint8_t*      m_map;          // mmap'd framebuffer (scanout)
	size_t        m_mapLen;
	struct fb_var_screeninfo m_vinfo;

	unsigned int  m_fbW, m_fbH;   // screen resolution
	unsigned int  m_viewW, m_viewH; // the picture's size: the screen's, swapped at 90/270
	unsigned int  m_bpp;          // bits per pixel (16 or 32)
	unsigned int  m_stride;       // bytes per row
	unsigned int  m_rOff, m_rLen, m_gOff, m_gLen, m_bOff, m_bLen;
	int           m_offX, m_offY; // top-left of the canvas in the picture (centred)
	bool          m_fastRGB565;   // scanout is plain little-endian RGB565
	double        m_refreshHz;    // detected refresh rate, 0 if unknown

	unsigned int  m_curBright;    // 0..100

	// ---- Console VT handover ----
	// On a screen that is also the text console, take over a VT the way X does
	// (grab it at startup, go KD_GRAPHICS), so the status screen does not fight
	// the console. Ctrl+Alt+Fn switches away - we stop drawing while another VT
	// is active and redraw when ours comes back.
	std::string   m_vtConfig;     // "auto" | "<n>" | "off"
	bool          m_useVT;        // true once we own a graphics VT
	int           m_consoleFd;    // /dev/tty0, for the VT ioctls
	int           m_vtFd;         // our VT's /dev/ttyN
	int           m_vtNum;        // our VT number
	int           m_origVT;       // the VT to switch back to on close
	int           m_origKDMode;   // the VT's console mode before KD_GRAPHICS

	bool setupVT();               // grab a VT; false = draw directly (no console)
	void teardownVT();            // restore the console

	void blitToScreen();

	// Derive the base canvas geometry from the framebuffer, as the picture is
	// turned: an aspect-matched design width and a height-filling scale.
	static double designWidth(const std::string& device, unsigned int rotation);
	static double canvasScale(const std::string& device, unsigned int rotation);
};

#endif
