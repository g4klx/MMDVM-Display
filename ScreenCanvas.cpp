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

#define STB_TRUETYPE_IMPLEMENTATION
#include "ScreenCanvas.h"
#include "Font_ttf.h"
#include "Log.h"

// PNG decoder for overlay image assets (header-only, public domain). PNG only,
// to keep the binary small; overlays use PNG so logos can carry transparency.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
// stb_image is vendored unmodified; with PNG only, some of its helpers go
// unused, and GCC flags false "maybe uninitialized" paths in it
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <algorithm>

#if !defined(M_PI)
#define M_PI 3.14159265358979323846
#endif

// Palette (RGB565)
static const uint16_t C_WHITE  = 0xFFFFU;
static const uint16_t C_BLACK  = 0x0000U;
static const uint16_t C_BLUE   = 0x045FU;
static const uint16_t C_GREY   = 0x632CU;
static const uint16_t C_GREEN  = 0x04A0U;
static const uint16_t C_RED    = 0xB000U;

static inline void unpack565(uint16_t p, int& r, int& g, int& b)
{
	r = (p >> 11) & 0x1F; g = (p >> 5) & 0x3F; b = p & 0x1F;
}
static inline uint16_t pack565(int r, int g, int b)
{
	return (uint16_t)((r << 11) | (g << 5) | b);
}
static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t cov)
{
	if (cov == 0U)   return bg;
	if (cov == 255U) return fg;
	int br, bgr, bb, fr, fgr, fb;
	unpack565(bg, br, bgr, bb); unpack565(fg, fr, fgr, fb);
	int r = br + ((fr - br) * cov) / 255;
	int g = bgr + ((fgr - bgr) * cov) / 255;
	int b = bb + ((fb - bb) * cov) / 255;
	return pack565(r, g, b);
}

CScreenCanvas::CScreenCanvas(double designW, double designH, double scale, unsigned int brightness,
		       unsigned int idleBrightness, unsigned int idleTimeout, const std::string& layoutPath) :
CDisplay(),
m_width((unsigned int)lround(designW * scale)),
m_height((unsigned int)lround(designH * scale)),
m_brightness(brightness),
m_idleBrightness(idleBrightness),
m_idleTimeout(idleTimeout),
m_dimmed(false),
m_dirty(false),
m_sinceFlush(0U),
m_lastDrawnMode(0xFFU),
m_designW(designW),
m_designH(designH),
m_scale(scale),
m_fontOk(false),
m_glyphBytes(0U),
m_imageBytes(0U),
m_layoutPath(layoutPath),
m_mode(MODE_IDLE),
m_duplex(false),
m_temperature(0.0F),
m_location(),
m_idleTimer(1000U, idleTimeout)
{
	// Relative overlay image paths resolve against the overlay file's folder.
	if (m_layoutPath != "default") {
		size_t slash = m_layoutPath.find_last_of('/');
		if (slash != std::string::npos)
			m_overlayBase = m_layoutPath.substr(0, slash);
	}

	m_emSize[SZ_S] = 17.0; m_emSize[SZ_M] = 22.0; m_emSize[SZ_L] = 30.0;

	int off = stbtt_GetFontOffsetForIndex(dejavu_mono_bold, 0);
	m_fontOk = (off >= 0) && (stbtt_InitFont(&m_font, dejavu_mono_bold, off) != 0);
	if (!m_fontOk)
		LogError("Screen: embedded font failed to initialise; text will be blank");

	int asc = 0, desc = 0, gap = 0, aw = 0, lsb = 0;
	double sc1 = 0.0;   // scale for an em of 1 design px == 1 / unitsPerEm
	int capY0 = 0, capY1 = 0, cx0 = 0, cx1 = 0;
	if (m_fontOk) {
		stbtt_GetFontVMetrics(&m_font, &asc, &desc, &gap);
		stbtt_GetCodepointHMetrics(&m_font, '0', &aw, &lsb);
		stbtt_GetCodepointBox(&m_font, 'H', &cx0, &capY0, &cx1, &capY1);
		sc1 = stbtt_ScaleForMappingEmToPixels(&m_font, 1.0f);
	}
	m_advRatio   = aw * sc1;
	m_ascRatio   = asc * sc1;
	m_lineHRatio = (asc - desc + gap) * sc1;
	m_capRatio   = (capY1 - capY0) * sc1;   // cap height, em units
}

CScreenCanvas::~CScreenCanvas()
{
}

inline int CScreenCanvas::px(double designUnits) const
{
	return (int)lround(designUnits * m_scale);
}

bool CScreenCanvas::open()
{
	if (m_width == 0U || m_height == 0U || m_width > 8192U || m_height > 8192U) {
		LogError("Screen: implausible canvas size %ux%u", m_width, m_height);
		return false;
	}
	try {
		m_fb.assign((size_t)m_width * m_height, C_WHITE);
	} catch (const std::bad_alloc&) {
		LogError("Screen: cannot allocate %ux%u canvas", m_width, m_height);
		return false;
	}

	if (!openOutput()) {
		m_fb.clear();
		m_fb.shrink_to_fit();
		return false;
	}

	m_layout.load(m_layoutPath);
	m_mode = MODE_IDLE;
	redraw();
	present();
	m_idleTimer.start();
	m_dirty = false;
	m_sinceFlush = 0U;
	m_lastDrawnMode = m_mode;
	return true;
}

void CScreenCanvas::close()
{
	closeOutput();
	m_fb.clear();
	m_fb.shrink_to_fit();
}

// ---- Vector font ----

