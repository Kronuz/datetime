// Date math: the offset and term arithmetic behind processDateMath, computeTimeZone and computeDateMath.
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
#include <climits>                                // for INT_MIN, INT_MAX
#include <cstdint>                                // for std::int64_t
#include <cctype>                                 // for std::isdigit
#include <format>                                 // for std::format
#include <string>                                 // for std::string
#include <string_view>                            // for std::string_view
#include "datetime_detail.h"                      // for the shared helpers


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
