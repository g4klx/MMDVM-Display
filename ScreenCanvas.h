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

// CScreenCanvas is the renderer behind the Screen display (CDisplayScreen).
// It paints a status screen into an in-memory RGB565 framebuffer,
// driven by a community-editable layout (see Layout.h): the write* handlers
// compose named data fields, and a layout interpreter draws each element.
//
// The layout is expressed in a fixed 240-high "design space" whose width
// tracks the screen's aspect ratio; TrueType glyphs and vector emblems are
// rasterised at the real output pixel size, so the output is crisp at any
// resolution.

#if !defined(SCREENCANVAS_H)
#define SCREENCANVAS_H

#include "Display.h"
#include "Defines.h"
#include "Timer.h"
#include "Layout.h"
#include "stb_truetype.h"

#include <string>
#include <cstdint>
#include <vector>
#include <map>

class CScreenCanvas : public CDisplay {
public:
	CScreenCanvas(double designW, double designH, double scale, unsigned int brightness,
		   unsigned int idleBrightness, unsigned int idleTimeout, const std::string& layoutPath = "default");
	virtual ~CScreenCanvas();

	virtual bool open() override;
	virtual void close() override;

	bool dumpPNG(const std::string& path) const;   // headless preview

protected:
	// ---- Backend hooks ----
	virtual bool openOutput()  = 0;
	virtual void closeOutput() = 0;
	virtual void present()     = 0;
	virtual void applyBrightness(unsigned int pct) { (void)pct; }
	// Per-clock housekeeping for the backend (e.g. a VT switch becoming
	// active again), run every clockInt before the coalesced flush.
	virtual void backendTick() {}

	// Framebuffer, shared with the backend (RGB565, m_width x m_height px).
	std::vector<uint16_t> m_fb;
	unsigned int m_width;
	unsigned int m_height;

	// ---- CDisplay overrides ----
	virtual void setIdleInt() override;
	virtual void setErrorInt() override;
	virtual void setLockoutInt() override;
	virtual void setQuitInt() override;

	virtual void writeDStarInt(const std::string& my1, const std::string& my2, const std::string& your, const std::string& type, const std::string& reflector) override;
	virtual void writeDStarRSSIInt(int rssi) override;
	virtual void writeDStarBERInt(float ber) override;
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
	virtual void clearFMInt() override;

	virtual void writeCWInt() override;
	virtual void clearCWInt() override;

	virtual void writeGeneralInt(const std::string& callsign, unsigned int id, bool duplex) override;
	virtual void writeCPUInt(float temperature, float frequency, float load) override;
	virtual void writeInfoInt(unsigned int rxFrequency, unsigned int txFrequency, const std::string& location) override;
	virtual void writeIPInt(const std::string& name, const std::string& ipV4, const std::string& ipV6) override;

	virtual void clockInt(unsigned int ms) override;

	// Idle/dim state (the backend decides what a dim does)
	unsigned int m_brightness;
	unsigned int m_idleBrightness;
	unsigned int m_idleTimeout;
	bool         m_dimmed;

private:
	// Coalesced rendering: write*Int only update fields and mark dirty; the
	// render + present happens at most once per flush interval from clockInt.
	bool          m_dirty;
	unsigned int  m_sinceFlush;
	unsigned char m_lastDrawnMode;
	void markDirty() { m_dirty = true; }
	void redraw();

	// Design-space dimensions (doubles, so the pixel canvas can match the fb).
	double       m_designW;
	double       m_designH;
	double       m_scale;

	enum FontSize { SZ_S = 0, SZ_M = 1, SZ_L = 2 };

	// ---- Vector font (stb_truetype) ----
	stbtt_fontinfo m_font;
	bool           m_fontOk;
	double         m_emSize[3];
	double         m_advRatio;
	double         m_ascRatio;
	double         m_lineHRatio;
	double         m_capRatio;     // cap height / em (for vertical centring)

	struct GlyphBmp { int w = 0, h = 0, xoff = 0, yoff = 0; std::vector<uint8_t> cov; };
	std::map<uint32_t, GlyphBmp> m_glyphs;
	size_t         m_glyphBytes;
	const GlyphBmp& glyph(int ch, int renderPx);

	// ---- Overlay image assets (PNG, loaded on demand and cached) ----
	struct ImageBmp { int w = 0, h = 0; bool ok = false; std::vector<uint8_t> rgba; };
	std::map<std::string, ImageBmp> m_images;
	size_t          m_imageBytes;
	std::string     m_overlayBase;        // folder of the overlay file, for relative paths
	const ImageBmp& image(const std::string& path);
	void            drawImage(const LElement& el);

	// ---- Layout + field state ----
	std::string   m_layoutPath;
	CLayout       m_layout;
	std::map<std::string, std::string> m_fields;   // named display values
	unsigned char m_mode;
	bool          m_duplex;
	float         m_temperature;
	std::string   m_location;
	CTimer        m_idleTimer;

	void  setField(const std::string& key, const std::string& value);
	void  clearCallFields();
	void  updateStatus();          // compose the idle temperature/location line
	std::string substitute(const std::string& tmpl, bool& skip) const;

	// Layout interpreter
	void  renderPage(const std::string& name);
	void  renderElement(const LElement& el);
	double   resolveDim(const std::string& tok, double ref) const;  // x/y/w in design units
	double   resolveEm(const std::string& tok) const;               // font size -> em
	uint16_t resolveColour(const std::string& name) const;
	static const char* modeName(unsigned char mode);

	// design->pixel
	inline int px(double designUnits) const;

	// ---- Rendering primitives (inputs in design units) ----
	void fill(uint16_t colour);
	void fillRect(double x, double y, double w, double h, uint16_t colour);
	void hline(double x, double y, double w, uint16_t colour);
	void drawText(double x, double y, const std::string& s, FontSize f, uint16_t colour);
	void drawTextC(double cx, double y, const std::string& s, FontSize f, uint16_t colour);
	void drawTextR(double rx, double y, const std::string& s, FontSize f, uint16_t colour);
	void drawGlyphPx(int px0, int py0, const GlyphBmp& g, uint16_t colour);
	int  lineH(FontSize f) const;

	double textWidthEm(const std::string& s, double em) const;
	double fitEm(const std::string& s, double emMax, double maxWidth) const;
	std::string fitText(const std::string& s, double emMax, double maxWidth, double& emOut) const;
	void drawTextEm(double x, double y, const std::string& s, double em, uint16_t colour);
	void drawTextEmC(double cx, double y, const std::string& s, double em, uint16_t colour);
	void drawTextEmR(double rx, double y, const std::string& s, double em, uint16_t colour);

	// ---- Anti-aliased vector primitives ----
	void blendPx(int x, int y, uint16_t colour, double cov);
	void fillCircleAA(double cx, double cy, double r, uint16_t colour);
	void ringAA(double cx, double cy, double rOuter, double rInner, uint16_t colour);
	void fillRoundRectAA(double x, double y, double w, double h, double r, uint16_t colTop, uint16_t colBot);
	void fillPolyAA(const std::vector<double>& pts, uint16_t colour);

	// Banner + mode emblem (reusable by layouts).
	void drawEmblem(unsigned char mode, uint16_t accent, double x, double y, double s);
	int  banner(unsigned char mode, const char* name, bool wordmark);
	inline double pxf(double designUnits) const { return designUnits * m_scale; }

	void touchActivity();
};

#endif
