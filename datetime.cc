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

#include <algorithm>                              // for std::min
#include <bit>                                    // for std::bit_cast
#include <cassert>                                // for assert
#include <cstring>                                // for std::memcpy
#include <climits>                                // for INT_MIN, INT_MAX
#include <cstdint>                                // for std::int64_t
#include <cctype>                                 // for std::isdigit
#include <cmath>                                  // for ceil
#include <exception>                              // for exception
#include <format>                                 // for std::format
#include <stdexcept>                              // for invalid_argument, out_of_range
#include <string>                                 // for std::string
#include <string_view>                            // for std::string_view

#include "strict_stox.hh"                         // for strict_stoull


const std::regex Datetime::date_re(R"(([0-9]{4})([-/ ]?)(0[1-9]|1[0-2])\2(0[0-9]|[12][0-9]|3[01])([T ]?([01]?[0-9]|2[0-3]):([0-5][0-9])(:([0-5][0-9])([.,]([0-9]+))?)?([ ]*[+-]([01]?[0-9]|2[0-3]):([0-5][0-9])|Z)?)?([ ]*\|\|[ ]*([+-/\dyMwdhms]+))?)", std::regex::optimize);
const std::regex Datetime::date_math_re("([+-]\\d+|\\/{1,2})([dyMwhms])", std::regex::optimize);


static constexpr int days[2][12] = {
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};


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


// ---------------------------------------------------------------------------
// Field readers. A fixed-width field is an optional leading isspace, an
// optional sign, then digits up to the end of the field, nothing else (the
// strtoul grammar). `field` takes the 2/4 plain digits fast path and otherwise
// emulates that grammar exactly. A nonzero negative value sets `wrap`; `v` is
// the signed value.
// ---------------------------------------------------------------------------

static bool slow_field(const char* p, int n, int& v, bool& wrap) {
	int i = 0;
	while (i < n && std::isspace(static_cast<unsigned char>(p[i]))) { ++i; }
	bool neg = false;
	if (i < n && (p[i] == '+' || p[i] == '-')) {
		neg = p[i] == '-';
		++i;
	}
	const int first = i;
	int acc = 0;
	while (i < n && p[i] >= '0' && p[i] <= '9') {
		acc = acc * 10 + (p[i] - '0');
		++i;
	}
	v = 0;
	wrap = false;
	if (i == first || i != n) {
		return false;
	}
	v = neg ? -acc : acc;
	wrap = neg && acc != 0;
	return true;
}

