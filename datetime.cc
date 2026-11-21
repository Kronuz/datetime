// Datetime parsing: the ISO 8601 and loose-format scanners, the parsers and the date and datetime predicates.
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

#include <cctype>                                 // for std::isdigit
#include <format>                                 // for std::format
#include <stdexcept>                              // for invalid_argument, out_of_range
#include <string_view>                            // for std::string_view
#include "datetime_detail.h"                      // for the shared helpers


const std::regex Datetime::date_re(R"(([0-9]{4})([-/ ]?)(0[1-9]|1[0-2])\2(0[0-9]|[12][0-9]|3[01])([T ]?([01]?[0-9]|2[0-3]):([0-5][0-9])(:([0-5][0-9])([.,]([0-9]+))?)?([ ]*[+-]([01]?[0-9]|2[0-3]):([0-5][0-9])|Z)?)?([ ]*\|\|[ ]*([+-/\dyMwdhms]+))?)", std::regex::optimize);
const std::regex Datetime::date_math_re("([+-]\\d+|\\/{1,2})([dyMwhms])", std::regex::optimize);


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
