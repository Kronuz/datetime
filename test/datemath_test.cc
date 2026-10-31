// Date-math and calendar checks against independent references.
//
//  0. See ref.h for the references. Months (3) and loose offsets (4) are checked below.
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
#include "ref.h"

static std::mt19937_64 rng(0xda7e3a7bULL);
static std::uint64_t checks = 0, failures = 0;
static std::uint64_t rnd(std::uint64_t n) { return rng() % n; }

#define EXPECT(cond, ...) do { ++checks; if (!(cond)) { ++failures; if (failures <= 25) { std::printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } } while (0)

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


// 3. Months: every start month, several years (leap and century edges), every
// day-of-month edge, and N in -240..240 (including all multiples of 12), against
// the month arithmetic model with end-of-month clamping; and normalizeMonths
// itself over -500..500.
static void month_arithmetic() {
	for (int y : { 1, 2, 4, 100, 399, 400, 1900, 1999, 2000, 2020, 2023, 2024, 2100, 9998, 9999 }) {
		for (int m = 1; m <= 12; ++m) {
			for (int d : { 1, 15, 28, 29, 30, 31 }) {
				if (d > dim(y, m)) { continue; }
				Datetime::tm_t base(y, m, d, 12, 30, 15);
				for (int n = -240; n <= 240; ++n) { check_term(base, n, 'M'); }
			}
		}
	}
	// The case from the report, spelled out.
	auto t = Datetime::DatetimeParser(std::string_view("2020-12-15||+12M"));
	EXPECT(t.year == 2021 && t.mon == 12 && t.day == 15, "2020-12-15 +12M -> %d-%d-%d", t.year, t.mon, t.day);
	auto c = Datetime::DatetimeParser(std::string_view("2020-01-31||+1M"));
	EXPECT(c.year == 2020 && c.mon == 2 && c.day == 29, "2020-01-31 +1M clamps to Feb 29");
	for (int mon = -500; mon <= 500; ++mon) {
		int year = 2000, mo = mon;
		Datetime::normalizeMonths(year, mo);
		const int k = static_cast<int>(floordiv(mon - 1, 12));
		EXPECT(mo == mon - 12 * k && mo >= 1 && mo <= 12 && year == 2000 + k, "normalizeMonths(%d) -> %d/%d", mon, year, mo);
	}
}

// 4. A loose-format offset makes the result UTC, as the ISO form does.
static void loose_offset_is_utc() {
	for (const char* s : { "2020/01/02 03:04:05+05:30", "2020 01 02 3:04 -02:00", "2020/01/02 03:04:05.5 +00:00" }) {
		EXPECT(Datetime::DatetimeParser(std::string_view(s)).is_utc, "%s is_utc", s);
	}
	EXPECT(!Datetime::DatetimeParser(std::string_view("2020/01/02 03:04:05")).is_utc, "no offset: not UTC");
	EXPECT(!Datetime::DatetimeParser(std::string_view("2020/01/02")).is_utc, "date only: not UTC");
	EXPECT(Datetime::DatetimeParser(std::string_view("2020-01-02T03:04:05+05:30")).is_utc, "ISO offset is UTC");
}


int main(int argc, char** argv) {
	const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
	if (argc > 2) { rng.seed(std::strtoull(argv[2], nullptr, 10)); }
	exhaustive_years();
	huge_terms(n);
	month_arithmetic();
	loose_offset_is_utc();
	std::printf("%llu checks, %llu failures\n", static_cast<unsigned long long>(checks), static_cast<unsigned long long>(failures));
	return failures == 0 ? 0 : 1;
}
