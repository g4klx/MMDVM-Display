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


#include "IdLookup.h"
#include "Log.h"

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <sys/stat.h>

// The Mobile Country Codes DMR IDs start with, and their countries
static const struct {
	unsigned int mcc;
	const char*  country;
} MCC_COUNTRIES[] = {
	{202U, "Greece"},
	{204U, "Netherlands"},
	{206U, "Belgium"},
	{208U, "France"},
	{212U, "Monaco"},
	{213U, "Andorra"},
	{214U, "Spain"},
	{216U, "Hungary"},
	{218U, "Bosnia and Herzegovina"},
	{219U, "Croatia"},
	{220U, "Serbia"},
	{221U, "Kosovo"},
	{222U, "Italy"},
	{225U, "Vatican City"},
	{226U, "Romania"},
	{228U, "Switzerland"},
	{230U, "Czech Republic"},
	{231U, "Slovakia"},
	{232U, "Austria"},
	{234U, "United Kingdom"},
	{235U, "United Kingdom"},
	{238U, "Denmark"},
	{240U, "Sweden"},
	{242U, "Norway"},
	{244U, "Finland"},
	{246U, "Lithuania"},
	{247U, "Latvia"},
	{248U, "Estonia"},
	{250U, "Russia"},
	{255U, "Ukraine"},
	{257U, "Belarus"},
	{259U, "Moldova"},
	{260U, "Poland"},
	{262U, "Germany"},
	{266U, "Gibraltar"},
	{268U, "Portugal"},
	{270U, "Luxembourg"},
	{272U, "Ireland"},
	{274U, "Iceland"},
	{276U, "Albania"},
	{278U, "Malta"},
	{280U, "Cyprus"},
	{282U, "Georgia"},
	{283U, "Armenia"},
	{284U, "Bulgaria"},
	{286U, "Turkey"},
	{288U, "Faroe Islands"},
	{289U, "Georgia"},
	{290U, "Greenland"},
	{292U, "San Marino"},
	{293U, "Slovenia"},
	{294U, "North Macedonia"},
	{295U, "Liechtenstein"},
	{297U, "Montenegro"},
	{302U, "Canada"},
	{308U, "Saint Pierre and Miquelon"},
	{310U, "United States"},
	{311U, "United States"},
	{312U, "United States"},
	{313U, "United States"},
	{314U, "United States"},
	{315U, "United States"},
	{316U, "United States"},
	{330U, "Puerto Rico"},
	{334U, "Mexico"},
	{338U, "Jamaica"},
	{340U, "French Guiana"},
	{342U, "Barbados"},
	{344U, "Antigua and Barbuda"},
	{346U, "Cayman Islands"},
	{348U, "Virgin Islands, British"},
	{350U, "Bermuda"},
	{352U, "Grenada"},
	{354U, "Montserrat"},
	{356U, "Saint Kitts and Nevis"},
	{358U, "Saint Lucia"},
	{360U, "Saint Vincent and the Grenadines"},
	{362U, "Netherlands Antilles"},
	{363U, "Aruba"},
	{364U, "Bahamas"},
	{365U, "Anguilla"},
	{366U, "Dominica"},
	{368U, "Cuba"},
	{370U, "Dominican Republic"},
	{372U, "Haiti"},
	{374U, "Trinidad and Tobago"},
	{376U, "Turks and Caicos Islands"},
	{400U, "Azerbaijan"},
	{401U, "Kazakhstan"},
	{402U, "Bhutan"},
	{404U, "India"},
	{405U, "India"},
	{410U, "Pakistan"},
	{412U, "Afghanistan"},
	{413U, "Sri Lanka"},
	{414U, "Myanmar"},
	{415U, "Lebanon"},
	{416U, "Jordan"},
	{417U, "Syria"},
	{418U, "Iraq"},
	{419U, "Kuwait"},
	{420U, "Saudi Arabia"},
	{421U, "Yemen"},
	{422U, "Oman"},
	{424U, "United Arab Emirates"},
	{425U, "Israel"},
	{426U, "Bahrain"},
	{427U, "Qatar"},
	{428U, "Mongolia"},
	{429U, "Nepal"},
	{430U, "United Arab Emirates"},
	{431U, "United Arab Emirates"},
	{432U, "Iran"},
	{434U, "Uzbekistan"},
	{436U, "Tajikistan"},
	{437U, "Kyrgyzstan"},
	{438U, "Turkmenistan"},
	{440U, "Japan"},
	{441U, "Japan"},
	{450U, "South Korea"},
	{452U, "Vietnam"},
	{454U, "Hong Kong"},
	{455U, "Macao"},
	{456U, "Cambodia"},
	{457U, "Laos"},
	{460U, "China"},
	{466U, "Taiwan"},
	{467U, "North Korea"},
	{470U, "Bangladesh"},
	{472U, "Maldives"},
	{502U, "Malaysia"},
	{505U, "Australia"},
	{510U, "Indonesia"},
	{514U, "Timor-Leste"},
	{515U, "Philippines"},
	{520U, "Thailand"},
	{525U, "Singapore"},
	{528U, "Brunei"},
	{530U, "New Zealand"},
	{537U, "Papua New Guinea"},
	{539U, "Tonga"},
	{540U, "Solomon Islands"},
	{541U, "Vanuatu"},
	{542U, "Fiji"},
	{543U, "Wallis and Futuna"},
	{544U, "American Samoa"},
	{545U, "Kiribati"},
	{546U, "New Caledonia"},
	{547U, "French Polynesia"},
	{548U, "Cook Islands"},
	{549U, "Samoa"},
	{550U, "Micronesia"},
	{551U, "Marshall Islands"},
	{552U, "Palau"},
	{553U, "Tuvalu"},
	{555U, "Niue"},
	{602U, "Egypt"},
	{603U, "Algeria"},
	{604U, "Morocco"},
	{605U, "Tunisia"},
	{606U, "Libya"},
	{607U, "Gambia"},
	{608U, "Senegal"},
	{609U, "Mauritania"},
	{610U, "Mali"},
	{611U, "Guinea"},
	{612U, "Côte d'Ivoire"},
	{613U, "Burkina Faso"},
	{614U, "Niger"},
	{615U, "Togo"},
	{616U, "Benin"},
	{617U, "Mauritius"},
	{618U, "Liberia"},
	{619U, "Sierra Leone"},
	{620U, "Ghana"},
	{621U, "Nigeria"},
	{622U, "Chad"},
	{623U, "Central African Republic"},
	{624U, "Cameroon"},
	{625U, "Cape Verde"},
	{626U, "Sao Tome and Principe"},
	{627U, "Equatorial Guinea"},
	{628U, "Gabon"},
	{629U, "Congo"},
	{630U, "DR Congo"},
	{631U, "Angola"},
	{632U, "Guinea-Bissau"},
	{633U, "Seychelles"},
	{634U, "Sudan"},
	{635U, "Rwanda"},
	{636U, "Ethiopia"},
	{637U, "Somalia"},
	{638U, "Djibouti"},
	{639U, "Kenya"},
	{640U, "Tanzania"},
	{641U, "Uganda"},
	{642U, "Burundi"},
	{643U, "Mozambique"},
	{645U, "Zambia"},
	{646U, "Madagascar"},
	{647U, "Réunion"},
	{648U, "Zimbabwe"},
	{649U, "Namibia"},
	{650U, "Malawi"},
	{651U, "Lesotho"},
	{652U, "Botswana"},
	{653U, "Eswatini"},
	{654U, "Comoros"},
	{655U, "South Africa"},
	{657U, "Eritrea"},
	{658U, "Saint Helena, Ascension and Tristan da Cunha"},
	{659U, "South Sudan"},
	{702U, "Belize"},
	{704U, "Guatemala"},
	{706U, "El Salvador"},
	{708U, "Honduras"},
	{710U, "Nicaragua"},
	{712U, "Costa Rica"},
	{714U, "Panama"},
	{716U, "Peru"},
	{722U, "Argentina"},
	{724U, "Brazil"},
	{730U, "Chile"},
	{732U, "Colombia"},
	{734U, "Venezuela"},
	{736U, "Bolivia"},
	{738U, "Guyana"},
	{740U, "Ecuador"},
	{744U, "Paraguay"},
	{746U, "Suriname"},
	{748U, "Uruguay"},
	{750U, "Falkland Islands (Malvinas)"},
};