const CScreenCanvas::GlyphBmp& CScreenCanvas::glyph(int ch, int renderPx)
{
	uint32_t key = ((uint32_t)renderPx << 9) | ((uint32_t)ch & 0x1FFU);
	auto it = m_glyphs.find(key);
	if (it != m_glyphs.end())
		return it->second;

	GlyphBmp g;
	if (m_fontOk && renderPx > 0) {
		double s = stbtt_ScaleForMappingEmToPixels(&m_font, (float)renderPx);
		int w = 0, h = 0, xo = 0, yo = 0;
		unsigned char* bmp = stbtt_GetCodepointBitmap(&m_font, (float)s, (float)s, ch, &w, &h, &xo, &yo);
		g.w = w; g.h = h; g.xoff = xo; g.yoff = yo;
		if (bmp != nullptr) {
			g.cov.assign(bmp, bmp + (size_t)w * h);
			stbtt_FreeBitmap(bmp, nullptr);
		}
	}
	m_glyphBytes += g.cov.size() + sizeof(GlyphBmp);
	auto res = m_glyphs.emplace(key, std::move(g));
	return res.first->second;
}

void CScreenCanvas::drawGlyphPx(int px0, int py0, const GlyphBmp& g, uint16_t colour)
{
	for (int gy = 0; gy < g.h; gy++) {
		int py = py0 + gy;
		if (py < 0 || py >= (int)m_height) continue;
		const uint8_t* row = &g.cov[(size_t)gy * g.w];
		for (int gx = 0; gx < g.w; gx++) {
			int pxc = px0 + gx;
			if (pxc < 0 || pxc >= (int)m_width) continue;
			uint8_t cov = row[gx];
			if (cov == 0U) continue;
			uint16_t* dst = &m_fb[(size_t)py * m_width + pxc];
			*dst = blend565(*dst, colour, cov);
		}
	}
}

int CScreenCanvas::lineH(FontSize f) const
{
	return (int)lround(m_lineHRatio * m_emSize[f]);
}

double CScreenCanvas::textWidthEm(const std::string& s, double em) const
{
	return (double)s.size() * m_advRatio * em;   // design units
}

// Shrink the em until the string fits maxWidth (design units), with a floor.
// The result is quantised so that the rasterised pixel size snaps to a small
// set of values, which bounds how many distinct glyph bitmaps get cached.
double CScreenCanvas::fitEm(const std::string& s, double emMax, double maxWidth) const
{
	double natural = textWidthEm(s, emMax);
	if (natural <= maxWidth || natural <= 0.0 || maxWidth <= 0.0)
		return emMax;
	double em = emMax * (maxWidth / natural);
	double floorEm = emMax * 0.45;        // don't shrink below ~45% of base
	if (floorEm < 8.0) floorEm = 8.0;
	if (em < floorEm) em = floorEm;
	// Snap the *render* pixel size to a 2px grid so similar-length strings
	// share cached glyphs instead of each caching a unique size.
	if (m_scale > 0.0) {
		double rpx = em * m_scale;
		rpx = std::floor(rpx / 2.0) * 2.0;
		if (rpx < 6.0) rpx = 6.0;
		em = rpx / m_scale;
	}
	return em;
}

void CScreenCanvas::drawTextEm(double x, double y, const std::string& s, double em, uint16_t colour)
{
	int rpx      = px(em);
	int penx     = px(x);
	// y is the vertical CENTRE of the text: drop the baseline by half the cap
	// height so the glyphs sit centred on y regardless of size.
	int baseline = px(y) + px(m_capRatio * em * 0.5);
	int advpx    = px(m_advRatio * em);
	for (unsigned char c : s) {
		if (c >= 0x20U) {           // control bytes render as a blank cell
			const GlyphBmp& g = glyph((int)c, rpx);
			if (!g.cov.empty())
				drawGlyphPx(penx + g.xoff, baseline + g.yoff, g, colour);
		}
		penx += advpx;
	}
}

void CScreenCanvas::drawTextEmC(double cx, double y, const std::string& s, double em, uint16_t colour)
{
	drawTextEm(cx - textWidthEm(s, em) / 2.0, y, s, em, colour);
}

void CScreenCanvas::drawTextEmR(double rx, double y, const std::string& s, double em, uint16_t colour)
{
	drawTextEm(rx - textWidthEm(s, em), y, s, em, colour);
}

void CScreenCanvas::drawText(double x, double y, const std::string& s, FontSize f, uint16_t colour)
{
	drawTextEm(x, y, s, m_emSize[f], colour);
}

void CScreenCanvas::drawTextC(double cx, double y, const std::string& s, FontSize f, uint16_t colour)
{
	drawTextEmC(cx, y, s, m_emSize[f], colour);
}

void CScreenCanvas::drawTextR(double rx, double y, const std::string& s, FontSize f, uint16_t colour)
{
	drawTextEmR(rx, y, s, m_emSize[f], colour);
}

std::string CScreenCanvas::fitText(const std::string& s, double emMax, double maxWidth, double& emOut) const
{
	double em = fitEm(s, emMax, maxWidth);
	// If it still overflows at the shrink floor, truncate with an ellipsis.
	if (textWidthEm(s, em) > maxWidth && !s.empty()) {
		double chw = m_advRatio * em;   // per-char design width (monospace)
		int maxChars = chw > 0.0 ? (int)(maxWidth / chw) : 0;
		emOut = em;
		if (maxChars <= 0) return std::string();
		if (maxChars <= 3) return s.substr(0, maxChars);
		return s.substr(0, maxChars - 3) + "...";
	}
	emOut = em;
	return s;
}

// ---- Primitives ----

void CScreenCanvas::fill(uint16_t colour)
{
	size_t n = (size_t)m_width * m_height;
	for (size_t i = 0; i < n; i++)
		m_fb[i] = colour;
}

void CScreenCanvas::fillRect(double x, double y, double w, double h, uint16_t colour)
{
	int x0 = px(x), y0 = px(y), x1 = px(x + w), y1 = px(y + h);
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > (int)m_width)  x1 = m_width;
	if (y1 > (int)m_height) y1 = m_height;
	for (int j = y0; j < y1; j++)
		for (int i = x0; i < x1; i++)
			m_fb[(size_t)j * m_width + i] = colour;
}

void CScreenCanvas::hline(double x, double y, double w, uint16_t colour)
{
	fillRect(x, y, w, 1.0, colour);
}

