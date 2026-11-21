// Helpers shared by the datetime pieces: the civil-calendar core, the grammar's widths and the fraction parsers.
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

#pragma once

#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "datetime.h"
#include "strict_stox.hh"                         // for strict_stod


// ---------------------------------------------------------------------------
// Integer civil-calendar core (no libc time-zone state, no throwing checks).
// Proleptic Gregorian; days are counted from 1970-01-01.
// ---------------------------------------------------------------------------

static constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) {
	auto q = a / b;
	return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

// Days since 1970-01-01 of year-month-day; `d` is used linearly, so an
// out-of-month day rolls over into the following months.
static constexpr std::int64_t days_from_civil(std::int64_t y, int m, std::int64_t d) {
	y -= m <= 2;
	const std::int64_t era = floor_div(y, 400);
	const std::int64_t yoe = y - era * 400;
	const std::int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

// The same for year >= 1, 1 <= m <= 12 and 1 <= d <= 31: unsigned arithmetic, so the
// divisions by constants need no sign handling.
static constexpr std::int64_t days_from_civil_pos(std::uint32_t y, unsigned m, unsigned d) {
	y -= m <= 2;
	const std::uint32_t era = y / 400;
	const std::uint32_t yoe = y - era * 400;
	const std::uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	const std::uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return static_cast<std::int64_t>(era) * 146097 + doe - 719468;
}

static constexpr void civil_from_days(std::int64_t z, std::int64_t& y, int& m, int& d) {
	z += 719468;
	const std::int64_t era = floor_div(z, 146097);
	const std::int64_t doe = z - era * 146097;
	const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const std::int64_t mp = (5 * doy + 2) / 153;
	d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
	m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
	y = yoe + era * 400 + (m <= 2);
}

// Seconds since the epoch to broken-down fields (floor semantics, like gmtime_r).
static inline void civil_from_secs(std::int64_t t, std::int64_t& y, int& mon, int& day, int& hour, int& min, int& sec, int* wday = nullptr) {
	const std::int64_t z = floor_div(t, 86400);
	const auto rem = static_cast<int>(t - z * 86400);
	civil_from_days(z, y, mon, day);
	hour = rem / 3600;
	min = rem % 3600 / 60;
	sec = rem % 60;
	if (wday) {
		*wday = static_cast<int>(((z % 7) + 11) % 7);  // 1970-01-01 is a Thursday (4)
	}
}

static inline void set_from_secs(Datetime::tm_t& tm, std::int64_t t) {
	std::int64_t y;
	civil_from_secs(t, y, tm.mon, tm.day, tm.hour, tm.min, tm.sec);
	if (y > INT_MAX) { throw DatetimeError("Year is out of range"); }
	tm.year = static_cast<int>(y);
}

static inline std::int64_t secs_of(const Datetime::tm_t& tm) {
	return days_from_civil(tm.year, tm.mon, tm.day) * 86400 + tm.hour * 3600LL + tm.min * 60LL + tm.sec;
}

// Seconds at 0001-01-01T00:00:00; a date-math step that starts from a year
// below it throws "Year is out of range".
static constexpr std::int64_t MIN_YEAR1_SECS = -62135596800LL;


// Offsets: "-H h" then "-M m" of date math, as one integer sum (the second
// step throws when the first one leaves year 0).
// Precondition: tm.year >= 1 and 1 <= tm.mon <= 12.
// `day_in_month` promises 1 <= tm.day <= days in tm.mon; then an offset that
// keeps the time of day within the same date touches only hour/min/sec.
static inline void apply_tz(Datetime::tm_t& tm, char op, int h, int m, bool day_in_month) {
	const int sign = op == '+' ? -1 : 1;
	if (day_in_month) {
		const auto sod = tm.hour * 3600LL + tm.min * 60LL + tm.sec + sign * (h * 3600LL + m * 60LL);
		if (sod >= 0 && sod < 86400) {
			tm.hour = static_cast<int>(sod / 3600);
			tm.min = static_cast<int>(sod % 3600 / 60);
			tm.sec = static_cast<int>(sod % 60);
			return;
		}
	}
	auto t = secs_of(tm) + sign * h * 3600LL;
	if (t < MIN_YEAR1_SECS) {
		// The second step had already applied its minutes when timegm threw.
		set_from_secs(tm, t);
		tm.min += sign * m;
		throw DatetimeError("Year is out of range");
	}
	set_from_secs(tm, t + sign * m * 60LL);
}


// The widths of the grammar. They are datetime's own: every scanner checks them before a
// field reaches strict-stox, so the verdicts do not depend on how wide a field strict-stox
// reads.
static constexpr std::size_t MAX_FRACTION_DIGITS = 62;  // a fraction has 1 to 62 digits
static constexpr std::size_t MAX_TERM_CHARS = 63;       // the number of a date-math term, sign excluded

// A fraction of 7 to 62 digits: strict_stod + normalize_fsec. Zero digits, or 63 or more,
// is not a fraction. Out of line, so the microsecond path below stays small where it is
// inlined.
[[gnu::noinline]] static bool parse_fsec_long(const char* digits, std::size_t nd, double& out) {
	if (nd < 1 || nd > MAX_FRACTION_DIGITS) { return false; }
	std::string buf;
	buf.reserve(nd + 1);
	buf.push_back('.');
	buf.append(digits, nd);
	int errno_save;
	auto fsec = strict_stod(&errno_save, buf);
	if (errno_save != 0) { return false; }
	out = Datetime::normalize_fsec(fsec);
	return true;
}

// Fraction digits to a normalized fsec. A fraction has 1 to 62 digits. Up to 6 digits are
// integer microseconds (round(strtod(".ddd") * 1e6) / 1e6 is exactly micro / 1e6).
static inline bool parse_fsec(const char* digits, std::size_t nd, double& out) {
	if (nd >= 1 && nd <= 6) {
		unsigned micro = 0;
		for (std::size_t i = 0; i < nd; ++i) {
			micro = micro * 10 + static_cast<unsigned>(digits[i] - '0');
		}
		static constexpr unsigned pow10[] = { 1, 10, 100, 1000, 10000, 100000, 1000000 };
		out = static_cast<double>(micro * pow10[6 - nd]) / DATETIME_MICROSECONDS;
		return true;
	}
	return parse_fsec_long(digits, nd, out);
}

// The fraction of a time or timedelta, the dot and then digits only: 1 to MAX_FRACTION_DIGITS
// digits, like every other fraction.
static bool parse_dotted_fsec(std::string_view dotted, double& out) {
	if (dotted.size() < 2 || dotted.size() - 1 > MAX_FRACTION_DIGITS) { return false; }
	int errno_save;
	auto fsec = strict_stod(&errno_save, dotted);
	if (errno_save != 0) { return false; }
	out = Datetime::normalize_fsec(fsec);
	return true;
}
