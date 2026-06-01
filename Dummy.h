/*
 *   Copyright (C) 2026 by Jonathan Naylor G4KLX
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

#if !defined(DUMMY_H)
#define DUMMY_H

#include "Display.h"

#include <string>

class CDummy : public CDisplay
{
public:
	CDummy();
	virtual ~CDummy();

	virtual bool open() override;

	virtual void close() override;

protected:
	virtual void setIdleInt() override;
	virtual void setErrorInt() override;
	virtual void setLockoutInt() override;
	virtual void setQuitInt() override;

	virtual void writeDStarInt(const std::string& my1, const std::string& my2, const std::string& your, const std::string& type, const std::string& reflector) override;
	virtual void clearDStarInt() override;

	virtual void writeDMRInt(unsigned int slotNo, const std::string& src, bool group, unsigned int dst, const std::string& type) override;
	virtual void clearDMRInt(unsigned int slotNo) override;

	virtual void writeFusionInt(const std::string& source, const std::string& dest, unsigned char dgid, const std::string& type, const std::string& origin) override;
	virtual void clearFusionInt() override;

	virtual void writeP25Int(const std::string& source, bool group, unsigned int dest, const std::string& type) override;
	virtual void clearP25Int() override;

	virtual void writeNXDNInt(const std::string& source, bool group, unsigned int dest, const std::string& type) override;
	virtual void clearNXDNInt() override;

	virtual void writePOCSAGInt(uint32_t ric, const std::string& message) override;
	virtual void clearPOCSAGInt() override;

	virtual void writeFMInt(const std::string& state) override;
	virtual void clearFMInt() override;

	virtual void writeCWInt() override;
	virtual void clearCWInt() override;

	virtual void clockInt(unsigned int ms) override;

private:
};

#endif