static inline bool field(const char* p, int n, int& v, bool& wrap) {
	int acc = 0;
	for (int i = 0; i < n; ++i) {
		const unsigned d = static_cast<unsigned char>(p[i]) - '0';
		if (d > 9) {
			return slow_field(p, n, v, wrap);
		}
		acc = acc * 10 + static_cast<int>(d);
	}
	v = acc;
	wrap = false;
	return true;
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


// ---------------------------------------------------------------------------
// ISO 8601 scanner, shared by both Iso8601Parser overloads. Fill == true is the
// overload that fills a tm_t (including the partial writes on failure);
// Fill == false only validates. The two differ in
// nothing but the writes: every check (negative or out-of-range fields, a
// fraction with no digits or too many) runs in both, so the two overloads return
// the same Format for every input.
// ---------------------------------------------------------------------------

template <bool Fill>
static Datetime::Format iso8601_scan(std::string_view datetime, Datetime::tm_t* tm) {
	using Datetime::Format;
	const auto size = datetime.size();
	const char* p = datetime.data();

	if (size == 10) {
		if (p[4] != '-' || p[7] != '-') { return Format::INVALID; }
	} else if (size < 19) {
		return Format::INVALID;
	} else {
		if (p[4] != '-' || p[7] != '-' || (p[10] != 'T' && p[10] != ' ') || p[13] != ':' || p[16] != ':') { return Format::INVALID; }
		if (size == 20) {
			if (p[19] != 'Z') { return Format::INVALID; }
			if constexpr (Fill) { tm->is_utc = true; }
		}
	}

	int year, mon, day;
	bool w;
	bool ok = field(p, 4, year, w);
	if constexpr (Fill) { tm->year = year; }
	if (!ok) { return Format::ERROR; }
	ok = field(p + 5, 2, mon, w);
	if constexpr (Fill) { tm->mon = mon; }
	if (!ok) { return Format::ERROR; }
	ok = field(p + 8, 2, day, w);
	if constexpr (Fill) { tm->day = day; }
	if (!ok) { return Format::ERROR; }
	if (!Datetime::isValidDate(year, mon, day)) { return Format::OUT_OF_RANGE; }

	if (size == 10) {
		if constexpr (Fill) {
			tm->hour = 0;
			tm->min = 0;
			tm->sec = 0;
			tm->fsec = 0.0;
		}
		return Format::VALID;
	}

	// hour/min/sec must lie in 0..limit-1; a negative field is out of range.
	auto below = [](int v, bool, int limit) { return v >= 0 && v < limit; };
	int hour, min, sec;
	ok = field(p + 11, 2, hour, w);
	if constexpr (Fill) { tm->hour = hour; }
	if (!ok) { return Format::ERROR; }
	if (!below(hour, w, 24)) { return Format::OUT_OF_RANGE; }
	ok = field(p + 14, 2, min, w);
	if constexpr (Fill) { tm->min = min; }
	if (!ok) { return Format::ERROR; }
	if (!below(min, w, 60)) { return Format::OUT_OF_RANGE; }
	ok = field(p + 17, 2, sec, w);
	if constexpr (Fill) { tm->sec = sec; }
	if (!ok) { return Format::ERROR; }
	if (!below(sec, w, 60)) { return Format::OUT_OF_RANGE; }

	if (size == 19 || size == 20) {
		if constexpr (Fill) { tm->fsec = 0.0; }
		return Format::VALID;
	}

	// The fraction: only filling needs its value; validating needs to know that
	// parse_fsec would succeed, which is exactly 1 to MAX_FRACTION_DIGITS digits (a digit
	// run cannot overflow).
	auto fraction = [&](std::size_t nd) {
		if constexpr (Fill) {
			double fsec;
			if (!parse_fsec(p + 20, nd, fsec)) { return false; }
			tm->fsec = fsec;
			return true;
		} else {
			return nd >= 1 && nd <= MAX_FRACTION_DIGITS;
		}
	};

	// Offset hh:mm at p (a negative field is out of range).
	// Returns VALID with h/m set, or the failing Format.
	auto offset = [](const char* q, int& h, int& m) {
		bool hw, mw;
		(void)hw; (void)mw;
		if (!field(q, 2, h, hw)) { return Format::ERROR; }
		if (h < 0 || h >= 24) { return Format::OUT_OF_RANGE; }
		if (!field(q + 3, 2, m, mw)) { return Format::ERROR; }
		if (m < 0 || m >= 60) { return Format::OUT_OF_RANGE; }
		return Format::VALID;
	};

	switch (p[19]) {
		case '+':
		case '-': {
			if constexpr (Fill) { tm->is_utc = true; }
			if (size == 25 && p[22] == ':') {
				if constexpr (Fill) { tm->fsec = 0.0; }
				int h, m;
				auto f = offset(p + 20, h, m);
				if (f != Format::VALID) { return f; }
				if constexpr (Fill) { apply_tz(*tm, p[19], h, m, true); }
				return Format::VALID;
			}
			return Format::INVALID;
		}
		case '.': {
			std::size_t i = 20;
			while (i < size && p[i] >= '0' && p[i] <= '9') { ++i; }
			const std::size_t nd = i - 20;
			if (i == size) {
				if (!fraction(nd)) { return Format::ERROR; }
				return Format::VALID;
			}
			switch (p[i]) {
				case 'Z':
					if constexpr (Fill) { tm->is_utc = true; }
					if (i + 1 == size) {
						if (!fraction(nd)) { return Format::ERROR; }
						return Format::VALID;
					}
					return Format::ERROR;
				case '+':
				case '-':
					if constexpr (Fill) { tm->is_utc = true; }
					if (size - i == 6 && p[i + 3] == ':') {
						int h, m;
						auto f = offset(p + i + 1, h, m);
						if (f != Format::VALID) { return f; }
						if (!fraction(nd)) { return Format::ERROR; }
						if constexpr (Fill) { apply_tz(*tm, p[i], h, m, true); }
						return Format::VALID;
					}
					return Format::INVALID;
				default:
					return Format::INVALID;
			}
		}
		default:
			return Format::INVALID;
	}
}


// ---------------------------------------------------------------------------
// Hand scanner for the grammar of date_re (which stays exported, unused):
//
//   YYYY ([-/ ]?) MM \2 DD
//   ( [T ]? H:mm ( :ss ([.,]digits+)? )? ( [ ]*[+-]H:mm | Z )? )?
//   ( [ ]* || [ ]* [+-,./0-9yMwdhms]+ )?
//
// with H = [01]?[0-9] | 2[0-3], MM = 01-12, DD = 00-31, mm/ss = 00-59. Every
// optional part is introduced by a character no other alternative can start
// with, so the greedy scan below picks the same captures as the regex.
// ---------------------------------------------------------------------------

struct DateReMatch {
	int year = 0, mon = 0, day = 0;
	bool has_time = false;
	int hour = 0, min = 0, sec = 0;
	bool has_sec = false;
	const char* frac = nullptr;
	std::size_t frac_n = 0;
	char tz = 0;  // '+', '-', 'Z' or 0
	int tz_h = 0, tz_m = 0;
	bool has_math = false;
	std::string_view math;
};

static bool scan_date_re(std::string_view s, DateReMatch& r) {
	const char* p = s.data();
	const std::size_t n = s.size();
	auto dig = [&](std::size_t k) { return k < n && p[k] >= '0' && p[k] <= '9'; };
	auto dv = [&](std::size_t k) { return p[k] - '0'; };

	if (!(dig(0) && dig(1) && dig(2) && dig(3))) { return false; }
	r.year = dv(0) * 1000 + dv(1) * 100 + dv(2) * 10 + dv(3);
	std::size_t i = 4;
	char sep = 0;
	if (i < n && (p[i] == '-' || p[i] == '/' || p[i] == ' ')) {
		sep = p[i++];
	}
	if (!(dig(i) && dig(i + 1))) { return false; }
	r.mon = dv(i) * 10 + dv(i + 1);
	if (r.mon < 1 || r.mon > 12) { return false; }
	i += 2;
	if (sep) {
		if (i >= n || p[i] != sep) { return false; }
		++i;
	}
	if (!(dig(i) && dig(i + 1))) { return false; }
	r.day = dv(i) * 10 + dv(i + 1);
	if (r.day > 31) { return false; }
	i += 2;

	// H followed by ':' at k; returns the digit count (0: not an hour).
	auto hour_at = [&](std::size_t k, int& h) -> int {
		if (dig(k)) {
			if (k + 1 < n && p[k + 1] == ':') {
				h = dv(k);
				return 1;
			}
			if (dig(k + 1) && k + 2 < n && p[k + 2] == ':') {
				const int a = dv(k), b = dv(k + 1);
				if (a <= 1 || (a == 2 && b <= 3)) {
					h = a * 10 + b;
					return 2;
				}
			}
		}
		return 0;
	};
	auto min_at = [&](std::size_t k, int& v) {
		if (dig(k) && dig(k + 1) && p[k] <= '5') {
			v = dv(k) * 10 + dv(k + 1);
			return true;
		}
		return false;
	};

	std::size_t t = i;
	if (t < n && (p[t] == 'T' || p[t] == ' ')) { ++t; }
	int hn = hour_at(t, r.hour);
	if (hn) {
		r.has_time = true;
		t += hn + 1;
		if (!min_at(t, r.min)) { return false; }
		i = t + 2;
		if (i < n && p[i] == ':') {
			if (!min_at(i + 1, r.sec)) { return false; }
			r.has_sec = true;
			i += 3;
			if (i < n && (p[i] == '.' || p[i] == ',')) {
				std::size_t f = i + 1;
				while (dig(f)) { ++f; }
				if (f == i + 1) { return false; }
				r.frac = p + i + 1;
				r.frac_n = f - i - 1;
				i = f;
			}
		}
		std::size_t j = i;
		while (j < n && p[j] == ' ') { ++j; }
		if (j < n && (p[j] == '+' || p[j] == '-')) {
			r.tz = p[j];
			hn = hour_at(j + 1, r.tz_h);
			if (!hn) { return false; }
			j += 1 + hn + 1;
			if (!min_at(j, r.tz_m)) { return false; }
			i = j + 2;
		} else if (i < n && p[i] == 'Z') {
			r.tz = 'Z';
			++i;
		}
	}

	std::size_t j = i;
	while (j < n && p[j] == ' ') { ++j; }
	if (j + 1 < n && p[j] == '|' && p[j + 1] == '|') {
		j += 2;
		while (j < n && p[j] == ' ') { ++j; }
		if (j >= n) { return false; }
		for (std::size_t k = j; k < n; ++k) {
			const char c = p[k];
			if (!((c >= '+' && c <= '/') || (c >= '0' && c <= '9') || c == 'y' || c == 'M' || c == 'w' || c == 'd' || c == 'h' || c == 'm' || c == 's')) {
				return false;
			}
		}
		r.has_math = true;
		r.math = s.substr(j);
		return true;
	}
	return i == n;
}


// Parse a datetime string (ISO 8601, optionally followed by `|| <date math>`)
// into a tm_t. This is the pure-string path that DatetimeParser(string_view)
// delegates to. The MsgPack/structured-object path lives in the Xapiand glue.
static inline void process_date_datetime(Datetime::tm_t& tm, std::string_view str_datetime) {
	// Check if datetime is ISO 8601.
	auto pos = str_datetime.find("||");
	if (pos == std::string_view::npos) {
		auto format = Datetime::Iso8601Parser(str_datetime, tm);
		switch (format) {
			case Datetime::Format::VALID:
				return;
			case Datetime::Format::INVALID:
				break;
			case Datetime::Format::OUT_OF_RANGE:
				goto error_out_of_range;
			default:
				goto error;
		}
	} else {
		auto format = Datetime::Iso8601Parser(str_datetime.substr(0, pos), tm);
		switch (format) {
			case Datetime::Format::VALID:
				Datetime::processDateMath(str_datetime.substr(pos + 2), tm);
				return;
			case Datetime::Format::INVALID:
				break;
			case Datetime::Format::OUT_OF_RANGE:
				goto error_out_of_range;
			default:
				goto error;
		}
	}

	{
		DateReMatch m;
		if (scan_date_re(str_datetime, m)) {
			tm.year = m.year;
			tm.mon = m.mon;
			tm.day = m.day;
			if (!Datetime::isValidDate(tm.year, tm.mon, tm.day)) {
				goto error_out_of_range;
			}

			// Process time
			if (!m.has_time) {
				tm.hour = tm.min = tm.sec = 0;
				tm.fsec = 0.0;
			} else {
				tm.hour = m.hour;
				tm.min = m.min;
				if (!m.has_sec) {
					tm.sec = 0;
					tm.fsec = 0.0;
				} else {
					tm.sec = m.sec;
					if (!m.frac) {
						tm.fsec = 0.0;
					} else {
						double fsec;
						if (!parse_fsec(m.frac, m.frac_n, fsec)) { goto error; }
						tm.fsec = fsec;
					}
				}
				if (m.tz == '+' || m.tz == '-') {
					// An applied offset makes the result UTC, as on the ISO path.
					tm.is_utc = true;
					apply_tz(tm, m.tz, m.tz_h, m.tz_m, true);
				} else if (m.tz == 'Z') {
					// Z is UTC, as on the ISO path: no offset to apply.
					tm.is_utc = true;
				}
			}

			// Process Datetime Math
			if (m.has_math) {
				Datetime::processDateMath(m.math, tm);
			}

			return;
		}
	}

error:
	throw DatetimeError(std::format("Error format in _datetime: {}", str_datetime));

error_out_of_range:
	throw DatetimeError(std::format("Datetime: {} is out of range", str_datetime));
}


/*
 * Returns struct tm according to the datetime specified by datetime.
 */
Datetime::tm_t
Datetime::DatetimeParser(std::string_view datetime)
{
	tm_t tm;
	process_date_datetime(tm, datetime);
	return tm;
}


/*
 * Full struct tm according to the datetime in ISO 8601 format.
 * (Iso8601Parser(datetime) below validates only; both share iso8601_scan.)
 */
Datetime::Format
Datetime::Iso8601Parser(std::string_view datetime, tm_t& tm)
{
	return iso8601_scan<true>(datetime, &tm);
}


Datetime::Format
Datetime::Iso8601Parser(std::string_view datetime)
{
	return iso8601_scan<false>(datetime, nullptr);
}


void
Datetime::processDateMath(std::string_view date_math, tm_t& tm)
{
	// Same grammar as date_math_re, matched contiguously from the start:
	//   ( [+-][0-9]+ | /{1,2} ) [dyMwhms]
	const char* p = date_math.data();
	const std::size_t n = date_math.size();
	std::size_t i = 0;
	while (i < n) {
		std::size_t j = i;
		if (p[j] == '+' || p[j] == '-') {
			const std::size_t d0 = ++j;
			while (j < n && p[j] >= '0' && p[j] <= '9') { ++j; }
			if (j == d0) { break; }
		} else if (p[j] == '/') {
			++j;
			if (j < n && p[j] == '/') { ++j; }
		} else {
			break;
		}
		if (j >= n) { break; }
		const char u = p[j];
		if (u != 'd' && u != 'y' && u != 'M' && u != 'w' && u != 'h' && u != 'm' && u != 's') { break; }
		computeDateMath(tm, date_math.substr(i, j - i), u);
		i = j + 1;
	}

	if (i != n) {
		throw DatetimeError(std::format("Datetime Math ({}) is used incorrectly", date_math));
	}
}


// Builds the two date-math operators as strings, for the arguments the
// integer path does not cover.
static void computeTimeZone_generic(Datetime::tm_t& tm, char op, std::string_view hour, std::string_view min) {
	std::string oph, opm;
	oph.reserve(3);
	opm.reserve(3);
	if (op == '+') {
		oph.push_back('-');
		oph.append(hour.data(), hour.size());
		opm.push_back('-');
		opm.append(min.data(), min.size());
	} else {
		oph.push_back('+');
		oph.append(hour.data(), hour.size());
		opm.push_back('+');
		opm.append(min.data(), min.size());
	}
	Datetime::computeDateMath(tm, oph, 'h');
	Datetime::computeDateMath(tm, opm, 'm');
}


void
Datetime::computeTimeZone(tm_t& tm, char op, std::string_view hour, std::string_view min)
{
	auto digits = [](std::string_view s, int& v) {
		if (s.empty() || s.size() > 2) { return false; }
		v = 0;
		for (char c : s) {
			if (c < '0' || c > '9') { return false; }
			v = v * 10 + (c - '0');
		}
		return true;
	};
	int h, m;
	if (tm.year >= 1 && tm.mon >= 1 && tm.mon <= 12 && digits(hour, h) && digits(min, m)) {
		apply_tz(tm, op, h, m, tm.day >= 1 && tm.day <= 28);
		return;
	}
	computeTimeZone_generic(tm, op, hour, min);
}




// The number of a date-math term: strtol grammar (leading isspace, sign, digits)
// consuming the whole string, at most MAX_TERM_CHARS chars, result within int.
static bool parse_int(std::string_view s, int& out) {
	const std::size_t n = s.size();
	if (n == 0 || n > MAX_TERM_CHARS) { return false; }
	std::size_t i = 0;
	while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) { ++i; }
	bool neg = false;
	if (i < n && (s[i] == '+' || s[i] == '-')) {
		neg = s[i] == '-';
		++i;
	}
	const std::size_t first = i;
	std::int64_t acc = 0;
	while (i < n && s[i] >= '0' && s[i] <= '9') {
		acc = std::min<std::int64_t>(acc * 10 + (s[i] - '0'), std::int64_t{1} << 40);  // saturate: out of int range anyway
		++i;
	}
	if (i == first || i != n) { return false; }
	if (neg) { acc = -acc; }
	if (acc < INT_MIN || acc > INT_MAX) { return false; }
	out = static_cast<int>(acc);
	return true;
}