// ---- Colour helpers ----
static inline uint16_t mix565(uint16_t a, uint16_t b, double t)
{
	int ar, ag, ab, br, bg, bb;
	unpack565(a, ar, ag, ab); unpack565(b, br, bg, bb);
	return pack565((int)lround(ar + (br - ar) * t),
		       (int)lround(ag + (bg - ag) * t),
		       (int)lround(ab + (bb - ab) * t));
}
static inline uint16_t lighten(uint16_t c, double amt) { return mix565(c, C_WHITE, amt); }
static inline uint16_t darken(uint16_t c, double amt)  { return mix565(c, C_BLACK, amt); }
static inline uint16_t RGB(int r, int g, int b) { return pack565(r >> 3, g >> 2, b >> 3); }

static const uint16_t GREY_DK = RGB(38, 44, 52);     // near-black slate for names
static const uint16_t GREY_MD = RGB(120, 130, 140);  // subtitle grey

// Per-mode accent colours (an original palette, not copied from any brand logo).
static uint16_t accentFor(unsigned char mode)
{
	switch (mode) {
	case MODE_DMR:    return RGB(0, 150, 160);   // teal
	case MODE_DSTAR:  return RGB(30, 120, 210);  // blue
	case MODE_YSF:    return RGB(90, 70, 190);   // violet
	case MODE_P25:    return RGB(225, 120, 0);   // amber
	case MODE_NXDN:   return RGB(0, 160, 95);    // green
	case MODE_POCSAG: return RGB(210, 60, 120);  // pink
	case MODE_FM:     return RGB(110, 120, 130); // grey
	default:          return RGB(25, 160, 225);  // MMDVM blue
	}
}

// ---- Anti-aliased vector primitives (design-space inputs) ----

void CScreenCanvas::blendPx(int x, int y, uint16_t colour, double cov)
{
	if (x < 0 || x >= (int)m_width || y < 0 || y >= (int)m_height) return;
	if (cov <= 0.0) return;
	if (cov > 1.0) cov = 1.0;
	uint16_t* dst = &m_fb[(size_t)y * m_width + x];
	*dst = blend565(*dst, colour, (uint8_t)lround(cov * 255.0));
}

void CScreenCanvas::fillCircleAA(double cx, double cy, double r, uint16_t colour)
{
	double fcx = pxf(cx), fcy = pxf(cy), fr = r * m_scale;
	int x0 = (int)std::floor(fcx - fr - 1), x1 = (int)std::ceil(fcx + fr + 1);
	int y0 = (int)std::floor(fcy - fr - 1), y1 = (int)std::ceil(fcy + fr + 1);
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) {
			double dx = x + 0.5 - fcx, dy = y + 0.5 - fcy;
			blendPx(x, y, colour, fr - std::sqrt(dx * dx + dy * dy) + 0.5);
		}
}

void CScreenCanvas::ringAA(double cx, double cy, double rOuter, double rInner, uint16_t colour)
{
	double fcx = pxf(cx), fcy = pxf(cy), ro = rOuter * m_scale, ri = rInner * m_scale;
	int x0 = (int)std::floor(fcx - ro - 1), x1 = (int)std::ceil(fcx + ro + 1);
	int y0 = (int)std::floor(fcy - ro - 1), y1 = (int)std::ceil(fcy + ro + 1);
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) {
			double dx = x + 0.5 - fcx, dy = y + 0.5 - fcy;
			double d = std::sqrt(dx * dx + dy * dy);
			blendPx(x, y, colour, std::min(ro - d, d - ri) + 0.5);
		}
}

void CScreenCanvas::fillRoundRectAA(double x, double y, double w, double h, double r, uint16_t colTop, uint16_t colBot)
{
	double fx = pxf(x), fy = pxf(y), fw = w * m_scale, fh = h * m_scale, fr = r * m_scale;
	double cx = fx + fw / 2, cy = fy + fh / 2, hx = fw / 2 - fr, hy = fh / 2 - fr;
	int x0 = (int)std::floor(fx - 1), x1 = (int)std::ceil(fx + fw + 1);
	int y0 = (int)std::floor(fy - 1), y1 = (int)std::ceil(fy + fh + 1);
	for (int py = y0; py <= y1; py++) {
		double t = fh > 0 ? ((py + 0.5) - fy) / fh : 0.0;
		if (t < 0) t = 0;
		if (t > 1) t = 1;
		uint16_t col = (colTop == colBot) ? colTop : mix565(colTop, colBot, t);
		for (int pxc = x0; pxc <= x1; pxc++) {
			double dx = std::fabs(pxc + 0.5 - cx) - hx;
			double dy = std::fabs(py + 0.5 - cy) - hy;
			double ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
			double sd = std::sqrt(ox * ox + oy * oy) - fr;
			blendPx(pxc, py, col, 0.5 - sd);
		}
	}
}

void CScreenCanvas::fillPolyAA(const std::vector<double>& pts, uint16_t colour)
{
	int n = (int)pts.size() / 2;
	if (n < 3) return;
	double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
	std::vector<double> fx(n), fy(n);
	for (int i = 0; i < n; i++) {
		fx[i] = pxf(pts[i * 2]); fy[i] = pxf(pts[i * 2 + 1]);
		minx = std::min(minx, fx[i]); maxx = std::max(maxx, fx[i]);
		miny = std::min(miny, fy[i]); maxy = std::max(maxy, fy[i]);
	}
	int x0 = (int)std::floor(minx) - 1, x1 = (int)std::ceil(maxx) + 1;
	int y0 = (int)std::floor(miny) - 1, y1 = (int)std::ceil(maxy) + 1;
	const int SS = 4;
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) {
			int inside = 0;
			for (int sy = 0; sy < SS; sy++)
				for (int sx = 0; sx < SS; sx++) {
					double px_ = x + (sx + 0.5) / SS, py_ = y + (sy + 0.5) / SS;
					bool in = false;
					for (int i = 0, j = n - 1; i < n; j = i++)
						if (((fy[i] > py_) != (fy[j] > py_)) &&
						    (px_ < (fx[j] - fx[i]) * (py_ - fy[i]) / (fy[j] - fy[i]) + fx[i]))
							in = !in;
					if (in) inside++;
				}
			if (inside) blendPx(x, y, colour, (double)inside / (SS * SS));
		}
}

