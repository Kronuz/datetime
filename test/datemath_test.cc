// Date-math and calendar checks against independent references.
//
//  1. Exhaustive over years 1..9999: a brute-force day counter (increment the
//     date one day at a time with the Gregorian leap rule) is the oracle for
//     toordinal, timegm, timestamp, to_tm_t and the round trips.
//  2. computeDateMath / processDateMath with huge +N / -N terms (up to the
//     int limits) against a days-from-civil reference (Howard Hinnant's
//     published algorithm, written here from its description) in 128-bit
//     arithmetic. Results in the supported years (1..INT_MAX) must match; a
//     step that leaves them must throw DatetimeError. Run it under UBSan:
//     before the 64-bit rewrite these terms overflowed int.
//
// Usage: datetime_datemath_test [iterations [seed]]
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include "datetime.h"

static std::mt19937_64 rng(0xda7e3a7bULL);
static std::uint64_t checks = 0, failures = 0;
static std::uint64_t rnd(std::uint64_t n) { return rng() % n; }

#define EXPECT(cond, ...) do { ++checks; if (!(cond)) { ++failures; if (failures <= 25) { std::printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } } while (0)

using i128 = __int128;

static i128 floordiv(i128 a, i128 b) { i128 q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }

// Hinnant: days since 1970-01-01 of a proleptic Gregorian date. Month 1..12;
// the day may exceed the month (it is added linearly).
static i128 ref_days(i128 y, int m, i128 d) {
	y -= m <= 2;
	const i128 era = floordiv(y, 400);
	const i128 yoe = y - era * 400;
	const i128 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const i128 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void ref_civil(i128 z, i128& y, int& m, int& d) {
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

static bool leap(i128 y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
static int dim(i128 y, int m) { static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; return d[m - 1] + (m == 2 && leap(y)); }


// 1. Exhaustive years 1..9999 against a brute-force day counter.
static void exhaustive_years() {
	std::int64_t count = 0;  // days since 0001-01-01
	for (int y = 1; y <= 9999; ++y) {
		for (int m = 1; m <= 12; ++m) {
			for (int d = 1; d <= dim(y, m); ++d, ++count) {
				const std::int64_t unix_days = count - 719162;
				EXPECT(Datetime::toordinal(y, m, d) == count + 1, "toordinal %d-%d-%d", y, m, d);
				EXPECT(ref_days(y, m, d) == unix_days, "reference %d-%d-%d", y, m, d);
				for (int sod : { 0, 1, 43200, 86399 }) {
					const std::int64_t t = unix_days * 86400 + sod;
					Datetime::tm_t tm(y, m, d, sod / 3600, sod % 3600 / 60, sod % 60);
					EXPECT(Datetime::timegm(tm) == t, "timegm %d-%d-%d +%d", y, m, d, sod);
					EXPECT(Datetime::timestamp(tm) == static_cast<double>(t), "timestamp %d-%d-%d +%d", y, m, d, sod);
					auto back = Datetime::to_tm_t(static_cast<std::time_t>(t));
					EXPECT(back.year == y && back.mon == m && back.day == d && back.hour == sod / 3600 && back.min == sod % 3600 / 60 && back.sec == sod % 60,
						"to_tm_t %d-%d-%d +%d -> %d-%d-%d %d:%d:%d", y, m, d, sod, back.year, back.mon, back.day, back.hour, back.min, back.sec);
				}
			}
		}
	}
	EXPECT(count == 3652059, "total days 0001..9999 = %lld", static_cast<long long>(count));
}


// 2. Date math with huge terms against the reference.
struct Expect {
	bool throws = false;
	i128 y = 0;
	int mon = 0, day = 0, hour = 0, min = 0, sec = 0;
};

// n units of `unit` added to a valid base. Mirrors the documented semantics:
// y/M move the field (M clamps the day to the new month's length), w/d/h/m/s add
// linearly; the supported years are 1..INT_MAX.
static Expect ref_add(const Datetime::tm_t& b, i128 n, char unit) {
	Expect e;
	i128 y = b.year;
	int mo = b.mon;
	i128 d = b.day, h = b.hour, mi = b.min, s = b.sec;
	switch (unit) {
		case 'y': y += n; break;
		case 'M': {
			const i128 sum = static_cast<i128>(b.mon) + n;
			if (sum > 12 && sum % 12 == 0) { e.throws = true; return e; }  // documented quirk: month 0
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

static void check_term(const Datetime::tm_t& base, i128 n, char unit) {
	const Expect e = ref_add(base, n, unit);
	Datetime::tm_t t = base;
	std::string op = (n < 0 ? "-" : "+") + std::to_string(static_cast<long long>(n < 0 ? -n : n));
	bool threw = false;
	try {
		Datetime::computeDateMath(t, op, unit);
	} catch (const DatetimeError&) {
		threw = true;
	}
	EXPECT(threw == e.throws, "%s%c on %d-%d-%d %d:%d:%d: threw=%d expected %d", op.c_str(), unit, base.year, base.mon, base.day, base.hour, base.min, base.sec, threw, e.throws);
	if (!threw && !e.throws) {
		EXPECT(t.year == e.y && t.mon == e.mon && t.day == e.day && t.hour == e.hour && t.min == e.min && t.sec == e.sec,
			"%s%c on %d-%d-%d %d:%d:%d -> %d-%d-%d %d:%d:%d, expected %lld-%d-%d %d:%d:%d", op.c_str(), unit, base.year, base.mon, base.day, base.hour, base.min, base.sec,
			t.year, t.mon, t.day, t.hour, t.min, t.sec, static_cast<long long>(e.y), e.mon, e.day, e.hour, e.min, e.sec);
	}
}

static Datetime::tm_t rand_base() {
	Datetime::tm_t t;
	const auto pick = rnd(8);
	t.year = pick == 0 ? 1 : pick == 1 ? 9999 : pick == 2 ? INT_MAX - static_cast<int>(rnd(3)) : static_cast<int>(1 + rnd(10000));
	t.mon = static_cast<int>(1 + rnd(12));
	t.day = static_cast<int>(1 + rnd(dim(t.year, t.mon)));
	t.hour = static_cast<int>(rnd(24));
	t.min = static_cast<int>(rnd(60));
	t.sec = static_cast<int>(rnd(60));
	return t;
}

static void huge_terms(std::uint64_t iterations) {
	const i128 edges[] = { 0, 1, 11, 12, 13, 24, 365, 366, 2147483647LL, 2147483646LL, 2147483645LL, 1073741824LL, 306783378LL, 306783379LL, 357913941LL, 357913942LL, 35791394LL, 596523LL, 596524LL, 2147483LL, 2147484LL, 999999999LL };
	const std::string units = "yMwdhms";
	for (std::uint64_t i = 0; i < iterations; ++i) {
		const auto base = rand_base();
		for (char u : units) {
			i128 n;
			switch (rnd(4)) {
				case 0: n = edges[rnd(sizeof(edges) / sizeof(edges[0]))]; break;
				case 1: n = static_cast<i128>(rng() % 2147483648ULL); break;
				case 2: n = static_cast<i128>(rng() % 100000ULL); break;
				default: n = static_cast<i128>(rng() % 10); break;
			}
			if (rnd(2)) { n = -n; }
			check_term(base, n, u);
		}
	}
	// Corners: the extremes of every unit from the first and last supported years.
	for (int y : { 1, 2, 9999, INT_MAX - 1, INT_MAX }) {
		for (char u : units) {
			for (i128 n : { i128(2147483647), i128(-2147483647), i128(1), i128(-1) }) {
				Datetime::tm_t base(y, 12, 31, 23, 59, 59);
				check_term(base, n, u);
				Datetime::tm_t first(y, 1, 1, 0, 0, 0);
				check_term(first, n, u);
			}
		}
	}
	// The terms arrive through processDateMath too, and chains keep working.
	{
		Datetime::tm_t t(2020, 6, 15, 12, 0, 0);
		Datetime::processDateMath("+2147483647d", t);
		i128 ry; int rm, rd;
		ref_civil(ref_days(2020, 6, 15) + 2147483647, ry, rm, rd);
		EXPECT(t.year == ry && t.mon == rm && t.day == rd && t.hour == 12, "2020-06-15 +2147483647d -> %d-%d-%d", t.year, t.mon, t.day);
		Datetime::tm_t u(2020, 6, 15, 12, 0, 0);
		bool threw = false;
		try { Datetime::processDateMath("+2147483647y+2147483647y", u); } catch (const DatetimeError& ex) { threw = std::string(ex.what()) == "Year is out of range"; }
		EXPECT(threw, "2 x +2147483647y must throw 'Year is out of range'");
		Datetime::tm_t v(2020, 6, 15, 12, 0, 0);
		threw = false;
		try { Datetime::processDateMath("-2147483647y", v); } catch (const DatetimeError& ex) { threw = std::string(ex.what()) == "Year is out of range"; }
		EXPECT(threw, "-2147483647y must throw 'Year is out of range'");
	}
}


int main(int argc, char** argv) {
	const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
	if (argc > 2) { rng.seed(std::strtoull(argv[2], nullptr, 10)); }
	exhaustive_years();
	huge_terms(n);
	std::printf("%llu checks, %llu failures\n", static_cast<unsigned long long>(checks), static_cast<unsigned long long>(failures));
	return failures == 0 ? 0 : 1;
}