// Adds n units (y, M, w, d, h, m, s) to tm and renormalizes, all in 64-bit
// arithmetic: |n| <= 2^31 and every field is an int, so no step can overflow.
// The supported years are 1..INT_MAX; a step that would leave them throws
// "Year is out of range" (the same error toordinal gives below year 1). The
// month normalization is the one of normalizeMonths.
static void add_term(Datetime::tm_t& tm, std::int64_t n, char unit) {
	std::int64_t year = tm.year, mon = tm.mon, day = tm.day, hour = tm.hour, min = tm.min, sec = tm.sec;
	auto store = [&] {
		auto fit = [](std::int64_t v) { return static_cast<int>(std::clamp<std::int64_t>(v, INT_MIN, INT_MAX)); };
		tm.year = fit(year); tm.mon = fit(mon); tm.day = fit(day);
		tm.hour = fit(hour); tm.min = fit(min); tm.sec = fit(sec);
	};
	auto normalize = [&] {
		// Month 1..12 of the year that holds the (possibly out-of-range) month.
		const std::int64_t k = floor_div(mon - 1, 12);
		year += k;
		mon -= 12 * k;
	};
	switch (unit) {
		case 'y': year += n; break;
		case 'M': {
			mon += n;
			normalize();
			store();
			if (year < INT_MIN || year > INT_MAX) { throw DatetimeError("Year is out of range"); }
			const auto max_days = Datetime::getDays_month(tm.year, tm.mon);
			if (tm.day > max_days) { day = max_days; }
			break;
		}
		case 'w': day += 7 * n; break;
		case 'd': day += n; break;
		case 'h': hour += n; break;
		case 'm': min += n; break;
		default: sec += n; break;
	}
	normalize();
	store();
	if (year < 1 || year > INT_MAX) { throw DatetimeError("Year is out of range"); }
	const std::int64_t t = days_from_civil(year, static_cast<int>(mon), day) * 86400 + hour * 3600 + min * 60 + sec;
	set_from_secs(tm, t);
}