// ---- Mode emblem: slick accent tile + white icon ----

void CScreenCanvas::drawEmblem(unsigned char mode, uint16_t accent, double x, double y, double s)
{
	fillRoundRectAA(x + s * 0.035, y + s * 0.07, s, s, s * 0.26, RGB(206, 211, 217), RGB(206, 211, 217)); // shadow
	fillRoundRectAA(x, y, s, s, s * 0.26, lighten(accent, 0.30), darken(accent, 0.06));            // tile

	const uint16_t W = C_WHITE;
	double cx = x + s / 2, cy = y + s / 2;

	switch (mode) {
	case MODE_DMR: {
		double bw = s * 0.15, bh = s * 0.46, gap = s * 0.1;
		fillRoundRectAA(cx - gap / 2 - bw, cy - bh / 2, bw, bh, bw / 2, W, W);
		fillRoundRectAA(cx + gap / 2,      cy - bh / 2, bw, bh, bw / 2, W, W);
		break;
	}
	case MODE_DSTAR: {
		double R = s * 0.34, r = s * 0.13;
		std::vector<double> p;
		for (int i = 0; i < 8; i++) {
			double a = -M_PI / 2 + i * M_PI / 4, rad = (i % 2 == 0) ? R : r;
			p.push_back(cx + rad * std::cos(a)); p.push_back(cy + rad * std::sin(a));
		}
		fillPolyAA(p, W);
		break;
	}
	case MODE_YSF: {
		double rr = s * 0.2, th = s * 0.08, off = s * 0.13;
		ringAA(cx - off, cy, rr, rr - th, W);
		ringAA(cx + off, cy, rr, rr - th, W);
		break;
	}
	case MODE_P25: {
		double w = s * 0.5, top = cy - s * 0.3, bot = cy + s * 0.34;
		std::vector<double> p = { cx - w / 2, top, cx + w / 2, top,
					  cx + w / 2, cy + s * 0.05, cx, bot, cx - w / 2, cy + s * 0.05 };
		fillPolyAA(p, W);
		break;
	}
	case MODE_NXDN: {
		double R = s * 0.33;
		std::vector<double> p;
		for (int i = 0; i < 6; i++) {
			double a = -M_PI / 2 + i * M_PI / 3;
			p.push_back(cx + R * std::cos(a)); p.push_back(cy + R * std::sin(a));
		}
		fillPolyAA(p, W);
		fillCircleAA(cx, cy, s * 0.11, darken(accent, 0.06));
		break;
	}
	case MODE_POCSAG: {
		fillCircleAA(cx, cy + s * 0.16, s * 0.07, W);
		ringAA(cx, cy + s * 0.16, s * 0.2, s * 0.145, W);
		ringAA(cx, cy + s * 0.16, s * 0.33, s * 0.275, W);
		break;
	}
	default: {   // MMDVM: three ascending signal bars
		double bw = s * 0.13, gap = s * 0.08, base = cy + s * 0.24;
		for (int i = 0; i < 3; i++) {
			double bh = s * (0.2 + i * 0.13);
			double bx = cx - (bw * 1.5 + gap) + i * (bw + gap);
			fillRoundRectAA(bx, base - bh, bw, bh, bw * 0.3, W, W);
		}
		break;
	}
	}
}

int CScreenCanvas::banner(unsigned char mode, const char* name, bool wordmark)
{
	const int H = 100;
	const uint16_t accent = accentFor(mode);

	if (wordmark) {
		std::string mark = "MMDVM";
		double em = fitEm(mark, 50.0, (double)m_designW - 48);
		double tw = textWidthEm(mark, em), adv = m_advRatio * em;
		double x = m_designW / 2.0 - tw / 2.0, y = 38;      // centre of the wordmark
		drawTextEm(x, y, "MM", em, GREY_DK);
		drawTextEm(x + 2 * adv, y, "DVM", em, accent);

		std::string sub = "MULTIMODE DIGITAL VOICE MODEM";
		double subEm = fitEm(sub, 12.5, (double)m_designW - 30);
		double subY = y + m_capRatio * em * 0.5 + m_capRatio * subEm * 0.5 + 6;
		drawTextEmC(m_designW / 2.0, subY, sub, subEm, GREY_MD);
		fillRoundRectAA(m_designW / 2.0 - 46, subY + m_capRatio * subEm * 0.5 + 4, 92, 4, 2, accent, accent);
		return H;
	}

	double s = 60;
	double em = fitEm(name, 46.0, (double)m_designW - (s + 44));
	double nameW = textWidthEm(name, em);
	double gap = 16, lockW = s + gap + nameW;
	double lx = (m_designW - lockW) / 2.0, ey = (H - s) / 2.0 - 6;

	drawEmblem(mode, accent, lx, ey, s);
	double ny = ey + s / 2.0;                              // name centre aligns with emblem centre
	drawTextEm(lx + s + gap, ny, name, em, GREY_DK);
	fillRoundRectAA(lx + s + gap, ny + m_capRatio * em * 0.5 + 6, nameW, 4, 2, accent, accent);
	return H;
}

static std::string mhz(unsigned int hz)
{
	if (hz == 0U) return "---.----";
	char buf[32];
	::snprintf(buf, sizeof(buf), "%.4f", (double)hz / 1000000.0);
	return buf;
}

// ---- Field helpers ----

void CScreenCanvas::setField(const std::string& key, const std::string& value)
{
	if (value.empty()) m_fields.erase(key);
	else               m_fields[key] = value;
}

void CScreenCanvas::clearCallFields()
{
	static const char* keys[] = { "src","dst","info","ta","rssi1","ber1",
		"s1src","s1dst","s1ta","s1rssi","s1ber","s2src","s2dst","s2ta","s2rssi","s2ber" };
	for (const char* k : keys) m_fields.erase(k);
}

void CScreenCanvas::updateStatus()
{
	std::string s;
	if (m_temperature > 0.1F) { char b[32]; ::snprintf(b, sizeof(b), "%.1f C", m_temperature); s = b; }
	if (!m_location.empty()) s += (s.empty() ? "" : "   ") + m_location;
	setField("status", s);
}

