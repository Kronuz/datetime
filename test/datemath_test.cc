// Date-math and calendar checks against independent references.
//
//  0. See ref.h for the references. Months (3), loose offsets (4), pre-1970 fractions (5),
//     normalizeMonths (6) and toordinal/timegm/timestamp validation (7) are checked below.
//  1. Exhaustive over years 1..9999: a brute-force day counter (increment the
//     date one day at a time with the Gregorian leap rule) is the oracle for
//     toordinal, timegm, timestamp, to_tm_t and the round trips.
//  2. computeDateMath / processDateMath with huge +N / -N terms (up to the
//     int limits) against a days-from-civil reference (Howard Hinnant's
//     published algorithm, written here from its description) in 128-bit
//     arithmetic. Results in the supported years (1..INT_MAX) must match; a
//     step that leaves them must throw DatetimeError. Run it under UBSan:
//     arithmetic. Run it under UBSan: these terms overflow `int`.
//
// Usage: datetime_datemath_test [iterations [seed]]
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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


static i128 ref_seconds(i128 y, int m, i128 d, int h, int mi, int s) {
	return ref_days(y, m, d) * 86400 + h * 3600 + mi * 60 + s;
}

static std::string error_of(auto&& f) {
	try { f(); } catch (const DatetimeError& e) { return e.what(); }
	return "";
}

// 5. The fraction of a timestamp is the distance above the whole second, before 1970 as
// after: timestamp(23:59:58.9) is -1.1, not -2.9, so instants rise with the clock; to_tm_t
// is its inverse. The oracle is days-from-civil (ref.h) for the whole seconds. Fractions of
// an eighth are exact in a double at every supported year, so the results are compared for
// equality; microsecond fractions near the epoch are compared through the round trip.
static void fractions_before_1970() {
	// The instants of 1969-12-31 around the last second, in time order.
	{
		Datetime::tm_t a(1969, 12, 31, 23, 59, 58, 0.9), c(1969, 12, 31, 23, 59, 59, 0.0), b(1969, 12, 31, 23, 59, 59, 0.1);
		const double ta = Datetime::timestamp(a), tc = Datetime::timestamp(c), tb = Datetime::timestamp(b);
		EXPECT(ta < tc && tc < tb, "pre-epoch instants out of order: %.6f %.6f %.6f", ta, tc, tb);
		EXPECT(std::fabs(ta + 1.1) < 1e-9 && tc == -1.0 && std::fabs(tb + 0.9) < 1e-9, "pre-epoch values %.9f %.9f %.9f", ta, tc, tb);
	}
	const double eighths[] = { 0.0, 0.125, 0.25, 0.5, 0.75, 0.875 };
	const int dates[][3] = { { 1, 1, 1 }, { 1, 12, 31 }, { 1600, 2, 29 }, { 1900, 3, 1 }, { 1969, 12, 30 }, { 1969, 12, 31 }, { 1970, 1, 1 }, { 1970, 1, 2 },
		{ 2000, 2, 29 }, { 2038, 1, 19 }, { 2400, 12, 31 }, { 9999, 12, 31 } };
	const int times[][3] = { { 0, 0, 0 }, { 0, 0, 1 }, { 12, 0, 0 }, { 23, 59, 58 }, { 23, 59, 59 } };
	for (const auto& d : dates) {
		for (const auto& hms : times) {
			for (double f : eighths) {
				Datetime::tm_t tm(d[0], d[1], d[2], hms[0], hms[1], hms[2], f);
				const i128 secs = ref_seconds(d[0], d[1], d[2], hms[0], hms[1], hms[2]);
				const double want = static_cast<double>(secs) + f;
				EXPECT(Datetime::timestamp(tm) == want, "timestamp %d-%d-%d %d:%d:%d + %g: %.9f, expected %.9f", d[0], d[1], d[2], hms[0], hms[1], hms[2], f, Datetime::timestamp(tm), want);
				const auto back = Datetime::to_tm_t(want);
				EXPECT(back.year == d[0] && back.mon == d[1] && back.day == d[2] && back.hour == hms[0] && back.min == hms[1] && back.sec == hms[2] && back.fsec == f,
					"to_tm_t(%.9f) -> %d-%d-%d %d:%d:%d + %g, expected %d-%d-%d %d:%d:%d + %g", want, back.year, back.mon, back.day, back.hour, back.min, back.sec, back.fsec,
					d[0], d[1], d[2], hms[0], hms[1], hms[2], f);
			}
		}
	}
	// Timestamps just below zero.
	struct Case { double ts; int hour, min, sec; double fsec; int day; };
	const Case cases[] = { { -0.5, 23, 59, 59, 0.5, 31 }, { -1.0, 23, 59, 59, 0.0, 31 }, { -1.25, 23, 59, 58, 0.75, 31 }, { -1e-7, 23, 59, 59, 0.999999, 31 },
		{ -86400.25, 23, 59, 59, 0.75, 30 }, { -86400.0, 0, 0, 0, 0.0, 31 }, { -0.0, 0, 0, 0, 0.0, 1 } };
	for (const auto& c : cases) {
		const auto t = Datetime::to_tm_t(c.ts);
		const bool jan = c.day == 1;
		EXPECT(t.year == (jan ? 1970 : 1969) && t.mon == (jan ? 1 : 12) && t.day == c.day && t.hour == c.hour && t.min == c.min && t.sec == c.sec && t.fsec == c.fsec,
			"to_tm_t(%.9g) -> %d-%d-%d %d:%d:%d + %.9g", c.ts, t.year, t.mon, t.day, t.hour, t.min, t.sec, t.fsec);
	}
	// Random timestamps around the epoch: the fields come from the reference calendar,
	// and a microsecond fraction survives the round trip.
	for (int i = 0; i < 200000; ++i) {
		const std::int64_t secs = static_cast<std::int64_t>(rnd(6000000000ULL)) - 3000000000LL;
		const double f = static_cast<double>(rnd(1000000)) / 1e6;
		const auto t = Datetime::to_tm_t(static_cast<double>(secs) + f);
		i128 y;
		int m, d;
		const i128 z = floordiv(secs, 86400);
		ref_civil(z, y, m, d);
		const int sod = static_cast<int>(secs - z * 86400);
		EXPECT(t.year == y && t.mon == m && t.day == d && t.hour == sod / 3600 && t.min == sod % 3600 / 60 && t.sec == sod % 60 && std::fabs(t.fsec - f) < 1e-9,
			"to_tm_t(%lld + %.6f) -> %d-%d-%d %d:%d:%d + %.9f", static_cast<long long>(secs), f, t.year, t.mon, t.day, t.hour, t.min, t.sec, t.fsec);
		Datetime::tm_t back = t;
		EXPECT(std::fabs(Datetime::timestamp(back) - (static_cast<double>(secs) + f)) < 1e-6, "round trip of %lld + %.6f", static_cast<long long>(secs), f);
	}
}

