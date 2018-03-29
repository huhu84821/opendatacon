#include "MD3.h"
#include "CRC.h"

static const uint8_t fcstab[256] =
{
	0x00, 0x6c, 0xd8, 0xb4, 0xdc, 0xb0, 0x04, 0x68, 0xd4, 0xb8, 0x0c, 0x60, 0x08, 0x64, 0xd0, 0xbc,
	0xc4, 0xa8, 0x1c, 0x70, 0x18, 0x74, 0xc0, 0xac, 0x10, 0x7c, 0xc8, 0xa4, 0xcc, 0xa0, 0x14, 0x78,
	0xe4, 0x88, 0x3c, 0x50, 0x38, 0x54, 0xe0, 0x8c, 0x30, 0x5c, 0xe8, 0x84, 0xec, 0x80, 0x34, 0x58,
	0x20, 0x4c, 0xf8, 0x94, 0xfc, 0x90, 0x24, 0x48, 0xf4, 0x98, 0x2c, 0x40, 0x28, 0x44, 0xf0, 0x9c,
	0xa4, 0xc8, 0x7c, 0x10, 0x78, 0x14, 0xa0, 0xcc, 0x70, 0x1c, 0xa8, 0xc4, 0xac, 0xc0, 0x74, 0x18,
	0x60, 0x0c, 0xb8, 0xd4, 0xbc, 0xd0, 0x64, 0x08, 0xb4, 0xd8, 0x6c, 0x00, 0x68, 0x04, 0xb0, 0xdc,
	0x40, 0x2c, 0x98, 0xf4, 0x9c, 0xf0, 0x44, 0x28, 0x94, 0xf8, 0x4c, 0x20, 0x48, 0x24, 0x90, 0xfc,
	0x84, 0xe8, 0x5c, 0x30, 0x58, 0x34, 0x80, 0xec, 0x50, 0x3c, 0x88, 0xe4, 0x8c, 0xe0, 0x54, 0x38,
	0x24, 0x48, 0xfc, 0x90, 0xf8, 0x94, 0x20, 0x4c, 0xf0, 0x9c, 0x28, 0x44, 0x2c, 0x40, 0xf4, 0x98,
	0xe0, 0x8c, 0x38, 0x54, 0x3c, 0x50, 0xe4, 0x88, 0x34, 0x58, 0xec, 0x80, 0xe8, 0x84, 0x30, 0x5c,
	0xc0, 0xac, 0x18, 0x74, 0x1c, 0x70, 0xc4, 0xa8, 0x14, 0x78, 0xcc, 0xa0, 0xc8, 0xa4, 0x10, 0x7c,
	0x04, 0x68, 0xdc, 0xb0, 0xd8, 0xb4, 0x00, 0x6c, 0xd0, 0xbc, 0x08, 0x64, 0x0c, 0x60, 0xd4, 0xb8,
	0x80, 0xec, 0x58, 0x34, 0x5c, 0x30, 0x84, 0xe8, 0x54, 0x38, 0x8c, 0xe0, 0x88, 0xe4, 0x50, 0x3c,
	0x44, 0x28, 0x9c, 0xf0, 0x98, 0xf4, 0x40, 0x2c, 0x90, 0xfc, 0x48, 0x24, 0x4c, 0x20, 0x94, 0xf8,
	0x64, 0x08, 0xbc, 0xd0, 0xb8, 0xd4, 0x60, 0x0c, 0xb0, 0xdc, 0x68, 0x04, 0x6c, 0x00, 0xb4, 0xd8,
	0xa0, 0xcc, 0x78, 0x14, 0x7c, 0x10, 0xa4, 0xc8, 0x74, 0x18, 0xac, 0xc0, 0xa8, 0xc4, 0x70, 0x1c
};

bool MD3CRCCompare(const uint8_t crc1, const uint8_t crc2)
{
	// Just make sure we only compare the 6 bits!
	return ((crc1 & 0x3f) == (crc2 & 0x3f));
}

uint8_t MD3CRC(const uint32_t data)
{
	uint8_t CRCChar = fcstab[(data >> 24) & 0x0ff];
	CRCChar = fcstab[((data >> 16) ^ CRCChar) & 0xff];
	CRCChar = fcstab[((data >> 8) ^ CRCChar) & 0xff];
	CRCChar = fcstab[(data  ^ CRCChar) & 0xff];

	CRCChar = ~(CRCChar >> 2) & 0x3F;

	return CRCChar;
}