const char* CScreenCanvas::modeName(unsigned char mode)
{
	switch (mode) {
	case MODE_DSTAR:  return "D-STAR";
	case MODE_DMR:    return "DMR";
	case MODE_YSF:    return "FUSION";
	case MODE_P25:    return "P25";
	case MODE_NXDN:   return "NXDN";
	case MODE_POCSAG: return "POCSAG";
	case MODE_FM:     return "FM";
	default:          return "MMDVM";
	}
}

// Expand $field references; set skip if the element should not be drawn
// (any referenced field is empty, or the result is empty).
std::string CScreenCanvas::substitute(const std::string& tmpl, bool& skip) const
{
	std::string out;
	bool hadField = false, emptyField = false;
	for (size_t i = 0; i < tmpl.size(); ) {
		if (tmpl[i] == '$') {
			size_t j = i + 1;
			while (j < tmpl.size() && (::isalnum((unsigned char)tmpl[j]) || tmpl[j] == '_' || tmpl[j] == '.')) j++;
			std::string name = tmpl.substr(i + 1, j - i - 1);
			hadField = true;
			auto it = m_fields.find(name);
			if (it == m_fields.end() || it->second.empty()) emptyField = true;
			else out += it->second;
			i = j;
		} else {
			out += tmpl[i];
			i++;
		}
	}
	skip = (hadField && emptyField) || out.empty();
	return out;
}

// ---- Layout interpreter ----

double CScreenCanvas::resolveDim(const std::string& tok, double ref) const
{
	double total = 0.0, sign = 1.0;
	size_t i = 0;
	while (i < tok.size()) {
		size_t j = i;
		while (j < tok.size() && tok[j] != '+' && tok[j] != '-') j++;
		std::string term = tok.substr(i, j - i);
		size_t a = term.find_first_not_of(" \t");
		if (a != std::string::npos) {
			size_t b = term.find_last_not_of(" \t");
			term = term.substr(a, b - a + 1);
			double v;
			if      (term == "center" || term == "c") v = ref / 2.0;
			else if (term == "right"  || term == "r" || term == "bottom" || term == "b") v = ref;
			else if (term == "left"   || term == "l" || term == "top"    || term == "t") v = 0.0;
			else if (term == "fill") v = ref - 32.0;
			else if (!term.empty() && term.back() == '%') v = ::atof(term.c_str()) * ref / 100.0;
			else v = ::atof(term.c_str());
			total += sign * v;
		}
		if (j < tok.size()) { sign = (tok[j] == '-') ? -1.0 : 1.0; i = j + 1; }
		else i = j;
	}
	return total;
}

double CScreenCanvas::resolveEm(const std::string& tok) const
{
	if (tok == "S" || tok == "s") return m_emSize[SZ_S];
	if (tok == "L" || tok == "l") return m_emSize[SZ_L];
	if (tok == "M" || tok == "m" || tok.empty()) return m_emSize[SZ_M];
	double v = ::atof(tok.c_str());
	return v > 0.0 ? v : m_emSize[SZ_M];
}

uint16_t CScreenCanvas::resolveColour(const std::string& name) const
{
	if (name == "ink" || name.empty()) return GREY_DK;
	if (name == "black")               return C_BLACK;
	if (name == "grey" || name == "gray") return C_GREY;
	if (name == "white")               return C_WHITE;
	if (name == "green")               return C_GREEN;
	if (name == "red")                 return C_RED;
	if (name == "blue")                return C_BLUE;
	if (name == "accent")              return accentFor(m_mode);
	return GREY_DK;
}

// Load (and cache) a PNG overlay asset as RGBA. Failures are cached too, so a
// missing file is logged once and then quietly skipped on every later frame.
const CScreenCanvas::ImageBmp& CScreenCanvas::image(const std::string& path)
{
	std::map<std::string, ImageBmp>::iterator it = m_images.find(path);
	if (it != m_images.end())
		return it->second;

	static const size_t IMAGE_CACHE_CAP = 16U * 1024U * 1024U;   // bytes
	if (m_imageBytes > IMAGE_CACHE_CAP) { m_images.clear(); m_imageBytes = 0U; }

	ImageBmp& slot = m_images[path];        // inserts an empty (ok=false) entry

	std::string full = path;
	if (!path.empty() && path[0] != '/' && !m_overlayBase.empty())
		full = m_overlayBase + "/" + path;

	int w = 0, h = 0, n = 0;
	unsigned char* data = stbi_load(full.c_str(), &w, &h, &n, 4);
	if (data != nullptr && w > 0 && h > 0) {
		slot.w = w; slot.h = h; slot.ok = true;
		slot.rgba.assign(data, data + (size_t)w * h * 4);
		m_imageBytes += slot.rgba.size();
	} else {
		LogWarning("Overlay image '%s' could not be loaded (PNG expected)", full.c_str());
	}
	if (data != nullptr)
		stbi_image_free(data);
	return slot;
}