// 6. normalizeMonths: any int month goes to 1..12 and the whole years are carried in 64-bit
// arithmetic, in constant time. A year that leaves int throws and changes nothing.
static void normalize_months() {
	const int years[] = { 1, 2020, INT_MAX, INT_MAX - 1, INT_MAX - 178956970, 0, -7, INT_MIN, INT_MIN + 1 };
	const int months[] = { INT_MIN, INT_MIN + 1, -2147483000, -12, -1, 0, 1, 12, 13, 24, 25, 2147483000, INT_MAX - 1, INT_MAX };
	for (int year : years) {
		for (int mon : months) {
			const i128 k = floordiv(static_cast<i128>(mon) - 1, 12);
			const i128 want_year = static_cast<i128>(year) + k;
			const int want_mon = static_cast<int>(mon - 12 * k);
			int y = year, m = mon;
			const auto err = error_of([&] { Datetime::normalizeMonths(y, m); });
			if (want_year < INT_MIN || want_year > INT_MAX) {
				EXPECT(err == "Year is out of range" && y == year && m == mon, "normalizeMonths(%d, %d) must throw and leave its arguments, got '%s' %d/%d", year, mon, err.c_str(), y, m);
			} else {
				EXPECT(err.empty() && y == want_year && m == want_mon && m >= 1 && m <= 12, "normalizeMonths(%d, %d) -> %d/%d, expected %lld/%d", year, mon, y, m, static_cast<long long>(want_year), want_mon);
			}
		}
	}
	for (int i = 0; i < 100000; ++i) {
		const int year = static_cast<int>(rng());
		const int mon = static_cast<int>(rng());
		const i128 k = floordiv(static_cast<i128>(mon) - 1, 12);
		const i128 want_year = static_cast<i128>(year) + k;
		int y = year, m = mon;
		const bool threw = !error_of([&] { Datetime::normalizeMonths(y, m); }).empty();
		const bool fits = want_year >= INT_MIN && want_year <= INT_MAX;
		EXPECT(threw == !fits && (threw || (y == want_year && m == mon - 12 * k)), "normalizeMonths(%d, %d) random", year, mon);
	}
	// Constant time: a loop of one iteration per twelve months would take seconds here.
	const auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < 100; ++i) {
		int y = 2000, m = INT_MIN + i;
		Datetime::normalizeMonths(y, m);
		EXPECT(m >= 1 && m <= 12, "normalizeMonths(INT_MIN + %d)", i);
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	EXPECT(seconds < 0.5, "normalizeMonths took %.3f s for 100 extreme months", seconds);
}

