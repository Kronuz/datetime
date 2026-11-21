// Calendar arithmetic: leap years, month lengths, ordinals, timegm and the timestamp conversions.
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

#include <cassert>                                // for assert
#include <climits>                                // for INT_MIN, INT_MAX
#include <cstdint>                                // for std::int64_t
#include <cmath>                                  // for ceil
#include "datetime_detail.h"                      // for the shared helpers



static constexpr int days[2][12] = {
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
};


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