/*
 * Compute a Datetime Math former by op + units.
 * op can be +#, -#, /, //
 * unit can be y, M, w, d, h, m, s
 */
void
Datetime::computeDateMath(tm_t& tm, std::string_view op, char unit)
{
	switch (op[0]) {
		case '+':
		case '-': {
			int num;
			if (!parse_int(op.substr(1), num)) {
				throw DatetimeError(std::format("Invalid format in Datetime Math unit: '{}'. '{}' must be numeric", unit, op.substr(1)));
			}
			if (std::string_view("yMwdhms").find(unit) == std::string_view::npos) {
				throw DatetimeError(std::format("Invalid format in Datetime Math unit: '{}'. Unit must be in {{ y, M, w, d, h, m, s }}", unit));
			}
			add_term(tm, op[0] == '+' ? std::int64_t{num} : -std::int64_t{num}, unit);
			return;
		}
		case '/':
			switch (unit) {
				case 'y':
					if (op.size() == 1) {
						tm.mon = 12;
						tm.day = getDays_month(tm.year, 12);
						tm.hour = 23;
						tm.min = tm.sec = 59;
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.mon = tm.day = 1;
						tm.hour = tm.min = tm.sec = 0;
						tm.fsec = 0.0;
					} else {
						throw DatetimeError(std::format("Invalid format in Datetime Math operator: {}. Operator must be in {{ +#, -#, /, // }}", op));
					}
					break;
				case 'M':
					if (op.size() == 1) {
						tm.day = getDays_month(tm.year, tm.mon);
						tm.hour = 23;
						tm.min = tm.sec = 59;
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.day = 1;
						tm.hour = tm.min = tm.sec = 0;
						tm.fsec = 0.0;
					} else {
						throw DatetimeError(std::format("Invalid format in Datetime Math operator: {}. Operator must be in {{ +#, -#, /, // }}", op));
					}
					break;
				case 'w': {
					std::int64_t y;
					int mo, d, h, mi, s, wday;
					civil_from_secs(timegm(tm), y, mo, d, h, mi, s, &wday);
					if (op.size() == 1) {
						tm.day += 6 - wday;
						tm.hour = 23;
						tm.min = tm.sec = 59;
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.day -= wday;
						tm.hour = tm.min = tm.sec = 0;
						tm.fsec = 0.0;
					} else {
						throw DatetimeError(std::format("Invalid format in Datetime Math operator: {}. Operator must be in {{ +#, -#, /, // }}", op));
					}
					break;
				}
				case 'd':
					if (op.size() == 1) {
						tm.hour = 23;
						tm.min = tm.sec = 59;
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.hour = tm.min = tm.sec = 0;
						tm.fsec = 0.0;
					} else {
						throw DatetimeError(std::format("Invalid format in Datetime Math operator: {}. Operator must be in {{ +#, -#, /, // }}", op));
					}
					break;
				case 'h':
					if (op.size() == 1) {
						tm.min = tm.sec = 59;
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.min = tm.sec = 0;
						tm.fsec = 0.0;
					} else {
						throw DatetimeError(std::format("Invalid format in Datetime Math operator: {}. Operator must be in {{ +#, -#, /, // }}", op));
					}
					break;
				case 'm':
					if (op.size() == 1) {
						tm.sec = 59;
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.sec = 0;
						tm.fsec = 0.0;
					} else {
						throw DatetimeError(std::format("Invalid format in Datetime Math operator: {}. Operator must be in {{ +#, -#, /, // }}", op));
					}
					break;
				case 's':
					if (op.size() == 1) {
						tm.fsec = DATETIME_MAX_FSEC;
					} else if (op.size() == 2 && op[1] == '/') {
						tm.fsec = 0.0;
					} else {
						throw DatetimeError(std::format("Invalid format in Datetime Math operator: {}. Operator must be in {{ +#, -#, /, // }}", op));
					}
				break;
			}
			break;
		default:
			throw DatetimeError(std::format("Invalid format in Datetime Math operator: {}. Operator must be in {{ +#, -#, /, // }}", op));
	}

	// Update datetime.
	set_from_secs(tm, timegm(tm));
}