// A line, without its end; false at the end of the file. A line longer
// than the buffer is read whole.
static bool readLine(FILE* fp, std::string& line)
{
	assert(fp != nullptr);

	line.clear();

	char buffer[256U];
	while (::fgets(buffer, sizeof(buffer), fp) != nullptr) {
		line += buffer;
		if (!line.empty() && (line.back() == '\n'))
			break;
	}

	if (line.empty())
		return false;

	while (!line.empty() && ((line.back() == '\n') || (line.back() == '\r')))
		line.pop_back();

	return true;
}

static std::string trim(const std::string& text)
{
	std::string::size_type start = text.find_first_not_of(" \t\"");
	if (start == std::string::npos)
		return std::string();

	std::string::size_type end = text.find_last_not_of(" \t\"");
	return text.substr(start, end - start + 1U);
}

// The ID a line starts with; false when it starts with none
static bool lineId(const std::string& line, unsigned int& id)
{
	const char* p = line.c_str();
	while ((*p == ' ') || (*p == '\t') || (*p == '"'))
		p++;

	if (!::isdigit((unsigned char)*p))
		return false;

	char* end = nullptr;
	unsigned long n = ::strtoul(p, &end, 10);
	if (n > 0xFFFFFFFFUL)
		return false;

	id = (unsigned int)n;
	return true;
}