// Draw a PNG asset into the design space, preserving its aspect ratio. One of
// w/h sizes it (the other follows the aspect); both give a box it fits inside.
// x is anchored by align (left/center/right); y is the element's vertical centre.
void CScreenCanvas::drawImage(const LElement& el)
{
	if (el.text.empty())
		return;
	const ImageBmp& img = image(el.text);
	if (!img.ok)
		return;

	double aspect = (double)img.w / (double)img.h;
	bool haveW = !el.w.empty(), haveH = !el.h.empty();
	double destW, destH;
	if (haveW && haveH) {
		double bw = resolveDim(el.w, m_designW), bh = resolveDim(el.h, m_designH);
		if (bw / bh > aspect) { destH = bh; destW = bh * aspect; }
		else                  { destW = bw; destH = bw / aspect; }
	} else if (haveH) {
		destH = resolveDim(el.h, m_designH); destW = destH * aspect;
	} else if (haveW) {
		destW = resolveDim(el.w, m_designW); destH = destW / aspect;
	} else {
		return;                             // need at least one of w/h
	}

	double x = resolveDim(el.x, m_designW);
	double y = resolveDim(el.y, m_designH);     // vertical centre
	double left;
	if      (el.align == "center" || el.align == "c") left = x - destW / 2.0;
	else if (el.align == "right"  || el.align == "r") left = x - destW;
	else                                              left = x;
	double top = y - destH / 2.0;

	int px0 = px(left), py0 = px(top);
	int pw  = px(left + destW) - px0;
	int ph  = px(top + destH)  - py0;
	if (pw <= 0 || ph <= 0)
		return;

	auto texel = [&](int ix, int iy, int c) -> double {
		if (ix < 0) ix = 0; else if (ix >= img.w) ix = img.w - 1;
		if (iy < 0) iy = 0; else if (iy >= img.h) iy = img.h - 1;
		return img.rgba[((size_t)iy * img.w + ix) * 4 + c];
	};
	auto sample = [&](double fu, double fv, int c) -> double {
		int x0 = (int)std::floor(fu), y0 = (int)std::floor(fv);
		double tx = fu - x0, ty = fv - y0;
		double a = texel(x0, y0, c)     * (1.0 - tx) + texel(x0 + 1, y0, c)     * tx;
		double b = texel(x0, y0 + 1, c) * (1.0 - tx) + texel(x0 + 1, y0 + 1, c) * tx;
		return a * (1.0 - ty) + b * ty;
	};

	for (int dy = 0; dy < ph; dy++) {
		double v = ((dy + 0.5) / ph) * img.h - 0.5;
		for (int dx = 0; dx < pw; dx++) {
			double u = ((dx + 0.5) / pw) * img.w - 0.5;
			double a = sample(u, v, 3);
			if (a <= 0.5)
				continue;
			uint8_t r = (uint8_t)lround(sample(u, v, 0));
			uint8_t g = (uint8_t)lround(sample(u, v, 1));
			uint8_t b = (uint8_t)lround(sample(u, v, 2));
			blendPx(px0 + dx, py0 + dy, RGB(r, g, b), a / 255.0);
		}
	}
}

void CScreenCanvas::renderElement(const LElement& el)
{
	if (el.kind == "wordmark") { banner(MODE_IDLE, "MMDVM", true); return; }
	if (el.kind == "banner")   { banner(m_mode, modeName(m_mode), false); return; }
	if (el.kind == "image")    { drawImage(el); return; }

	if (el.kind == "rule") {
		hline(resolveDim(el.x, m_designW), resolveDim(el.y, m_designH),
		      resolveDim(el.w, m_designW), resolveColour(el.color));
		return;
	}
	if (el.kind == "rect") {
		double x = resolveDim(el.x, m_designW), y = resolveDim(el.y, m_designH);
		double w = resolveDim(el.w, m_designW), h = resolveDim(el.h, m_designH);
		uint16_t col = resolveColour(el.color);
		if (!el.radius.empty())
			fillRoundRectAA(x, y, w, h, resolveDim(el.radius, m_designW), col, col);
		else
			fillRect(x, y, w, h, col);
		return;
	}
	if (el.kind == "text") {
		bool skip = false;
		std::string s = substitute(el.text, skip);
		if (skip) return;
		double x = resolveDim(el.x, m_designW), y = resolveDim(el.y, m_designH);
		double em = resolveEm(el.size);
		uint16_t col = resolveColour(el.color);
		if (!el.fit.empty()) {
			double em2;
			s = fitText(s, em, resolveDim(el.fit, m_designW), em2);
			em = em2;
		}
		if      (el.align == "center" || el.align == "c") drawTextEmC(x, y, s, em, col);
		else if (el.align == "right"  || el.align == "r") drawTextEmR(x, y, s, em, col);
		else                                              drawTextEm(x, y, s, em, col);
		return;
	}
}

void CScreenCanvas::renderPage(const std::string& name)
{
	fill(C_WHITE);
	const std::vector<LElement>* elems = m_layout.page(name);
	if (elems == nullptr) elems = m_layout.page("call");
	if (elems == nullptr) return;
	for (const LElement& el : *elems)
		renderElement(el);
}

void CScreenCanvas::touchActivity()
{
	m_idleTimer.start();
	if (m_dimmed) {
		m_dimmed = false;
		applyBrightness(m_brightness);
	}
}

// ---- Render dispatch ----

static const size_t GLYPH_CACHE_CAP = 6U * 1024U * 1024U;   // bytes
// A status screen is not video: cap same-page updates to 1 fps. Page/mode
// changes bypass this and show at once, so a new call still appears instantly.
static const unsigned int FLUSH_INTERVAL_MS = 1000U;

// DMR slot 1/2 -> array index 0/1 (simplex always 0); -1 if out of range.
static int dmrPos(unsigned int slotNo, bool duplex)
{
	if (!duplex) return 0;
	if (slotNo < 1U || slotNo > 2U) return -1;
	return (int)slotNo - 1;
}

void CScreenCanvas::redraw()
{
	if (m_fb.empty())
		return;
	if (m_glyphBytes > GLYPH_CACHE_CAP) {   // bound cache memory (never mid-draw)
		m_glyphs.clear();
		m_glyphBytes = 0U;
	}
	const char* page;
	switch (m_mode) {
	case MODE_IDLE:    page = "idle";    break;
	case MODE_ERROR:   page = "error";   break;
	case MODE_LOCKOUT: page = "lockout"; break;
	case MODE_QUIT:    page = "quit";    break;
	case MODE_CW:      page = "cw";      break;
	case MODE_DMR:     page = m_duplex ? "dmr2" : "call"; break;
	default:           page = "call";    break;
	}
	renderPage(page);
}

// ---- CDisplay overrides (these only mutate fields + mark dirty) ----

void CScreenCanvas::setIdleInt()
{
	m_mode = MODE_IDLE;
	clearCallFields();
	m_idleTimer.start();
	markDirty();
}

void CScreenCanvas::setErrorInt()   { m_mode = MODE_ERROR;   touchActivity(); markDirty(); }
void CScreenCanvas::setLockoutInt() { m_mode = MODE_LOCKOUT; touchActivity(); markDirty(); }
void CScreenCanvas::setQuitInt()    { m_mode = MODE_QUIT;    markDirty(); }