/*
 * Returns if a year is leap.
 */
bool
Datetime::isleapYear(int year)
{
	return year % 400 == 0 || (year % 4 == 0 && ((year % 100) != 0));
}


/*
 * Returns if a tm_year is leap.
 */
bool
Datetime::isleapRef_year(int tm_year)
{
	tm_year += DATETIME_START_YEAR;
	return isleapYear(tm_year);
}


static int
_getDays_month(int year, int month)
{
	assert(month > 0);
	assert(month <= 12);
	auto leap = static_cast<int>(Datetime::isleapYear(year));
	return days[leap][month - 1];
}

/*
 * Returns number of days in month, given the year.
 */
int
Datetime::getDays_month(int year, int month)
{
	if (month < 1 || month > 12) {
		throw DatetimeError("Month must be in 1..12");
	}
	return _getDays_month(year, month);
}


/*
 * Normalize the month into 1..12, carrying whole years into `year`. A result year
 * outside int throws "Year is out of range" and leaves both arguments alone.
 */
void
Datetime::normalizeMonths(int& year, int& mon)
{
	const std::int64_t k = floor_div(static_cast<std::int64_t>(mon) - 1, 12);
	const std::int64_t y = year + k;
	if (y < INT_MIN || y > INT_MAX) { throw DatetimeError("Year is out of range"); }
	year = static_cast<int>(y);
	mon = static_cast<int>(mon - 12 * k);
}


/*
 * Days since 1970-01-01 of the first day of month `mon` of `year`, with the month carried into
 * the year first (in place); throws below year 1.
 */
static inline std::int64_t
first_of_month(int& year, int& mon)
{
	if (mon < 1 || mon > 12) { Datetime::normalizeMonths(year, mon); }
	if (year < 1) { throw DatetimeError("Year is out of range"); }
	return days_from_civil_pos(static_cast<std::uint32_t>(year), static_cast<unsigned>(mon), 1);
}


/*
 * Returns the proleptic Gregorian ordinal of the datetime,
 * where January 1 of year 1 has ordinal 1 (reference datetime).
 * year -> Any positive number except zero.
 * month -> Between 1 and 12 inclusive.
 * day -> Between 1 and the number of days in the given month of the given year.
 */
std::time_t
Datetime::toordinal(int year, int month, int day)
{
	if (year < 1) { throw DatetimeError("Year is out of range"); }
	if (day < 1) { throw DatetimeError("Day is out of range for month"); }
	if (month < 1 || month > 12) { throw DatetimeError("Month must be in 1..12"); }
	if (day > 28 && day > _getDays_month(year, month)) { throw DatetimeError("Day is out of range for month"); }
	return static_cast<std::time_t>(days_from_civil_pos(static_cast<std::uint32_t>(year), static_cast<unsigned>(month), static_cast<unsigned>(day)) + DATETIME_EPOCH_ORD);
}


/*
 * Function to calculate Unix timestamp from Coordinated Universal Time (UTC).
 * Only for year greater than 0.
 */
std::time_t
Datetime::timegm(const std::tm& tm)
{
	int year = tm.tm_year + DATETIME_START_YEAR, mon = tm.tm_mon + 1;
	const std::int64_t days = first_of_month(year, mon) + tm.tm_mday - 1;
	return static_cast<std::time_t>(days * 86400 + tm.tm_hour * 3600LL + tm.tm_min * 60LL + tm.tm_sec);
}


/*
 * Function to calculate Unix timestamp from Coordinated Universal Time (UTC).
 * Only for year greater than 0.
 */
std::time_t
Datetime::timegm(tm_t& tm)
{
	const std::int64_t days = first_of_month(tm.year, tm.mon) + tm.day - 1;
	return static_cast<std::time_t>(days * 86400 + tm.hour * 3600LL + tm.min * 60LL + tm.sec);
}


/*
 * Transforms timestamp to a struct tm_t.
 */
Datetime::tm_t
Datetime::to_tm_t(std::time_t timestamp)
{
	std::int64_t y;
	int mon, day, hour, min, sec;
	civil_from_secs(timestamp, y, mon, day, hour, min, sec);
	return { static_cast<int>(y), mon, day, hour, min, sec };
}