CIdLookup::CIdLookup(const std::string& file) :
m_file(file),
m_mtime(0),
m_size(-1L),
m_ok(false),
m_csv(false),
m_sorted(false),
m_start(0L),
m_columns()
{
}

CIdLookup::~CIdLookup()
{
}

std::string CIdLookup::countryOf(unsigned int id)
{
	// A user (7 digits), repeater or hotspot (6, or 8-9 with an ESSID) ID
	// starts with its MCC
	char text[20U];
	::sprintf(text, "%u", id);

	size_t length = ::strlen(text);
	if ((length < 6U) || (length > 9U))
		return std::string();

	unsigned int mcc = (unsigned int)((text[0U] - '0') * 100 + (text[1U] - '0') * 10 + (text[2U] - '0'));

	unsigned int lo = 0U;
	unsigned int hi = sizeof(MCC_COUNTRIES) / sizeof(MCC_COUNTRIES[0U]);
	while (lo < hi) {
		unsigned int mid = (lo + hi) / 2U;
		if (MCC_COUNTRIES[mid].mcc == mcc)
			return MCC_COUNTRIES[mid].country;
		if (MCC_COUNTRIES[mid].mcc < mcc)
			lo = mid + 1U;
		else
			hi = mid;
	}

	return std::string();
}

// Looks at the list when it has changed: its format, and whether it is
// sorted (read through once).
bool CIdLookup::check()
{
	struct stat st;
	if (::stat(m_file.c_str(), &st) != 0) {
		if (m_size != -2L)
			LogWarning("Cannot read the ID list %s", m_file.c_str());
		m_size = -2L;
		m_ok   = false;
		return false;
	}

	if ((st.st_mtime == m_mtime) && (long(st.st_size) == m_size))
		return m_ok;

	m_mtime = st.st_mtime;
	m_size  = long(st.st_size);
	m_ok    = false;

	// Binary mode throughout: the offsets are bytes
	FILE* fp = ::fopen(m_file.c_str(), "rb");
	if (fp == nullptr) {
		LogWarning("Cannot read the ID list %s", m_file.c_str());
		return false;
	}

	m_csv   = false;
	m_start = 0L;
	for (unsigned int i = 0U; i < COL_COUNT; i++)
		m_columns[i] = -1;

	// RadioID's header names its columns
	std::string line;
	if (readLine(fp, line)) {
		std::string upper;
		for (char c : line)
			upper += char(::toupper((unsigned char)c));

		if (upper.find("RADIO_ID") != std::string::npos) {
			static const char* NAMES[COL_COUNT] = {"RADIO_ID", "CALLSIGN", "FIRST_NAME", "LAST_NAME", "CITY", "STATE", "COUNTRY"};

			m_csv = true;
			int column = 0;
			std::string::size_type pos = 0U;
			while (pos <= upper.size()) {
				std::string::size_type comma = upper.find(',', pos);
				if (comma == std::string::npos)
					comma = upper.size();

				const std::string name = trim(upper.substr(pos, comma - pos));
				for (unsigned int i = 0U; i < COL_COUNT; i++) {
					if (name == NAMES[i])
						m_columns[i] = column;
				}

				column++;
				pos = comma + 1U;
			}

			m_start = ::ftell(fp);
		} else if (line.find(',') != std::string::npos) {
			// Comma separated without a header: RadioID's columns
			m_csv = true;
			for (unsigned int i = 0U; i < COL_COUNT; i++)
				m_columns[i] = int(i);
		}
	}

	if (m_csv && ((m_columns[COL_ID] < 0) || (m_columns[COL_CALLSIGN] < 0))) {
		LogWarning("The ID list %s has no RADIO_ID or CALLSIGN column", m_file.c_str());
		::fclose(fp);
		return false;
	}

	// Sorted by ID?
	::fseek(fp, m_start, SEEK_SET);
	m_sorted = true;
	unsigned int count = 0U;
	unsigned int last  = 0U;
	while (readLine(fp, line)) {
		unsigned int id;
		if (!lineId(line, id))
			continue;

		if (id < last)
			m_sorted = false;

		last = id;
		count++;
	}

	::fclose(fp);

	LogMessage("ID list %s: %u stations%s", m_file.c_str(), count, m_sorted ? "" : ", not sorted (each search reads it through)");

	m_ok = true;
	return true;
}

