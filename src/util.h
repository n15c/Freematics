// Pure helper functions without hardware dependencies (host unit tested).
#pragma once
#include <stdint.h>
#include <stddef.h>

// GNSS date (ddmmyy) and time (hhmmsscc) to unix time; 0 if invalid
uint32_t gnssToUnix(uint32_t date, uint32_t time);
// great circle distance
double distanceMeters(double lat1, double lon1, double lat2, double lon2);

int hexVal(char c);
bool isHexStr(const char* s, int len);
uint32_t parseHex(const char* s, int len);

// compact, URL-safe number formatting (no exponent, no '+')
int fmtNum(char* buf, size_t size, float v);
// append "&key=value" (key sanitised to [A-Za-z0-9_.-])
void appendKV(char* buf, size_t size, const char* key, float v);

// Extract a signal from bytes.
//  bigEndian: bit 0 = MSB of byte 0, read MSB first (UDS style, byte n -> n*8)
//  little endian: Intel/DBC, startBit = LSB position (byte n bit b -> n*8+b)
bool extractBits(const uint8_t* data, int len, uint16_t startBit, uint8_t bitLen, bool bigEndian, bool isSigned, int64_t& out);

// Parse an ELM style response to a request like "22F40D" or "010C" and return
// the data bytes after the positive response header (SID+0x40, echoed PID/DID).
// Handles single frames and ISO-TP multi line output ("0: 62 F4 0D ..."). buf is modified.
bool parseRequestResponse(char* buf, const char* request, uint8_t* out, int outSize, int& outLen);

// Parse one line of CAN monitor output (ATM1 with headers on).
// Supports "7E8 03 41 0D 00", "18DAF110 03 41 ...", "18 DA F1 10 03 41 ...",
// "7E803410D00" and "$7E8,03,41,0D,00". line is modified.
bool parseSniffLine(char* line, bool ext29, uint32_t& id, uint8_t* data, uint8_t& len);