/*
 * Transforms timestamp to a struct tm_t. The whole seconds are the floor of the
 * timestamp and the fraction is what lies above it, before 1970 as after.
 */
Datetime::tm_t
Datetime::to_tm_t(double timestamp)
{
	const double whole = std::floor(timestamp);
	std::int64_t y;
	int mon, day, hour, min, sec;
	civil_from_secs(static_cast<std::time_t>(whole), y, mon, day, hour, min, sec);
	return { static_cast<int>(y), mon, day, hour, min, sec, normalize_fsec(timestamp - whole) };
}


/*
 * Function to calculate Unix timestamp from Coordinated Universal Time (UTC).
 * Only for year greater than 0.
 * Returns Timestamp with the microseconds as the decimal part.
 */
double
Datetime::timestamp(const tm_t& tm)
{
	int year = tm.year, mon = tm.mon;
	const std::int64_t days = first_of_month(year, mon) + tm.day - 1;
	return static_cast<double>(days * 86400 + tm.hour * 3600LL + tm.min * 60LL + tm.sec) + tm.fsec;
}


/*
 * Validate Datetime.
 */
bool
Datetime::isValidDate(int year, int month, int day)
{
	if (year < 1) {
		return false;
	}

	if (month < 1 || month > 12) {
		return false;
	}

	if (day < 1 || day > _getDays_month(year, month)) {
		return false;
	}

	return true;
}


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


bool
Datetime::isDate(std::string_view date)
{
	auto size = date.size();
	switch (size) {
		case 10: // 0000-00-00
			if (date[4] == '-' && date[7] == '-') {
				int year, mon, day;
				bool w;
				if (!field(date.data(), 4, year, w)) { return false; }
				if (!field(date.data() + 5, 2, mon, w)) { return false; }
				if (!field(date.data() + 8, 2, day, w)) { return false; }
				return isValidDate(year, mon, day);
			}
			[[fallthrough]];
		default:
			return false;
	}
}


bool
Datetime::isDatetime(std::string_view datetime)
{
	auto format = Iso8601Parser(datetime);
	switch (format) {
		case Format::VALID:
			return true;
		case Format::INVALID: {
			DateReMatch m;
			return scan_date_re(datetime, m);
		}
		default:
			return false;
	}
}


Datetime::clk_t
Datetime::TimeParser(std::string_view _time)
{
	int errno_save;
	clk_t clk;
	auto length = _time.length();
	switch (length) {
		case 5: // 00:00
			if (_time[2] == ':') {
				clk.hour = strict_stoul(&errno_save, _time.substr(0, 2));
				if (errno_save != 0) { goto error; }
				clk.min = strict_stoul(&errno_save, _time.substr(3, 2));
				if (errno_save != 0) { goto error; }
				return clk;
			}
			break;
		case 8: // 00:00:00
			if (_time[2] == ':' && _time[5] == ':') {
				clk.hour = strict_stoul(&errno_save, _time.substr(0, 2));
				if (errno_save != 0) { goto error; }
				clk.min = strict_stoul(&errno_save, _time.substr(3, 2));
				if (errno_save != 0) { goto error; }
				clk.sec = strict_stoul(&errno_save, _time.substr(6, 2));
				if (errno_save != 0) { goto error; }
				return clk;
			}
			break;
		default: //  00:00:00[+-]00:00  00:00:00.000...  00:00:00.000...[+-]00:00
			if (length > 9 && (_time[2] == ':' && _time[5] == ':')) {
				clk.hour = strict_stoul(&errno_save, _time.substr(0, 2));
				if (errno_save != 0) { goto error; }
				clk.min = strict_stoul(&errno_save, _time.substr(3, 2));
				if (errno_save != 0) { goto error; }
				clk.sec = strict_stoul(&errno_save, _time.substr(6, 2));
				if (errno_save != 0) { goto error; }
				switch (_time[8]) {
					case '-':
						clk.tz_s = '-';
						[[fallthrough]];
					case '+':
						if (length == 14 && _time[11] == ':') {
							clk.tz_h = strict_stoul(&errno_save, _time.substr(9, 2));
							if (errno_save != 0) { goto error; }
							clk.tz_m = strict_stoul(&errno_save, _time.substr(12, 2));
							if (errno_save != 0) { goto error; }
							return clk;
						}
						break;
					case '.': {
						auto it = _time.begin() + 8;
						const auto it_e = _time.end();
						for (auto aux = it + 1; aux != it_e; ++aux) {
							const auto& c = *aux;
							if (c < '0' || c > '9') {
								switch (c) {
									case '-':
										clk.tz_s = '-';
										[[fallthrough]];
									case '+':
										if ((it_e - aux) == 6) {
											auto aux_end = aux + 3;
											if (*aux_end == ':') {
												clk.tz_h = strict_stoul(&errno_save, std::string_view(aux + 1, aux_end - aux - 1));
												if (errno_save != 0) { goto error; }
												clk.tz_m = strict_stoul(&errno_save, std::string_view(aux_end + 1, it_e - aux_end - 1));
												if (errno_save != 0) { goto error; }
												double fsec;
												if (!parse_dotted_fsec(std::string_view(it, aux - it), fsec)) { goto error; }
												clk.fsec = fsec;
												return clk;
											}
										}
										break;
									default:
										break;
								}
								goto error;
							}
						}
						double fsec;
						if (!parse_dotted_fsec(std::string_view(it, it_e - it), fsec)) { goto error; }
						clk.fsec = fsec;
						return clk;
					}
					default:
						break;
				}
			}
			break;
	}

error:
	throw TimeError(std::format("Error format in time: {}, the format must be '00:00(:00(.0...)([+-]00:00))'", _time));
}


/*
 * Transforms double time to a struct clk_t.
 */