void CScreenCanvas::writeDStarInt(const std::string& my1, const std::string& my2, const std::string& your, const std::string& type, const std::string& reflector)
{
	clearCallFields();
	char buf[128];
	::snprintf(buf, sizeof(buf), "%s %s/%s", type.c_str(), my1.c_str(), my2.c_str());
	setField("src", buf);
	setField("dst", your);
	setField("info", (reflector != "        ") ? ("via " + reflector) : "");
	m_mode = MODE_DSTAR;
	touchActivity();
	markDirty();
}

void CScreenCanvas::writeDStarRSSIInt(int rssi) { char b[32]; ::snprintf(b, sizeof(b), "RSSI %ddBm", rssi); setField("rssi1", b); markDirty(); }
void CScreenCanvas::writeDStarBERInt(float ber) { char b[32]; ::snprintf(b, sizeof(b), "BER %.1f%%", ber);  setField("ber1",  b); markDirty(); }
void CScreenCanvas::clearDStarInt() { clearCallFields(); setField("src", "Listening"); markDirty(); }

void CScreenCanvas::writeDMRInt(unsigned int slotNo, const std::string& src, bool group, unsigned int dst, const std::string& type)
{
	int pos = dmrPos(slotNo, m_duplex);
	if (pos < 0) return;
	char buf[128];
	if (m_duplex) {
		std::string p = (pos == 0) ? "s1" : "s2";
		::snprintf(buf, sizeof(buf), "%s %s", type.c_str(), src.c_str()); setField(p + "src", buf);
		::snprintf(buf, sizeof(buf), "%s%u", group ? "TG" : "", dst);     setField(p + "dst", buf);
	} else {
		clearCallFields();
		::snprintf(buf, sizeof(buf), "%s %s", type.c_str(), src.c_str());         setField("src", buf);
		::snprintf(buf, sizeof(buf), "TS%u %s%u", slotNo, group ? "TG" : "", dst); setField("dst", buf);
	}
	m_mode = MODE_DMR;
	touchActivity();
	markDirty();
}

void CScreenCanvas::writeDMRRSSIInt(unsigned int slotNo, int rssi)
{
	int pos = dmrPos(slotNo, m_duplex);
	if (pos < 0) return;
	char b[32];
	if (m_duplex) { ::snprintf(b, sizeof(b), "%ddBm", rssi);      setField((pos == 0) ? "s1rssi" : "s2rssi", b); }
	else          { ::snprintf(b, sizeof(b), "RSSI %ddBm", rssi); setField("rssi1", b); }
	markDirty();
}

void CScreenCanvas::writeDMRTAInt(unsigned int slotNo, const std::string& talkerAlias)
{
	int pos = dmrPos(slotNo, m_duplex);
	if (pos < 0) return;
	setField(m_duplex ? ((pos == 0) ? "s1ta" : "s2ta") : "ta", talkerAlias);
	markDirty();
}

void CScreenCanvas::writeDMRBERInt(unsigned int slotNo, float ber)
{
	int pos = dmrPos(slotNo, m_duplex);
	if (pos < 0) return;
	char b[32];
	if (m_duplex) { ::snprintf(b, sizeof(b), "%.1f%%", ber);     setField((pos == 0) ? "s1ber" : "s2ber", b); }
	else          { ::snprintf(b, sizeof(b), "BER %.1f%%", ber); setField("ber1", b); }
	markDirty();
}

void CScreenCanvas::clearDMRInt(unsigned int slotNo)
{
	int pos = dmrPos(slotNo, m_duplex);
	if (pos < 0) return;
	if (m_duplex) {
		std::string p = (pos == 0) ? "s1" : "s2";
		setField(p + "src", "Listening");
		m_fields.erase(p + "dst"); m_fields.erase(p + "ta");
		m_fields.erase(p + "rssi"); m_fields.erase(p + "ber");
	} else {
		clearCallFields();
		setField("src", "Listening");
	}
	markDirty();
}

void CScreenCanvas::writeFusionInt(const std::string& source, const std::string& dest, unsigned char dgid, const std::string& type, const std::string& origin)
{
	(void)dest;
	clearCallFields();
	char buf[128];
	::snprintf(buf, sizeof(buf), "%s %s", type.c_str(), source.c_str()); setField("src", buf);
	::snprintf(buf, sizeof(buf), "DG-ID %u", dgid);                      setField("dst", buf);
	setField("info", (origin != "          ") ? ("at " + origin) : "");
	m_mode = MODE_YSF;
	touchActivity();
	markDirty();
}

void CScreenCanvas::writeFusionRSSIInt(int rssi) { writeDStarRSSIInt(rssi); }
void CScreenCanvas::writeFusionBERInt(float ber) { writeDStarBERInt(ber); }
void CScreenCanvas::clearFusionInt() { clearDStarInt(); }

void CScreenCanvas::writeP25Int(const std::string& source, bool group, unsigned int dest, const std::string& type)
{
	clearCallFields();
	char buf[128];
	::snprintf(buf, sizeof(buf), "%s %s", type.c_str(), source.c_str()); setField("src", buf);
	::snprintf(buf, sizeof(buf), "%s%u", group ? "TG" : "", dest);       setField("dst", buf);
	m_mode = MODE_P25;
	touchActivity();
	markDirty();
}

void CScreenCanvas::writeP25RSSIInt(int rssi) { writeDStarRSSIInt(rssi); }
void CScreenCanvas::writeP25BERInt(float ber) { writeDStarBERInt(ber); }
void CScreenCanvas::clearP25Int() { clearDStarInt(); }

void CScreenCanvas::writeNXDNInt(const std::string& source, bool group, unsigned int dest, const std::string& type)
{
	clearCallFields();
	char buf[128];
	::snprintf(buf, sizeof(buf), "%s %s", type.c_str(), source.c_str()); setField("src", buf);
	::snprintf(buf, sizeof(buf), "%s%u", group ? "TG" : "", dest);       setField("dst", buf);
	m_mode = MODE_NXDN;
	touchActivity();
	markDirty();
}

