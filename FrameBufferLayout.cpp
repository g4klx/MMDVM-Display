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

#include "FrameBufferLayout.h"
#include "Log.h"

#include <cstdio>
#include <cstring>

// The built-in default layout. It reproduces the standard design and doubles
// as the worked reference a community author copies and edits. Fields begin
// with '$'; coordinates are in a 240-high design space (width tracks aspect).
static const char* DEFAULT_LAYOUT = R"LAYOUT(
# =====================================================================
#  MMDVM-Display - default overlay
# =====================================================================
#  This is the full, built-in layout, provided as an editable file so you
#  can copy it and make your own. It is what the binary uses when no overlay
#  is configured.
#
#  Install: put this file (or your edited copy) in your OverlayDir and select
#  it in MMDVM-Display.ini:
#
#      [FrameBuffer]
#      OverlayDir=/etc/mmdvm-display/overlays
#      Overlay=default
#
#  See README.md in this folder for the full reference. Quick notes:
#    * Coordinates are a 240-high design space; width tracks the screen's
#      aspect ratio. Use plain numbers for y (and fixed left margins), and
#      % / center / right / fill for anything that should span the width.
#    * y is the VERTICAL CENTRE of the text. align sets the horizontal anchor:
#      left (x = left edge), center (x = centre), right (x = right edge).
#    * $name is a live field (see the field list in README). An element whose
#      field is empty is simply not drawn.
#    * Colours: ink grey white accent green red blue. Sizes: S M L or a number.
# =====================================================================

# ---- Idle: the resting screen -------------------------------------------
[page idle]
wordmark
text "$callsign"  x=center y=119 size=L align=center color=ink fit=fill
text "$id"        x=center y=148 size=S align=center color=grey
rule              x=16 y=163 w=fill color=grey
text "IP"         x=16 y=176 size=S color=grey
text "$ip"        x=74 y=176 size=S color=ink fit=100%-90
text "RX"         x=16 y=198 size=S color=grey
text "$rx"        x=74 y=198 size=S color=ink fit=50%-82
text "TX"         x=50% y=198 size=S color=grey
text "$tx"        x=50%+58 y=198 size=S color=ink fit=50%-74
text "$status"    x=center y=220 size=S align=center color=grey fit=fill

# ---- Call: every single-stream mode (the banner adapts to the mode) -----
#  D-Star, DMR simplex, System Fusion, P25, NXDN, POCSAG and FM.
[page call]
banner
text "$src"   x=16 y=122 size=M color=ink   fit=100%-32
text "$dst"   x=16 y=158 size=M color=ink   fit=100%-32
text "$info"  x=16 y=194 size=M color=ink   fit=100%-32
text "$ta"    x=16 y=194 size=M color=green fit=100%-32
text "$rssi1" x=16      y=bottom-14 size=S color=grey fit=50%-24
text "$ber1"  x=100%-16 y=bottom-14 size=S align=right color=grey

# ---- DMR duplex: both timeslots side by side ----------------------------
[page dmr2]
banner
rect           x=50% y=112 w=1 h=104 color=grey
text "TS1"     x=16 y=116 size=S color=blue
text "$s1src"  x=16 y=138 size=M color=ink   fit=50%-24
text "$s1dst"  x=16 y=172 size=S color=ink   fit=50%-24
text "$s1ta"   x=16 y=196 size=S color=green fit=50%-24
text "$s1rssi" x=16     y=bottom-14 size=S color=grey
text "$s1ber"  x=50%-12 y=bottom-14 size=S align=right color=grey
text "TS2"     x=50%+12 y=116 size=S color=blue
text "$s2src"  x=50%+12 y=138 size=M color=ink   fit=50%-36
text "$s2dst"  x=50%+12 y=172 size=S color=ink   fit=50%-36
text "$s2ta"   x=50%+12 y=196 size=S color=green fit=50%-36
text "$s2rssi" x=50%+12  y=bottom-14 size=S color=grey
text "$s2ber"  x=100%-12 y=bottom-14 size=S align=right color=grey

# ---- Status pages -------------------------------------------------------
[page error]
wordmark
text "ERROR"   x=center y=141 size=L align=center color=red

[page lockout]
wordmark
text "LOCKOUT" x=center y=141 size=L align=center color=red

[page quit]
wordmark
text "STOPPED" x=center y=141 size=L align=center color=grey

[page cw]
wordmark
text "CW ID"   x=center y=141 size=L align=center color=ink
)LAYOUT";

