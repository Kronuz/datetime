// Independent references shared by the date-math tests: Howard Hinnant's
// days-from-civil (written from its published description) in 128-bit
// arithmetic, and a month/second arithmetic model of one date-math term.
#pragma once
#include <climits>
#include <cstdint>

#include "datetime.h"

using i128 = __int128;

inline i128 floordiv(i128 a, i128 b) { i128 q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

// Hinnant: days since 1970-01-01 of a proleptic Gregorian date. Month 1..12;
// the day may exceed the month (it is added linearly).
inline i128 ref_days(i128 y, int m, i128 d) {
	y -= m <= 2;
	const i128 era = floordiv(y, 400);
	const i128 yoe = y - era * 400;
	const i128 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const i128 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

inline void ref_civil(i128 z, i128& y, int& m, int& d) {
	z += 719468;
	const i128 era = floordiv(z, 146097);
	const i128 doe = z - era * 146097;
	const i128 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = yoe + era * 400;
	const i128 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const i128 mp = (5 * doy + 2) / 153;
	d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
	m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
	y += m <= 2;
}

inline bool leap(i128 y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
inline int dim(i128 y, int m) { static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; return d[m - 1] + (m == 2 && leap(y)); }


struct Expect {
	bool throws = false;
	i128 y = 0;
	int mon = 0, day = 0, hour = 0, min = 0, sec = 0;
};

// n units of `unit` added to a valid base. Mirrors the documented semantics:
// y/M move the field (M clamps the day to the new month's length), w/d/h/m/s add
// linearly; the supported years are 1..INT_MAX.
inline Expect ref_add(const Datetime::tm_t& b, i128 n, char unit) {
	Expect e;
	i128 y = b.year;
	int mo = b.mon;
	i128 d = b.day, h = b.hour, mi = b.min, s = b.sec;
	switch (unit) {
		case 'y': y += n; break;
		case 'M': {
			const i128 total = y * 12 + (b.mon - 1) + n;
			y = floordiv(total, 12);
			mo = static_cast<int>(total - y * 12) + 1;
			if (y >= 1 && y <= INT_MAX && d > dim(y, mo)) { d = dim(y, mo); }
			break;
		}
		case 'w': d += 7 * n; break;
		case 'd': d += n; break;
		case 'h': h += n; break;
		case 'm': mi += n; break;
		default: s += n; break;
	}
	if (y < 1 || y > INT_MAX) { e.throws = true; return e; }
	const i128 t = ref_days(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
	i128 ry;
	int rm, rd;
	const i128 z = floordiv(t, 86400);
	ref_civil(z, ry, rm, rd);
	if (ry > INT_MAX) { e.throws = true; return e; }
	const int rem = static_cast<int>(t - z * 86400);
	e.y = ry; e.mon = rm; e.day = rd; e.hour = rem / 3600; e.min = rem % 3600 / 60; e.sec = rem % 60;
	return e;
}

