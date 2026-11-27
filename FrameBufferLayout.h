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

// A community-editable screen layout, loaded from a plain-text file (or the
// built-in default). It is organised like a Nextion project: named "pages"
// (idle, call, dmr2, error, ...) each holding a list of "elements" that bind
// named data fields to a position and style. The renderer (CFrameBufferCanvas) walks
// the active page and draws each element, substituting live field values.
//
// Format (one element per line; blank lines and # comments ignored):
//
//   [page idle]
//   wordmark
//   text "$callsign"   x=center y=108 size=L align=center color=ink fit=fill
//   text "IP"          x=16 y=168 size=S color=grey
//   text "$ip"         x=74 y=168 size=S color=ink fit=100%-90
//   rule               x=16 y=162 w=fill color=grey
//
// Coordinates/lengths are in a 240-high design space; the width tracks the
// screen's aspect. A value may be a number, a percentage of the dimension,
// the keywords center/right/bottom, "fill" (the content width), or a sum of
// those, e.g. 50%-82 or 100%-16.

#if !defined(FRAMEBUFFERLAYOUT_H)
#define FRAMEBUFFERLAYOUT_H

#include <string>
#include <vector>
#include <map>

struct LElement {
	std::string kind;    // text | banner | wordmark | rule | rect | image
	std::string text;    // "text" template ($field + literals), or "image" file path
	std::string x, y, w, h;
	std::string size, align, color, fit, radius;
};

class CFrameBufferLayout {
public:
	CFrameBufferLayout();

	// Load from a file; on any failure fall back to the built-in default.
	// Returns true if the named file was used, false if the default was used.
	bool load(const std::string& path);
	void loadDefault();

	// Returns the named page from the loaded layout, falling back to the
	// built-in default page of the same name (so an overlay need only define
	// the pages it wants to change).
	const std::vector<LElement>* page(const std::string& name) const;

private:
	bool parseInto(const std::string& text, std::map<std::string, std::vector<LElement> >& out) const;
	std::map<std::string, std::vector<LElement> > m_pages;      // active (custom or default)
	std::map<std::string, std::vector<LElement> > m_defaults;   // built-in fallback
};

#endif