// 7. toordinal, timegm and timestamp validate as before and count days from the reference
// calendar. The exceptions and their order are those of the original: the year first, then
// a day below 1, then the month, then the day against the month length.
static void ordinal_validation() {
	using Datetime::toordinal;
	EXPECT(error_of([] { toordinal(0, 1, 1); }) == "Year is out of range", "year 0");
	EXPECT(error_of([] { toordinal(-5, 13, 40); }) == "Year is out of range", "year -5 wins over the month");
	EXPECT(error_of([] { toordinal(2020, 0, 0); }) == "Day is out of range for month", "day 0 wins over the month");
	EXPECT(error_of([] { toordinal(2020, 0, 1); }) == "Month must be in 1..12", "month 0");
	EXPECT(error_of([] { toordinal(2020, 13, 1); }) == "Month must be in 1..12", "month 13");
	EXPECT(error_of([] { toordinal(2020, 13, 99); }) == "Month must be in 1..12", "month 13 wins over a long day");
	EXPECT(error_of([] { toordinal(2021, 2, 29); }) == "Day is out of range for month", "2021-02-29");
	EXPECT(error_of([] { toordinal(2020, 2, 30); }) == "Day is out of range for month", "2020-02-30");
	EXPECT(error_of([] { toordinal(2100, 2, 29); }) == "Day is out of range for month", "2100-02-29");
	EXPECT(error_of([] { toordinal(2020, 4, 31); }) == "Day is out of range for month", "04-31");
	EXPECT(error_of([] { toordinal(2000, 2, 29); }).empty() && error_of([] { toordinal(2020, 12, 31); }).empty(), "valid last days");
	EXPECT(toordinal(1, 1, 1) == 1 && toordinal(1970, 1, 1) == DATETIME_EPOCH_ORD && toordinal(9999, 12, 31) == 3652059, "known ordinals");
	EXPECT(error_of([] { Datetime::tm_t t(0, 1, 1); Datetime::timegm(t); }) == "Year is out of range", "timegm year 0");
	EXPECT(error_of([] { Datetime::tm_t t(0, 1, 1); Datetime::timestamp(t); }) == "Year is out of range", "timestamp year 0");
	EXPECT(error_of([] { std::tm t{}; t.tm_year = -1900; t.tm_mon = 0; t.tm_mday = 1; Datetime::timegm(t); }) == "Year is out of range", "std::tm year 0");
	// A month outside 1..12 is carried into the year, and the day is a linear offset.
	{
		Datetime::tm_t t(2020, 14, 40, 1, 2, 3);
		const std::time_t got = Datetime::timegm(t);
		EXPECT(t.year == 2021 && t.mon == 2, "timegm normalizes the month in place: %d-%d", t.year, t.mon);
		EXPECT(got == ref_seconds(2021, 2, 40, 1, 2, 3), "timegm(2020-14-40 01:02:03)");
		std::tm s{};
		s.tm_year = 120; s.tm_mon = -3; s.tm_mday = -5; s.tm_hour = 25; s.tm_min = -1; s.tm_sec = 61;
		EXPECT(Datetime::timegm(s) == ref_seconds(2019, 10, -5, 25, -1, 61), "timegm(std::tm) with out-of-range fields");
	}
	// Years above the range where a double holds the seconds exactly: the whole seconds are exact.
	{
		Datetime::tm_t t(INT_MAX, 12, 31, 23, 59, 59);
		EXPECT(static_cast<i128>(Datetime::timegm(t)) == ref_seconds(INT_MAX, 12, 31, 23, 59, 59), "timegm(INT_MAX-12-31)");
		const double want = static_cast<double>(ref_seconds(INT_MAX, 12, 31, 23, 59, 59));
		EXPECT(Datetime::timestamp(t) == want, "timestamp(INT_MAX-12-31): %.1f vs %.1f", Datetime::timestamp(t), want);
	}
}


int main(int argc, char** argv) {
	const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
	if (argc > 2) { rng.seed(std::strtoull(argv[2], nullptr, 10)); }
	exhaustive_years();
	huge_terms(n);
	month_arithmetic();
	loose_offset_is_utc();
	fractions_before_1970();
	normalize_months();
	ordinal_validation();
	std::printf("%llu checks, %llu failures\n", static_cast<unsigned long long>(checks), static_cast<unsigned long long>(failures));
	return failures == 0 ? 0 : 1;
}