CFrameBufferLayout::CFrameBufferLayout()
{
}

// Split a line into whitespace-separated tokens; a "quoted string" is one
// token with the quotes stripped.
static std::vector<std::string> tokenize(const std::string& line)
{
	std::vector<std::string> out;
	size_t i = 0, n = line.size();
	while (i < n) {
		while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
		if (i >= n) break;
		if (line[i] == '"') {
			size_t j = ++i;
			while (j < n && line[j] != '"') j++;
			out.push_back(line.substr(i, j - i));
			i = (j < n) ? j + 1 : j;
		} else {
			size_t j = i;
			while (j < n && line[j] != ' ' && line[j] != '\t') j++;
			out.push_back(line.substr(i, j - i));
			i = j;
		}
	}
	return out;
}

static void setAttr(LElement& e, const std::string& key, const std::string& val)
{
	if      (key == "x")      e.x = val;
	else if (key == "y")      e.y = val;
	else if (key == "w")      e.w = val;
	else if (key == "h")      e.h = val;
	else if (key == "size")   e.size = val;
	else if (key == "align")  e.align = val;
	else if (key == "color")  e.color = val;
	else if (key == "colour") e.color = val;
	else if (key == "fit")    e.fit = val;
	else if (key == "r" || key == "radius") e.radius = val;
}

bool CFrameBufferLayout::parseInto(const std::string& text, std::map<std::string, std::vector<LElement> >& out) const
{
	out.clear();
	std::string cur;                 // current page name
	size_t i = 0, n = text.size();
	while (i < n) {
		size_t e = text.find('\n', i);
		std::string line = text.substr(i, (e == std::string::npos ? n : e) - i);
		i = (e == std::string::npos) ? n : e + 1;

		// trim leading/trailing whitespace and CR
		size_t a = line.find_first_not_of(" \t\r");
		if (a == std::string::npos) continue;
		size_t b = line.find_last_not_of(" \t\r");
		line = line.substr(a, b - a + 1);
		if (line.empty() || line[0] == '#') continue;

		if (line[0] == '[') {
			// [page name]
			std::vector<std::string> t = tokenize(line.substr(1, line.size() - 2));
			if (t.size() >= 2 && t[0] == "page") { cur = t[1]; out[cur]; }
			continue;
		}
		if (cur.empty()) continue;

		std::vector<std::string> t = tokenize(line);
		if (t.empty()) continue;
		LElement el;
		el.kind = t[0];
		size_t k = 1;
		if (el.kind == "text" && k < t.size() && line.find('"') != std::string::npos) {
			el.text = t[k]; k++;
		} else if (el.kind == "image" && k < t.size() && t[k].find('=') == std::string::npos) {
			el.text = t[k]; k++;      // image "logo.png" (quoted or bare, no '=')
		}
		for (; k < t.size(); k++) {
			size_t eq = t[k].find('=');
			if (eq == std::string::npos) continue;
			setAttr(el, t[k].substr(0, eq), t[k].substr(eq + 1));
		}
		out[cur].push_back(el);
	}
	return !out.empty();
}

void CFrameBufferLayout::loadDefault()
{
	parseInto(DEFAULT_LAYOUT, m_pages);
	m_defaults = m_pages;
}

bool CFrameBufferLayout::load(const std::string& path)
{
	if (!path.empty() && path != "default") {
		FILE* fp = ::fopen(path.c_str(), "rb");
		if (fp != nullptr) {
			std::string text;
			char buf[4096];
			size_t r;
			while ((r = ::fread(buf, 1, sizeof(buf), fp)) > 0)
				text.append(buf, r);
			::fclose(fp);
			if (parseInto(text, m_pages)) {
				parseInto(DEFAULT_LAYOUT, m_defaults);
				LogMessage("FrameBuffer: loaded layout %s (%zu pages; missing pages fall back to default)", path.c_str(), m_pages.size());
				return true;
			}
			LogError("FrameBuffer: layout %s is empty or invalid; using the default", path.c_str());
		} else {
			LogError("FrameBuffer: cannot open layout %s; using the default", path.c_str());
		}
	}
	loadDefault();
	return false;
}

const std::vector<LElement>* CFrameBufferLayout::page(const std::string& name) const
{
	auto it = m_pages.find(name);
	if (it != m_pages.end()) return &it->second;
	auto id = m_defaults.find(name);
	return (id == m_defaults.end()) ? nullptr : &id->second;
}