void CScreenCanvas::writeNXDNRSSIInt(int rssi) { writeDStarRSSIInt(rssi); }
void CScreenCanvas::writeNXDNBERInt(float ber) { writeDStarBERInt(ber); }
void CScreenCanvas::clearNXDNInt() { clearDStarInt(); }

void CScreenCanvas::writePOCSAGInt(uint32_t ric, const std::string& message)
{
	clearCallFields();
	char buf[64]; ::snprintf(buf, sizeof(buf), "RIC %u", ric); setField("src", buf);
	setField("dst", message);
	m_mode = MODE_POCSAG;
	touchActivity();
	markDirty();
}

void CScreenCanvas::clearPOCSAGInt() { clearCallFields(); setField("src", "Waiting"); markDirty(); }

void CScreenCanvas::writeFMInt(const std::string& state)
{
	clearCallFields();
	setField("src", state);
	m_mode = MODE_FM;
	touchActivity();
	markDirty();
}

void CScreenCanvas::clearFMInt() { clearDStarInt(); }

void CScreenCanvas::writeCWInt()
{
	m_mode = MODE_CW;
	touchActivity();
	markDirty();
}

void CScreenCanvas::clearCWInt() { setIdleInt(); }

void CScreenCanvas::writeGeneralInt(const std::string& callsign, unsigned int id, bool duplex)
{
	setField("callsign", callsign.empty() ? "--------" : callsign);
	if (id != 0U) { char b[32]; ::snprintf(b, sizeof(b), "ID %u", id); setField("id", b); }
	else setField("id", "");
	m_duplex = duplex;
	markDirty();
}

void CScreenCanvas::writeCPUInt(float temperature, float frequency, float load)
{
	(void)frequency; (void)load;
	m_temperature = temperature;
	updateStatus();
	markDirty();
}

void CScreenCanvas::writeInfoInt(unsigned int rxFrequency, unsigned int txFrequency, const std::string& location)
{
	setField("rx", mhz(rxFrequency));
	setField("tx", mhz(txFrequency));
	m_location = location;
	updateStatus();
	markDirty();
}

void CScreenCanvas::writeIPInt(const std::string& name, const std::string& ipV4, const std::string& ipV6)
{
	(void)ipV6;
	std::string ip = ipV4;
	if (!name.empty()) ip += " (" + name + ")";
	setField("ip", ip);
	markDirty();
}

void CScreenCanvas::clockInt(unsigned int ms)
{
	backendTick();

	m_idleTimer.clock(ms);
	if (m_idleTimeout > 0U && !m_dimmed && m_idleTimer.isRunning() && m_idleTimer.hasExpired()) {
		m_dimmed = true;
		applyBrightness(m_idleBrightness);
	}

	if (m_sinceFlush < FLUSH_INTERVAL_MS)
		m_sinceFlush += ms;
	bool pageChanged = (m_mode != m_lastDrawnMode);
	if (m_dirty && (pageChanged || m_sinceFlush >= FLUSH_INTERVAL_MS)) {
		redraw();
		present();
		m_dirty = false;
		m_sinceFlush = 0U;
		m_lastDrawnMode = m_mode;
	}
}

// ---- Minimal PNG writer (no libpng): one IDAT of stored deflate blocks ----

static uint32_t crc32_png(const uint8_t* p, size_t n, uint32_t crc)
{
	crc = ~crc;
	for (size_t i = 0; i < n; i++) {
		crc ^= p[i];
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320U & (~(crc & 1U) + 1U));
	}
	return ~crc;
}
static void put32(std::vector<uint8_t>& v, uint32_t x)
{
	v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
static void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
	put32(out, (uint32_t)data.size());
	std::vector<uint8_t> td(type, type + 4);
	td.insert(td.end(), data.begin(), data.end());
	out.insert(out.end(), td.begin(), td.end());
	put32(out, crc32_png(td.data(), td.size(), 0));
}

bool CScreenCanvas::dumpPNG(const std::string& path) const
{
	if (m_fb.empty())
		return false;
	const int W = (int)m_width, H = (int)m_height;
	std::vector<uint8_t> raw;
	raw.reserve((size_t)H * (1 + W * 3));
	for (int y = 0; y < H; y++) {
		raw.push_back(0);
		for (int x = 0; x < W; x++) {
			uint16_t p = m_fb[(size_t)y * W + x];
			raw.push_back(((p >> 11) & 0x1F) * 255 / 31);
			raw.push_back(((p >> 5) & 0x3F) * 255 / 63);
			raw.push_back((p & 0x1F) * 255 / 31);
		}
	}
	std::vector<uint8_t> zl;
	zl.push_back(0x78); zl.push_back(0x01);
	size_t pos = 0;
	while (pos < raw.size()) {
		size_t n = raw.size() - pos; if (n > 65535) n = 65535;
		zl.push_back(pos + n >= raw.size() ? 1 : 0);
		zl.push_back(n & 0xFF); zl.push_back((n >> 8) & 0xFF);
		zl.push_back(~n & 0xFF); zl.push_back((~n >> 8) & 0xFF);
		zl.insert(zl.end(), raw.begin() + pos, raw.begin() + pos + n);
		pos += n;
	}
	uint32_t a1 = 1, a2 = 0;
	for (uint8_t b : raw) { a1 = (a1 + b) % 65521; a2 = (a2 + a1) % 65521; }
	put32(zl, (a2 << 16) | a1);

	std::vector<uint8_t> out;
	const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
	out.insert(out.end(), sig, sig + 8);
	std::vector<uint8_t> ihdr;
	put32(ihdr, W); put32(ihdr, H);
	ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
	chunk(out, "IHDR", ihdr);
	chunk(out, "IDAT", zl);
	chunk(out, "IEND", {});

	FILE* fp = ::fopen(path.c_str(), "wb");
	if (fp == nullptr) return false;
	::fwrite(out.data(), 1, out.size(), fp);
	::fclose(fp);
	return true;
}