Datetime::clk_t
Datetime::time_to_clk_t(double t)
{
	if (isvalidTime(t)) {
		clk_t clk;
		if (t < 0) {
			auto _time = static_cast<int>(-t);
			clk.fsec = t + _time;
			if (clk.fsec < 0.0) {
				++_time;
			}
			clk.tz_h = _time / 3600;
			int aux;
			if (clk.tz_h < 100) {
				aux = clk.tz_h * 3600;
			} else {
				clk.tz_h = 99;
				aux = clk.tz_h * 3600;
			}
			clk.tz_m = (_time - aux) / 60;
			clk.sec = _time - aux - clk.tz_m * 60;
			if (clk.sec != 0) {
				clk.sec = 60 - clk.sec;
				++clk.tz_m;
			}
			clk.tz_s = '+';
			if (clk.fsec < 0) {
				clk.fsec += 1.0;
			}
		} else {
			auto _time = static_cast<int>(t);
			clk.hour = _time / 3600;
			if (clk.hour < 100) {
				auto aux = _time - clk.hour * 3600;
				clk.min = aux / 60;
				clk.sec = aux - clk.min * 60;
			} else {
				if (clk.hour < 199) {
					auto aux = _time - clk.hour * 3600;
					clk.tz_h = clk.hour - 99;
					clk.hour = 99;
					clk.min = aux / 60;
					clk.sec = aux - clk.min * 60;
				} else {
					clk.tz_h = 99;
					clk.hour = 99;
					auto aux = _time - 712800; // 198 * 3600
					clk.min = aux / 60;
					if (clk.min < 100) {
						clk.sec = aux - clk.min * 60;
					} else {
						if (clk.min < 199) {
							clk.sec = aux - clk.min * 60;
							clk.tz_m = 198 - clk.min;
							clk.min = 99;
						} else {
							clk.tz_m = 99;
							clk.min = 99;
							clk.sec = aux - 11880; // 198 * 60
						}
					}
				}
			}
			clk.tz_s = '-';
			clk.fsec = t - _time;
		}

		return clk;
	}

	throw TimeError("Bad serialised time value");
}


double
Datetime::time_to_double(const clk_t& clk)
{
	int hour, min;
	if (clk.tz_s == '-') {
		hour = clk.hour + clk.tz_h;
		min = clk.min + clk.tz_m;
	} else {
		hour = clk.hour - clk.tz_h;
		min = clk.min - clk.tz_m;
	}
	return clk.sec + clk.fsec + (hour * 60 + min) * 60;
}


std::string
Datetime::time_to_string(const clk_t& clk, bool trim)
{
	if (trim && clk.tz_h == 0 && clk.tz_m == 0) {
		auto res = std::format("{:02}:{:02}:{:02}",
			clk.hour, clk.min, clk.sec);
		if (clk.fsec > 0) {
			res += std::format("{:.6f}", clk.fsec).erase(0, 1);
			auto it_e = res.end();
			auto it = it_e - 1;
			for (; *it == '0'; --it) { }
			if (*it != '.') {
				++it;
			}
			res.erase(it, it_e);
		}
		return res;
	}

	if (clk.fsec > 0) {
		auto res = std::format("{:02}:{:02}:{:02}",
			clk.hour, clk.min, clk.sec);
		res += std::format("{:.6f}{}{:02}:{:02}", clk.fsec,
			clk.tz_s, clk.tz_h, clk.tz_m).erase(0, 1);
		return res;
	}

	return std::format("{:02}:{:02}:{:02}.000000{}{:02}:{:02}",
		clk.hour, clk.min, clk.sec,
		clk.tz_s, clk.tz_h, clk.tz_m);
}


std::string
Datetime::time_to_string(double t, bool trim)
{
	return time_to_string(time_to_clk_t(t), trim);
}


bool
Datetime::isTime(std::string_view _time) {
	auto length = _time.length();
	switch (length) {
		case 5: // 00:00
			return (std::isdigit(_time[0]) != 0) && (std::isdigit(_time[1]) != 0) && _time[2] == ':' &&
				(std::isdigit(_time[3]) != 0) && (std::isdigit(_time[4]) != 0);
		case 8: // 00:00:00
			return (std::isdigit(_time[0]) != 0) && (std::isdigit(_time[1]) != 0) && _time[2] == ':' &&
				(std::isdigit(_time[3]) != 0) && (std::isdigit(_time[4]) != 0) && _time[5] == ':' &&
				(std::isdigit(_time[6]) != 0) && (std::isdigit(_time[7]) != 0);
		default: //  00:00:00[+-]00:00  00:00:00.000...  00:00:00.000...[+-]00:00
			if (length > 9 && (std::isdigit(_time[0]) != 0) && (std::isdigit(_time[1]) != 0) && _time[2] == ':' &&
				(std::isdigit(_time[3]) != 0) && (std::isdigit(_time[4]) != 0) && _time[5] == ':' &&
				(std::isdigit(_time[6]) != 0) && (std::isdigit(_time[7]) != 0)) {
				switch (_time[8]) {
					case '+':
					case '-':
						return length == 14 && (std::isdigit(_time[9]) != 0) && (std::isdigit(_time[10]) != 0) && _time[11] == ':' &&
							(std::isdigit(_time[12]) != 0) && (std::isdigit(_time[13]) != 0);
					case '.':
						for (size_t i = 9; i < length; ++i) {
							const auto c = _time[i];
							if (std::isdigit(c) == 0) {
								return (c == '+' || c == '-') && length == (i + 6) && (std::isdigit(_time[i + 1]) != 0) &&
									(std::isdigit(_time[i + 2]) != 0) && _time[i + 3] == ':' && (std::isdigit(_time[i + 4]) != 0) &&
									(std::isdigit(_time[i + 5]) != 0);
							}
						}
						return true;
				}
			}
			return false;
	}
}


