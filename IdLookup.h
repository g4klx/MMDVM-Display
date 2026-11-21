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


#if !defined(IDLOOKUP_H)
#define	IDLOOKUP_H

#include <cstdio>
#include <ctime>
#include <string>

// A station as an ID list gives it; fields the list does not have are empty.
class CIdEntry {
public:
	std::string callsign;
	std::string name;
	std::string city;
	std::string state;
	std::string country;
};

// Finds a station by its ID in an ID list on disk, when it is needed: the
// list is not read into memory. A list sorted by ID (as RadioID's and
// Pi-Star's are) is searched by halves, a few short reads that the
// operating system's file cache serves; any other is read through.
//
// The lists: "<id> <callsign> <name>" a line, separated by tabs or spaces
// (DMRIds.dat), or comma separated with RadioID's header (user.csv,
// NXDN.csv: RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY).
class CIdLookup {
public:
	CIdLookup(const std::string& file);
	~CIdLookup();

	bool find(unsigned int id, CIdEntry& entry);

	// The country of a DMR ID, by the Mobile Country Code it starts with
	// ("" for IDs that carry none)
	static std::string countryOf(unsigned int id);

private:
	enum { COL_ID, COL_CALLSIGN, COL_FIRST, COL_LAST, COL_CITY, COL_STATE, COL_COUNTRY, COL_COUNT };

	std::string m_file;
	time_t      m_mtime;
	long        m_size;
	bool        m_ok;
	bool        m_csv;
	bool        m_sorted;
	long        m_start;		// where the first station's line starts
	int         m_columns[COL_COUNT];

	bool check();
	bool parse(const std::string& line, unsigned int& id, CIdEntry* entry) const;
	bool findSorted(FILE* fp, unsigned int id, CIdEntry& entry) const;
	bool findAll(FILE* fp, unsigned int id, CIdEntry& entry) const;
};

#endif
