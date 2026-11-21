// ISO 8601 formatting of datetimes and timestamps.
/*
 * Copyright © 2026 Germán Méndez Bravo (Kronuz)
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "datetime.h"

#include <bit>                                    // for std::bit_cast
#include <cstring>                                // for std::memcpy
#include <cstdint>                                // for std::int64_t
#include <format>                                 // for std::format
#include <string>                                 // for std::string




/*
 * Return a string with the datetime in ISO 8601 Format.
 */
static inline char*
put_int(char* o, std::int64_t v, int width)
{
	// Same output as std::format("{:0<width>}", v): the sign counts toward the width.
	if (v >= 0 && v < 100 && width == 2) {
		*o++ = static_cast<char>('0' + v / 10);
		*o++ = static_cast<char>('0' + v % 10);
		return o;
	}
	std::uint64_t u;
	if (v < 0) {
		*o++ = '-';
		--width;
		u = 0 - static_cast<std::uint64_t>(v);
	} else {
		u = static_cast<std::uint64_t>(v);
	}
	char tmp[24];
	int n = 0;
	do {
		tmp[n++] = static_cast<char>('0' + u % 10);
		u /= 10;
	} while (u);
	while (n < width) { tmp[n++] = '0'; }
	while (n) { *o++ = tmp[--n]; }
	return o;
}

static inline char*
put_date_time(char* o, std::int64_t year, int mon, int day, char sep, int hour, int min, int sec)
{
	o = put_int(o, year, 4);
	*o++ = '-';
	o = put_int(o, mon, 2);
	*o++ = '-';
	o = put_int(o, day, 2);
	*o++ = sep;
	o = put_int(o, hour, 2);
	*o++ = ':';
	o = put_int(o, min, 2);
	*o++ = ':';
	return put_int(o, sec, 2);
}

std::string
Datetime::iso8601(const std::tm& tm, bool trim, char sep)
{
	char buf[160];
	char* o = put_date_time(buf, std::int64_t{tm.tm_year} + DATETIME_START_YEAR, tm.tm_mon + 1, tm.tm_mday, sep, tm.tm_hour, tm.tm_min, tm.tm_sec);
	if (!trim) {
		std::memcpy(o, ".000000", 7);
		o += 7;
	}
	return std::string(buf, static_cast<std::size_t>(o - buf));
}


// The std::format based writer, for fractions the integer writer does not
// cover (>= 1 or not finite), where {:.6f} prints extra digits.
static std::string
iso8601_generic(const Datetime::tm_t& tm, bool trim, char sep)
{
	if (trim) {
		auto res = std::format("{:04}-{:02}-{:02}{}{:02}:{:02}:{:02}{}",
			tm.year, tm.mon, tm.day, sep,
			tm.hour, tm.min, tm.sec, tm.is_utc ? "Z" : "");
		if (tm.fsec > 0.0) {
			if (tm.is_utc) {
				res.pop_back();
			}
			res += std::format("{:.6f}", tm.fsec).erase(0, 1);
			auto it_e = res.end();
			auto it = it_e - 1;
			for (; *it == '0'; --it) { }
			if (*it != '.') {
				++it;
			}
			res.erase(it, it_e);
			if (tm.is_utc) {
				res.push_back('Z');
			}
		}
		return res;
	}

	if (tm.fsec > 0.0) {
		auto res = std::format("{:04}-{:02}-{:02}{}{:02}:{:02}:{:02}",
			tm.year, tm.mon, tm.day, sep,
			tm.hour, tm.min, tm.sec);
		res += std::format("{:.6f}", tm.fsec).erase(0, 1);
		if (tm.is_utc) {
				res.push_back('Z');
		}
		return res;
	}

	return std::format("{:04}-{:02}-{:02}{}{:02}:{:02}:{:02}.000000{}",
		tm.year, tm.mon, tm.day, sep,
		tm.hour, tm.min, tm.sec, tm.is_utc ? "Z" : "");
}


#ifdef __SIZEOF_INT128__
// Microseconds of 0 < f < 1 as std::format("{:.6f}") rounds them: the exact
// binary value, ties to even. f = M * 2^-s exactly, so the product with 1e6 is
// an integer-and-remainder problem in 128 bits (no double rounding).
static inline unsigned
fsec_micros(double f)
{
	const auto bits = std::bit_cast<std::uint64_t>(f);
	const auto exp = static_cast<int>(bits >> 52);
	std::uint64_t mant = bits & ((std::uint64_t{1} << 52) - 1);
	int shift;
	if (exp == 0) {
		shift = 1074;
	} else {
		mant |= std::uint64_t{1} << 52;
		shift = 1075 - exp;
	}
	if (shift >= 128) { return 0; }
	using u128 = unsigned __int128;
	const u128 prod = u128{mant} * 1000000u;
	u128 q = prod >> shift;
	const u128 rem = prod - (q << shift);
	const u128 half = u128{1} << (shift - 1);
	if (rem > half || (rem == half && (q & 1))) { ++q; }
	return static_cast<unsigned>(q);
}
#endif


/*
 * Return a string with the datetime in ISO 8601 Format.
 */
std::string
Datetime::iso8601(const tm_t& tm, bool trim, char sep)
{
#ifdef __SIZEOF_INT128__
	const bool frac = tm.fsec > 0.0;
	if (!frac || tm.fsec < 1.0) {
		char buf[160];
		char* o = put_date_time(buf, tm.year, tm.mon, tm.day, sep, tm.hour, tm.min, tm.sec);
		if (frac) {
			const unsigned micro = fsec_micros(tm.fsec) % 1000000;  // 1.000000 prints as ".000000"
			char d[6];
			unsigned v = micro;
			for (int i = 5; i >= 0; --i) { d[i] = static_cast<char>('0' + v % 10); v /= 10; }
			int n = 6;
			if (trim) {
				while (n > 0 && d[n - 1] == '0') { --n; }
			}
			if (n > 0) {
				*o++ = '.';
				std::memcpy(o, d, static_cast<std::size_t>(n));
				o += n;
			}
		} else if (!trim) {
			std::memcpy(o, ".000000", 7);
			o += 7;
		}
		if (tm.is_utc) { *o++ = 'Z'; }
		return std::string(buf, static_cast<std::size_t>(o - buf));
	}
#endif
	return iso8601_generic(tm, trim, sep);
}


/*
 * Transforms a timestamp in seconds with decimal fraction to ISO 8601 format.
 */
std::string
Datetime::iso8601(double timestamp, bool trim, char sep)
{
	auto tm = to_tm_t(timestamp);
	return iso8601(tm, trim, sep);
}


/*
 * Transforms a time_point in seconds with decimal fraction to ISO 8601 format.
 */
std::string
Datetime::iso8601(std::chrono::system_clock::time_point tp, bool trim, char sep)
{
	return iso8601(timestamp(tp), trim, sep);
}