Datetime::clk_t
Datetime::TimedeltaParser(std::string_view timedelta)
{
	int errno_save;
	clk_t clk;
	auto size = timedelta.size();
	switch (size) {
		case 6: // [+-]00:00
			switch (timedelta[0]) {
				case '-':
					clk.tz_s = '-';
					[[fallthrough]];
				case '+':
					if (timedelta[3] == ':') {
						clk.hour = strict_stoul(&errno_save, timedelta.substr(1, 2));
						if (errno_save != 0) { goto error; }
						clk.min = strict_stoul(&errno_save, timedelta.substr(4, 2));
						if (errno_save != 0) { goto error; }
						return clk;
					}
					[[fallthrough]];
				default:
					break;
			}
			break;

		case 9: // [+-]00:00:00
			switch (timedelta[0]) {
				case '-':
					clk.tz_s = '-';
					[[fallthrough]];
				case '+':
					if (timedelta[3] == ':' && timedelta[6] == ':') {
						clk.hour = strict_stoul(&errno_save, timedelta.substr(1, 2));
						if (errno_save != 0) { goto error; }
						clk.min = strict_stoul(&errno_save, timedelta.substr(4, 2));
						if (errno_save != 0) { goto error; }
						clk.sec = strict_stoul(&errno_save, timedelta.substr(7, 2));
						if (errno_save != 0) { goto error; }
						return clk;
					}
					[[fallthrough]];
				default:
					break;
			}
			break;

		case 0: // nothing to read
			break;

		default: //  [+-]00:00:00.000...
			switch (timedelta[0]) {
				case '-':
					clk.tz_s = '-';
					[[fallthrough]];
				case '+':
					if (size > 10 && (timedelta[3] == ':' && timedelta[6] == ':' && timedelta[9] == '.')) {
						clk.hour = strict_stoul(&errno_save, timedelta.substr(1, 2));
						if (errno_save != 0) { goto error; }
						clk.min = strict_stoul(&errno_save, timedelta.substr(4, 2));
						if (errno_save != 0) { goto error; }
						clk.sec = strict_stoul(&errno_save, timedelta.substr(7, 2));
						if (errno_save != 0) { goto error; }
						auto it = timedelta.begin() + 9;
						const auto it_e = timedelta.end();
						for (auto aux = it + 1; aux != it_e; ++aux) {
							const auto& c = *aux;
							if (c < '0' || c > '9') {
								goto error;
							}
						}
						double fsec;
						if (!parse_dotted_fsec(std::string_view(it, it_e - it), fsec)) { goto error; }
						clk.fsec = fsec;
						return clk;
					}
					[[fallthrough]];
				default:
					break;
			}
			break;
	}

error:
	throw TimedeltaError(std::format("Error format in timedelta: {}, the format must be '[+-]00:00(:00(.0...))'", timedelta));
}


/*
 * Transforms double timedelta to a struct clk_t.
 */
Datetime::clk_t
Datetime::timedelta_to_clk_t(double t)
{
	if (isvalidTimedelta(t)) {
		clk_t clk;
		if (t < 0) {
			t *= -1.0;
			clk.tz_s = '-';
		}

		auto _time = static_cast<int>(t);
		clk.hour = _time / 3600;
		if (clk.hour < 100) {
			auto aux = _time - clk.hour * 3600;
			clk.min = aux / 60;
			clk.sec = aux - clk.min * 60;
		} else {
			clk.hour = 99;
			auto aux = _time - clk.hour * 3600;
			clk.min = aux / 60;
			if (clk.min < 100) {
				clk.sec =  aux - clk.min * 60;
			} else {
				clk.min = 99;
				clk.sec =  aux - clk.min * 60;
			}
		}
		clk.fsec = t - _time;

		return clk;
	}

	throw TimedeltaError("Bad serialised timedelta value");
}


double
Datetime::timedelta_to_double(const clk_t& clk)
{
	double t = (clk.hour * 60 + clk.min) * 60 + clk.sec + clk.fsec;
	return clk.tz_s == '-' ? -t : t;
}


std::string
Datetime::timedelta_to_string(const clk_t& clk, bool trim)
{
	if (trim) {
		auto res = std::format("{}{:02}:{:02}:{:02}",
			clk.tz_s, clk.hour, clk.min, clk.sec);
		if (clk.fsec > 0) {
			res += std::format("{:.6f}", clk.fsec).erase(0, 1);
			auto it_e = res.end();
			auto it = it_e - 1;
			for (; *it == '0'; --it) { }
			if (*it != '.') {
				++it;
			}
			res.erase(it, it_e);
		}
		return res;
	}

	if (clk.fsec > 0) {
		return std::format("{}{:02}:{:02}:{:02}",
			clk.tz_s, clk.hour, clk.min, clk.sec);
	}

	return std::format("{}{:02}:{:02}:{:02}.000000",
		clk.tz_s, clk.hour, clk.min, clk.sec);
}


std::string
Datetime::timedelta_to_string(double t, bool trim)
{
	return timedelta_to_string(timedelta_to_clk_t(t), trim);
}


bool
Datetime::isTimedelta(std::string_view timedelta)
{
	auto size = timedelta.size();
	switch (size) {
		case 6: // [+-]00:00
			return (timedelta[0] == '+' || timedelta[0] == '-') && (std::isdigit(timedelta[1]) != 0) && (std::isdigit(timedelta[2]) != 0) &&
				timedelta[3] == ':' && (std::isdigit(timedelta[4]) != 0) && (std::isdigit(timedelta[5]) != 0);
		case 9: // [+-]00:00:00
			return (timedelta[0] == '+' || timedelta[0] == '-') && (std::isdigit(timedelta[1]) != 0) && (std::isdigit(timedelta[2]) != 0) &&
				timedelta[3] == ':' && (std::isdigit(timedelta[4]) != 0) && (std::isdigit(timedelta[5]) != 0) &&
				timedelta[6] == ':' && (std::isdigit(timedelta[7]) != 0) && (std::isdigit(timedelta[8]) != 0);
		default: // [+-]00:00:00  [+-]00:00:00.000...
			if (size > 10 && (timedelta[0] == '+' || timedelta[0] == '-') && (std::isdigit(timedelta[1]) != 0) &&
				(std::isdigit(timedelta[2]) != 0) && timedelta[3] == ':' && (std::isdigit(timedelta[4]) != 0) &&
				(std::isdigit(timedelta[5]) != 0) && timedelta[6] == ':' && (std::isdigit(timedelta[7]) != 0) &&
				(std::isdigit(timedelta[8]) != 0) && timedelta[9] == '.') {
				for (size_t i = 10; i < size; ++i) {
					if (std::isdigit(timedelta[i]) == 0) {
						return false;
					}
				}
				return true;
			}
			return false;
	}
}