bool CIdLookup::parse(const std::string& line, unsigned int& id, CIdEntry* entry) const
{
	if (!lineId(line, id))
		return false;

	if (entry == nullptr)
		return true;

	*entry = CIdEntry();

	if (m_csv) {
		// Comma separated, a field in quotes may hold commas
		std::vector<std::string> fields;
		std::string field;
		bool quoted = false;
		for (char c : line) {
			if (c == '"')
				quoted = !quoted;
			else if ((c == ',') && !quoted) {
				fields.push_back(trim(field));
				field.clear();
			} else
				field += c;
		}
		fields.push_back(trim(field));

		auto column = [&](int index) -> std::string {
			int i = m_columns[index];
			return ((i >= 0) && (i < int(fields.size()))) ? fields[i] : std::string();
		};

		entry->callsign = column(COL_CALLSIGN);
		entry->name     = column(COL_FIRST);
		std::string last = column(COL_LAST);
		if (!last.empty())
			entry->name += entry->name.empty() ? last : " " + last;
		entry->city     = column(COL_CITY);
		entry->state    = column(COL_STATE);
		entry->country  = column(COL_COUNTRY);
	} else {
		// "<id> <callsign> <name>", by tabs or spaces; the name may hold spaces
		std::string::size_type pos = line.find_first_not_of(" \t");
		pos = line.find_first_of(" \t", pos);
		pos = line.find_first_not_of(" \t", pos);
		if (pos == std::string::npos)
			return true;

		std::string::size_type end = line.find_first_of(" \t", pos);
		entry->callsign = line.substr(pos, end - pos);
		if (end != std::string::npos)
			entry->name = trim(line.substr(end));
	}

	return true;
}

// By halves of the file: lines starting in [lo, hi) are searched.
bool CIdLookup::findSorted(FILE* fp, unsigned int id, CIdEntry& entry) const
{
	long lo = m_start;
	long hi = m_size;
	std::string line;

	while (lo < hi) {
		long mid = lo + (hi - lo) / 2L;

		// The first line starting at or after mid
		long pos = mid;
		if (mid > lo) {
			::fseek(fp, mid - 1L, SEEK_SET);
			int c;
			while (((c = ::fgetc(fp)) != EOF) && (c != '\n'))
				;
			pos = ::ftell(fp);
		} else {
			::fseek(fp, mid, SEEK_SET);
		}

		if ((pos >= hi) || !readLine(fp, line)) {
			hi = mid;
			continue;
		}
		long next = ::ftell(fp);

		unsigned int lineID;
		if (!lineId(line, lineID) || (lineID < id)) {
			lo = next;
		} else if (lineID > id) {
			hi = mid;
		} else {
			unsigned int dummy;
			return parse(line, dummy, &entry);
		}
	}

	return false;
}

bool CIdLookup::findAll(FILE* fp, unsigned int id, CIdEntry& entry) const
{
	::fseek(fp, m_start, SEEK_SET);

	std::string line;
	while (readLine(fp, line)) {
		unsigned int lineID;
		if (lineId(line, lineID) && (lineID == id)) {
			unsigned int dummy;
			return parse(line, dummy, &entry);
		}
	}

	return false;
}

bool CIdLookup::find(unsigned int id, CIdEntry& entry)
{
	if (!check())
		return false;

	FILE* fp = ::fopen(m_file.c_str(), "rb");
	if (fp == nullptr)
		return false;

	bool found = m_sorted ? findSorted(fp, id, entry) : findAll(fp, id, entry);

	::fclose(fp);

	return found;
}
