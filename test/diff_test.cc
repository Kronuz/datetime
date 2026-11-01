// Differential test: the hand-written parsers/formatters in datetime.cc against
// the frozen reference implementation in test/old/ (namespace DatetimeOld).
//
// Every public entry point is fed random, grammar-based, mutated and
// exhaustively single-byte-mutated inputs, valid and invalid, and the results
// are compared completely: Format code, every tm_t field (bitwise, including the
// partial writes left by a failing parse), exception message, and formatted
// strings.
//
// The reference and the library differ on nine explicit input classes (the
// reference carries bugs the library fixes, or rides a width limit of strict-stox
// that the library does not). Outside them reference == library
// is enforced completely (verdicts, every tm_t field, exception text, formats):
//  1. Negative hour/min/sec ("2020-01-02T-3:04:05"): the reference's filling
//     overload Iso8601Parser(str, tm) accepts it and its validating one rejects
//     it. The library returns OUT_OF_RANGE from both.
//  2. A fraction dot with no digits ("...:05.", "...:05.Z") or with more digits
//     than a fraction has (63+): the reference's validating overload accepts
//     it and its filling one returns ERROR. The library returns ERROR from both.
//  3. Loose-format strings ending in Z ("2020/01/02 03:04:05Z"): the reference
//     always throws "Invalid format in Datetime Math unit: 'h'. '' must be
//     numeric". The library parses Z as UTC (is_utc = true), as on the ISO path.
//  4. computeDateMath with huge +N terms: the reference overflows int (UB); the
//     library is 64-bit with a "Year is out of range" error. The reference cannot
//     be run on those inputs, so test/datemath_test.cc checks them against a
//     days-from-civil reference instead, and overflow_prone() skips them here.
//  5. A month that is a multiple of 12 above 12 ("2020-12-15||+12M"): the
//     reference's normalizeMonths yields month 0 and it throws "Month must be in
//     1..12". The library yields month 12 of the right year. Oracle: the month
//     arithmetic model in ref.h (single terms), and for chains/parsers that the
//     library never throws that message; a base tm_t whose month is a multiple
//     of 12 above 12 that throws in both is compared with the reference on the
//     base with the month already normalized. test/datemath_test.cc checks all
//     start months and N in -240..240 exhaustively.
//  6. A loose-format offset ("2020/01/02 03:04:05+05:30"): the library sets
//     is_utc, as the ISO path does for an applied offset. Oracle: the
//     reference's result with is_utc forced true.
//  7. A timestamp before 1970 with a fraction (`timestamp(tm_t)`, `to_tm_t(double)`,
//     `iso8601(double)`): the reference subtracted the fraction from a negative whole-second
//     count (23:59:58.9 was -2.9) and truncated a negative timestamp toward zero, dropping
//     its fraction. The library adds the fraction (-1.1) and floors. Oracle: the reference
//     calendar (ref.h) for the whole seconds, and the reference's own whole seconds plus or
//     minus the fraction for `timestamp`.
//  8. A numeric field wider than the grammar allows: the number of a date-math term has at
//     most 63 characters (sign excluded) and a fraction, in a datetime, a time or a
//     timedelta, 1 to 62 digits. Wider is invalid (DatetimeError "... must be numeric",
//     "Error format in ..."). The library checks the widths itself, before a field reaches
//     strict-stox. The reference calls strict-stox with the field, so its verdict is
//     whatever strict-stox's width was: strict-stox up to 21b7941 read 63 bytes and rejected
//     the rest (so reference == library), strict-stox from c1c9bc1 reads any width and the
//     reference accepts "+000...0010d" (64 characters, the value 10) and a 63-digit
//     fraction (so reference != library). No assertion is made about the reference
//     there; the library must equal the oracle on both pins. Oracle: the widths above, as
//     written in docs/API.md (the constants below are the test's own copy), the reference
//     on the terms before a wide one, and the reference's exception text.
//  9. An empty timedelta ("") : the reference reads timedelta[0] of an empty view (out of
//     bounds, an assertion under libstdc++'s hardening); the library throws TimedeltaError
//     ("Error format in timedelta: , ..."). Only the library is run on it.
// The oracle for 1 and 2 is ref_iso() below, a reference verdict written from
// the grammar; it is cross-checked against the reference implementation
// everywhere the two agree, and against both library overloads everywhere. The
// oracle for 3 is the reference parsing the same string with "+00:00" instead of
// "Z", plus is_utc.
//
// Equal on purpose:
//  - Fields accept strtoul's leading whitespace and sign ("+5", " 5").
//  - Iso8601Parser can throw ("Year is out of range") for year 1 with an offset
//    that moves the instant before 0001-01-01, despite returning a Format. The
//    instant is outside the supported years.
//
// Usage: datetime_diff_test [iterations [seed]]   (default 300000 per random family)
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "datetime.h"
#include "old/datetime_old.h"
#include "ref.h"


static std::mt19937_64 rng(0x5eed1234abcdULL);
static std::uint64_t checks = 0;
static std::uint64_t failures = 0;

static std::uint64_t rnd(std::uint64_t n) { return rng() % n; }
static bool chance(unsigned pct) { return rnd(100) < pct; }

// A digest of every input the test generates. The inputs must not depend on the compiler:
// an expression with two draws in unsequenced operands (function arguments, the operands of
// `+`) decides at the compiler's whim which draw goes where. The digest is printed and,
// for the default run, compared with the recorded one, so two toolchains that generate
// different inputs fail instead of testing different things.
static std::uint64_t digest = 0xcbf29ce484222325ULL;
// The digest of the default run, `datetime_diff_test 20000`.
static constexpr std::uint64_t expected_digest_20000 = 0xacbb5fbcb3587e59ULL;
static void feed_u(std::uint64_t v) { digest = (digest ^ v) * 0x100000001b3ULL; }
static void feed_s(std::string_view s) {
	for (unsigned char c : s) { feed_u(c); }
	feed_u(s.size() | (std::uint64_t{1} << 63));
}
template <typename T>
static void feed_tm(const T& t) {
	for (int v : { t.year, t.mon, t.day, t.hour, t.min, t.sec }) { feed_u(static_cast<std::uint32_t>(v)); }
	feed_u(std::bit_cast<std::uint64_t>(t.fsec));
	feed_u(t.is_utc);
}

static std::string show(const std::string& s) {
	std::string r = "\"";
	for (unsigned char c : s) {
		if (c >= 32 && c < 127 && c != '"' && c != '\\') { r += static_cast<char>(c); }
		else { char b[8]; std::snprintf(b, sizeof(b), "\\x%02x", c); r += b; }
	}
	return r + "\"";
}

// An exception message as what() returns it: a C string, so it ends at the first NUL.
static std::string as_what(std::string m) {
	m.resize(std::strlen(m.c_str()));
	return m;
}

static void fail(const char* what, const std::string& input, const std::string& detail) {
	++failures;
	if (failures <= 25) {
		std::printf("MISMATCH %s input=%s %s\n", what, show(input).c_str(), detail.c_str());
	}
}


struct Tm {
	int year, mon, day, hour, min, sec;
	std::uint64_t fsec_bits;
	bool is_utc;
	bool operator==(const Tm& o) const { return std::memcmp(this, &o, sizeof(Tm)) == 0 && true; }
};

template <typename T>
static Tm pack(const T& t) {
	Tm r;
	std::memset(&r, 0, sizeof(r));
	r.year = t.year; r.mon = t.mon; r.day = t.day; r.hour = t.hour; r.min = t.min; r.sec = t.sec;
	r.fsec_bits = std::bit_cast<std::uint64_t>(t.fsec);
	r.is_utc = t.is_utc;
	return r;
}

static std::string show(const Tm& t) {
	char b[160];
	std::snprintf(b, sizeof(b), "{%d-%d-%d %d:%d:%d fsec=%.17g utc=%d}", t.year, t.mon, t.day, t.hour, t.min, t.sec,
		std::bit_cast<double>(t.fsec_bits), t.is_utc);
	return b;
}

template <typename T>
static T sentinel(unsigned variant) {
	T t;
	t.year = -777; t.mon = -7; t.day = -7; t.hour = -7; t.min = -7; t.sec = -7;
	t.fsec = -5.5;
	t.is_utc = variant & 1;
	return t;
}

// Outcome of a call: exception message ("" = none) plus a payload.
struct Outcome {
	bool threw = false;
	std::string msg;
	std::string payload;
};

template <typename F>
static Outcome run(F&& f) {
	Outcome o;
	try {
		o.payload = f();
	} catch (const std::exception& e) {
		o.threw = true;
		o.msg = e.what();
	}
	return o;
}

static void compare(const char* what, const std::string& input, const Outcome& a, const Outcome& b) {
	++checks;
	if (a.threw != b.threw || a.msg != b.msg || a.payload != b.payload) {
		fail(what, input, "old=[" + (a.threw ? "throw " + a.msg : a.payload) + "] new=[" + (b.threw ? "throw " + b.msg : b.payload) + "]");
	}
}


// Date-math terms with 7..10 significant digits can overflow int inside the
// (unchanged, original) computeDateMath arithmetic: UB in both implementations.
// Out-of-int-range terms (11+ digits) fail cleanly and stay covered.
static bool overflow_prone(const std::string& s) {
	for (std::size_t i = 0; i < s.size(); ++i) {
		if (s[i] != '+' && s[i] != '-') { continue; }
		std::size_t j = i + 1;
		while (j < s.size() && s[j] == '0') { ++j; }
		std::size_t k = j;
		while (k < s.size() && s[k] >= '0' && s[k] <= '9') { ++k; }
		if (k - j >= 7 && k - j <= 10) { return true; }
	}
	return false;
}

using Fmt = Datetime::Format;

// Class 8: the widths of the grammar, as docs/API.md states them (the test's own copy).
static constexpr std::size_t grammar_term_chars = 63;       // the number of a date-math term, sign excluded
static constexpr std::size_t grammar_fraction_digits = 62;  // a fraction has 1 to 62 digits
static std::uint64_t class_wide_term = 0, class_wide_frac = 0, class_empty = 0;

// Whether a fraction separator in `s` is followed by more digits than a fraction has.
static bool wide_fraction(std::string_view s) {
	for (std::size_t i = 0; i < s.size(); ++i) {
		if (s[i] != '.' && s[i] != ',') { continue; }
		std::size_t j = i + 1;
		while (j < s.size() && s[j] >= '0' && s[j] <= '9') { ++j; }
		if (j - i - 1 > grammar_fraction_digits) { return true; }
	}
	return false;
}

// Whether `s` has a run of digits longer than the number of a date-math term.
static bool wide_term_digits(std::string_view s) {
	std::size_t run = 0;
	for (char c : s) {
		run = c >= '0' && c <= '9' ? run + 1 : 0;
		if (run > grammar_term_chars) { return true; }
	}
	return false;
}

// Reference ISO 8601 verdict, from the grammar (shape keyed on the length, as
// the documented formats are):
//   YYYY-MM-DD | YYYY-MM-DD(T| )hh:mm:ss[Z | .f+[Z|+hh:mm|-hh:mm] | +hh:mm | -hh:mm]
// A field is strtoul-style (optional whitespace and sign, then digits only).
// Every field must be in range (a negative one is out of range) and a
// fraction needs 1..62 digits.
static bool ref_field(std::string_view s, std::size_t pos, std::size_t n, int& v) {
	std::size_t i = pos, end = pos + n;
	while (i < end && (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))) { ++i; }
	int sign = 1;
	if (i < end && (s[i] == '+' || s[i] == '-')) { sign = s[i] == '-' ? -1 : 1; ++i; }
	if (i == end) { return false; }
	int acc = 0;
	for (; i < end; ++i) {
		if (s[i] < '0' || s[i] > '9') { return false; }
		acc = acc * 10 + (s[i] - '0');
	}
	v = sign * acc;
	return true;
}

static bool ref_leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

static Fmt ref_iso(std::string_view s) {
	const std::size_t n = s.size();
	if (n != 10 && n < 19) { return Fmt::INVALID; }
	if (s[4] != '-' || s[7] != '-') { return Fmt::INVALID; }
	if (n > 10 && ((s[10] != 'T' && s[10] != ' ') || s[13] != ':' || s[16] != ':')) { return Fmt::INVALID; }
	if (n == 20 && s[19] != 'Z') { return Fmt::INVALID; }
	int Y, M, D;
	if (!ref_field(s, 0, 4, Y) || !ref_field(s, 5, 2, M) || !ref_field(s, 8, 2, D)) { return Fmt::ERROR; }
	static const int dim[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (Y < 1 || M < 1 || M > 12 || D < 1 || D > dim[M - 1] + (M == 2 && ref_leap(Y))) { return Fmt::OUT_OF_RANGE; }
	if (n == 10) { return Fmt::VALID; }
	const std::size_t pos[] = { 11, 14, 17 };
	const int limit[] = { 24, 60, 60 };
	for (int k = 0; k < 3; ++k) {
		int v;
		if (!ref_field(s, pos[k], 2, v)) { return Fmt::ERROR; }
		if (v < 0 || v >= limit[k]) { return Fmt::OUT_OF_RANGE; }
	}
	if (n == 19 || n == 20) { return Fmt::VALID; }
	auto offset = [&](std::size_t at) {  // hh:mm at `at`
		int h, m;
		if (!ref_field(s, at, 2, h)) { return Fmt::ERROR; }
		if (h < 0 || h >= 24) { return Fmt::OUT_OF_RANGE; }
		if (!ref_field(s, at + 3, 2, m)) { return Fmt::ERROR; }
		if (m < 0 || m >= 60) { return Fmt::OUT_OF_RANGE; }
		return Fmt::VALID;
	};
	if (s[19] == '+' || s[19] == '-') {
		return n == 25 && s[22] == ':' ? offset(20) : Fmt::INVALID;
	}
	if (s[19] != '.') { return Fmt::INVALID; }
	std::size_t i = 20;
	while (i < n && s[i] >= '0' && s[i] <= '9') { ++i; }
	const std::size_t nd = i - 20;
	Fmt rest = Fmt::VALID;
	if (i < n) {
		if (s[i] == 'Z') {
			if (i + 1 != n) { return Fmt::ERROR; }
		} else if (s[i] == '+' || s[i] == '-') {
			if (n - i != 6 || s[i + 3] != ':') { return Fmt::INVALID; }
			rest = offset(i + 1);
		} else {
			return Fmt::INVALID;
		}
	}
	if (rest != Fmt::VALID) { return rest; }
	return nd >= 1 && nd <= grammar_fraction_digits ? Fmt::VALID : Fmt::ERROR;
}

static std::uint64_t class_neg = 0, class_frac = 0, class_z = 0, class_month = 0, class_off = 0, class_pre = 0;
static const char* const month_msg = "Month must be in 1..12";

static void check_string(const std::string& s) {
	feed_s(s);
	if (overflow_prone(s)) { return; }
	std::string_view sv(s);

	// New overloads against the reference, and against each other.
	const Fmt ref = ref_iso(sv);
	const Fmt nv = Datetime::Iso8601Parser(sv);
	// The filling overload can throw "Year is out of range" after a VALID scan (year 1 with an offset).
	auto fill = [](auto parse) { try { return parse(); } catch (const std::exception&) { return Fmt::VALID; } };
	const Fmt nf = fill([&] { auto t = sentinel<Datetime::tm_t>(0); return Datetime::Iso8601Parser(sv, t); });
	++checks;
	if (nv != ref || nf != ref) {
		fail("verdict(new vs reference)", s, "ref=" + std::to_string(int(ref)) + " validate=" + std::to_string(int(nv)) + " fill=" + std::to_string(int(nf)));
	}

	// Old vs new, Iso8601Parser(tm): equal unless the old verdict was wrong.
	const Fmt of = fill([&] { auto t = sentinel<DatetimeOld::tm_t>(0); return Fmt(int(DatetimeOld::Iso8601Parser(sv, t))); });
	const Fmt ov = Fmt(int(DatetimeOld::Iso8601Parser(sv)));
	for (unsigned v = 0; v < 2; ++v) {
		if (of == ref) {
			compare("Iso8601Parser(tm)", s,
				run([&] { auto t = sentinel<DatetimeOld::tm_t>(v); auto f = DatetimeOld::Iso8601Parser(sv, t); return std::to_string(int(f)) + show(pack(t)); }),
				run([&] { auto t = sentinel<Datetime::tm_t>(v); auto f = Datetime::Iso8601Parser(sv, t); return std::to_string(int(f)) + show(pack(t)); }));
		} else if (v == 0) {
			++checks;
			if ((of != Fmt::VALID && of != Fmt::INVALID) || ref == Fmt::VALID || ref == Fmt::INVALID) { fail("changed class (fill): old was not more permissive", s, ""); }
			(ref == Fmt::OUT_OF_RANGE ? class_neg : class_frac)++;
		}
	}
	if (ov == ref) {
		compare("Iso8601Parser", s,
			run([&] { return std::to_string(int(DatetimeOld::Iso8601Parser(sv))); }),
			run([&] { return std::to_string(int(Datetime::Iso8601Parser(sv))); }));
	} else {
		++checks;
		if ((ov != Fmt::VALID && ov != Fmt::INVALID) || ref == Fmt::VALID || ref == Fmt::INVALID) { fail("changed class (validate): old was not more permissive", s, ""); }
		(ref == Fmt::OUT_OF_RANGE ? class_neg : class_frac)++;
	}

	// DatetimeParser works on the part before "||".
	const auto bar = sv.find("||");
	const auto prefix = sv.substr(0, bar);
	const Fmt pref = ref_iso(prefix);
	const Fmt pold = fill([&] { auto t = sentinel<DatetimeOld::tm_t>(0); return Fmt(int(DatetimeOld::Iso8601Parser(prefix, t))); });
	auto old_dp = run([&] { auto t = DatetimeOld::DatetimeParser(sv); return show(pack(t)); });
	auto new_dp = run([&] { auto t = Datetime::DatetimeParser(sv); return show(pack(t)); });
	const std::string zmsg = "Invalid format in Datetime Math unit: 'h'. '' must be numeric";
	if (pold != pref) {
		// Class 1/2: the new parse must fail exactly as the reference verdict says.
		++checks;
		const std::string want = pref == Fmt::ERROR ? "Error format in _datetime: " + s : "Datetime: " + s + " is out of range";
		if (!new_dp.threw || new_dp.msg != want) { fail("DatetimeParser changed class", s, "new=[" + (new_dp.threw ? new_dp.msg : new_dp.payload) + "]"); }
	} else if (pref == Fmt::INVALID && wide_fraction(prefix)) {
		// Class 8: a loose-format fraction wider than 62 digits is not a datetime. The reference's
		// answer depends on strict-stox's width, so only the oracle is checked.
		++class_wide_frac;
		++checks;
		if (!new_dp.threw || (new_dp.msg != as_what("Error format in _datetime: " + s) && new_dp.msg != as_what("Datetime: " + s + " is out of range"))) {
			fail("DatetimeParser wide fraction", s, "new=[" + (new_dp.threw ? new_dp.msg : new_dp.payload) + "]");
		}
	} else if (old_dp.threw && old_dp.msg == zmsg) {
		// Class 3: oracle is the original on the "+00:00" spelling, with is_utc.
		++class_z;
		const auto z = s.find('Z');
		++checks;
		if (z == std::string::npos) { fail("loose Z class without a Z", s, ""); }
		else {
			const std::string s2 = s.substr(0, z) + "+00:00" + s.substr(z + 1);
			auto oracle = run([&] { auto t = DatetimeOld::DatetimeParser(std::string_view(s2)); t.is_utc = true; return show(pack(t)); });
			compare("DatetimeParser loose Z", s, oracle, new_dp);
		}
	} else if (old_dp.threw && old_dp.msg == month_msg) {
		// Class 5: the old month-0 throw; the new code must not throw it.
		++class_month;
		++checks;
		if (new_dp.threw && new_dp.msg == month_msg) { fail("DatetimeParser month class", s, "still throws"); }
	} else if (!old_dp.threw && pref == Fmt::INVALID && prefix.find(':') != std::string_view::npos &&
			prefix.find_first_of("+-", prefix.find(':')) != std::string_view::npos) {
		// Class 6: a loose-format offset; the oracle is the old result with is_utc set.
		++class_off;
		auto oracle = run([&] { auto t = DatetimeOld::DatetimeParser(sv); t.is_utc = true; return show(pack(t)); });
		compare("DatetimeParser loose offset", s, oracle, new_dp);
	} else {
		compare("DatetimeParser", s, old_dp, new_dp);
	}

	if (ov == ref) {
		compare("isDatetime", s,
			run([&] { return std::to_string(DatetimeOld::isDatetime(sv)); }),
			run([&] { return std::to_string(Datetime::isDatetime(sv)); }));
	} else {
		++checks;
		if (Datetime::isDatetime(sv)) { fail("isDatetime changed class", s, "still true"); }
	}
	compare("isDate", s,
		run([&] { return std::to_string(DatetimeOld::isDate(sv)); }),
		run([&] { return std::to_string(Datetime::isDate(sv)); }));
}


static const char alphabet[] = "0123456789-:/ .,TZ+|ydMwhms\tx\0\xe9";

static char rand_char() { return alphabet[rnd(sizeof(alphabet) - 1 + 1)]; }  // includes the NUL

static std::string digits(int n) {
	std::string r;
	for (int i = 0; i < n; ++i) { r += static_cast<char>('0' + rnd(10)); }
	return r;
}

static std::string two(int v) { char b[8]; std::snprintf(b, sizeof(b), "%02d", v); return b; }

static std::string gen_math() {
	std::string m;
	int n = static_cast<int>(rnd(4));
	for (int i = 0; i < n; ++i) {
		switch (rnd(4)) {
			case 0: m += "/"; break;
			case 1: m += "//"; break;
			case 2: {
				const char* sign = chance(50) ? "+" : "-";
				const std::string number = chance(5) ? "1" + digits(10 + static_cast<int>(rnd(3))) : digits(1 + static_cast<int>(rnd(3)));
				m += sign + number;
				break;
			}
			default: {
				const char* sign = chance(50) ? "+" : "-";
				const std::string number = std::to_string(rnd(40));
				m += sign + number;
				break;
			}
		}
		m += "dyMwhmsx"[rnd(chance(97) ? 7 : 8)];
	}
	if (chance(10)) { m += rand_char(); }
	return m;
}

static std::string gen_tz() {
	std::string z;
	switch (rnd(6)) {
		case 0: return "";
		case 1: return "Z";
		default: {
			if (chance(15)) { z += std::string(rnd(3), ' '); }
			z += chance(50) ? '+' : '-';
			if (chance(70)) { z += two(static_cast<int>(rnd(chance(10) ? 30 : 24))); } else { z += std::to_string(rnd(12)); }
			z += ':';
			z += two(static_cast<int>(rnd(chance(10) ? 70 : 60)));
			return z;
		}
	}
}

// A string near the accepted grammars (strict ISO shapes and the loose ones).
static std::string gen_date() {
	std::string s;
	bool strict = chance(60);
	int year = chance(10) ? static_cast<int>(rnd(10000)) : static_cast<int>(1 + rnd(chance(50) ? 2100 : 9999));
	char buf[16];
	std::snprintf(buf, sizeof(buf), "%04d", year);
	s += buf;
	std::string sep = strict ? "-" : std::string(1, "-/ "[rnd(3)]);
	if (!strict && chance(20)) { sep = ""; }
	int mon = chance(90) ? static_cast<int>(1 + rnd(12)) : static_cast<int>(rnd(15));
	int day = chance(85) ? static_cast<int>(1 + rnd(28)) : static_cast<int>(rnd(chance(50) ? 33 : 40));
	s += sep + two(mon) + (chance(3) ? "-" : sep) + two(day);
	if (chance(35)) { return s + (chance(30) ? "||" + gen_math() : ""); }
	if (strict) {
		s += chance(70) ? 'T' : ' ';
	} else {
		s += chance(50) ? "T" : (chance(50) ? " " : "");
	}
	int hour = static_cast<int>(rnd(chance(8) ? 30 : 24));
	if (strict || chance(60)) { s += two(hour); } else { s += std::to_string(hour); }
	s += ':' + two(static_cast<int>(rnd(chance(8) ? 70 : 60)));
	if (strict || chance(80)) {
		s += ':' + two(static_cast<int>(rnd(chance(8) ? 70 : 60)));
		if (chance(50)) {
			s += chance(80) ? '.' : ',';
			int nd = chance(70) ? static_cast<int>(1 + rnd(6)) : static_cast<int>(rnd(chance(10) ? 70 : 12));
			s += digits(nd);
		}
	}
	s += gen_tz();
	if (chance(25)) {
		// Each draw is its own statement: operands of `+` and call arguments are unsequenced.
		const std::string before_bars(rnd(3), ' ');
		const std::string after_bars(rnd(3), ' ');
		s += before_bars + "||" + after_bars + gen_math();
	}
	return s;
}

static std::string mutate(std::string s) {
	int n = 1 + (chance(30) ? static_cast<int>(rnd(3)) : 0);
	for (int i = 0; i < n; ++i) {
		switch (rnd(5)) {
			case 0: if (!s.empty()) { const std::size_t at = rnd(s.size()); s[at] = rand_char(); } break;
			case 1: if (!s.empty()) { s.erase(rnd(s.size()), 1); } break;
			case 2: { const std::size_t at = rnd(s.size() + 1); s.insert(at, 1, rand_char()); break; }
			case 3: if (!s.empty()) { s.resize(rnd(s.size())); } break;
			default: if (!s.empty()) { const std::size_t at = rnd(s.size()); s[at] = static_cast<char>(rnd(256)); } break;
		}
	}
	return s;
}


// Every single-byte value at every position of each template.
static void exhaustive_bytes() {
	const char* templates[] = {
		"2020-01-02", "2020-01-02T03:04:05", "2020-01-02 03:04:05", "2020-01-02T03:04:05Z",
		"2020-01-02T03:04:05+05:30", "2020-01-02T03:04:05-00:30", "2020-01-02T03:04:05.5",
		"2020-01-02T03:04:05.123456Z", "2020-01-02T03:04:05.1234567+05:30", "2020-01-02T03:04:05.123-23:59",
		"2020-01-02T03:04:05||+1d/d", "2020/01/02 3:04:05", "20200102T03:04", "2020 01 02 03:04:05.25 +05:30 || +1d",
		"2020-01-02T03:04:05,5Z||//M", "2020-01-02||+1M", "0001-01-01T00:00:00+05:00", "0001-01-01T00:00:00.5-05:30",
	};
	for (auto t : templates) {
		std::string base = t;
		check_string(base);
		for (std::size_t pos = 0; pos < base.size(); ++pos) {
			for (int b = 0; b < 256; ++b) {
				std::string s = base;
				s[pos] = static_cast<char>(b);
				check_string(s);
			}
		}
	}
}

// All 2-char values for every 2-char field position of a 25-char template.
static void exhaustive_fields() {
	const char cs[] = "0123456789+- \t:xZ.";
	std::string base = "2020-01-02T03:04:05+05:30";
	const std::size_t starts[] = { 0, 2, 5, 8, 11, 14, 17, 20, 23 };
	for (auto st : starts) {
		for (char a : cs) {
			for (char b : cs) {
				std::string s = base;
				s[st] = a;
				s[st + 1] = b;
				check_string(s);
				std::string f = base.substr(0, 19) + ".5" + base.substr(19);  // fraction + offset
				f[st] = a;
				f[st + 1] = b;
				check_string(f);
				std::string d = "2020-01-02T03:04:05Z";
				d[st < 20 ? st : 0] = a;
				check_string(d);
			}
		}
	}
	// Four-char year field.
	for (char a : cs) for (char b : cs) for (char c : cs) {
		std::string s = "2020-01-02";
		s[0] = a; s[1] = b; s[2] = c;
		check_string(s);
		check_string(s + "T00:00:00");
	}
}

// All fractions with 1..8 digits where it matters, and the micro grid.
static void fractions() {
	for (unsigned micro = 0; micro < 1000000; ++micro) {
		char b[8];
		std::snprintf(b, sizeof(b), "%06u", micro);
		std::string s = std::string("2020-01-02T03:04:05.") + b + "Z";
		if (micro % 7 == 0) { check_string(s); continue; }
		++checks;
		auto a = DatetimeOld::tm_t(), c = DatetimeOld::tm_t();
		(void)c;
		DatetimeOld::tm_t o;
		Datetime::tm_t n;
		auto fo = DatetimeOld::Iso8601Parser(s, o);
		auto fn = Datetime::Iso8601Parser(s, n);
		(void)a;
		if (int(fo) != int(fn) || !(pack(o) == pack(n))) { fail("fraction", s, show(pack(o)) + " vs " + show(pack(n))); }
	}
	for (int nd = 1; nd <= 12; ++nd) {
		for (int i = 0; i < 20000; ++i) {
			std::string d = digits(nd);
			if (i == 0) { d = std::string(nd, '0'); }
			if (i == 1) { d = std::string(nd, '9'); }
			if (i == 2) { d = "5" + std::string(nd - 1, '0'); }
			if (i == 3 && nd > 6) { d = "0000005" + std::string(nd - 7, '0'); }
			if (i == 4 && nd > 6) { d = "0000004" + std::string(nd - 7, '9'); }
			if (i == 5 && nd > 6) { d = "9999995" + std::string(nd - 7, '0'); }
			check_string("2020-01-02T03:04:05." + d);
			if (i % 4 == 0) { check_string("2020-01-02T03:04:05." + d + "Z"); check_string("2020-01-02T03:04:05." + d + "+01:00"); check_string("2020/01/02 03:04:05," + d); }
		}
	}
	for (int nd : {61, 62, 63, 64, 65, 100}) {
		check_string("2020-01-02T03:04:05." + std::string(nd, '1'));
		check_string("2020-01-02T03:04:05." + std::string(nd, '1') + "Z");
		check_string("2020-01-02T03:04:05." + std::string(nd, '1') + "-01:30");
		check_string("2020/01/02 03:04:05." + std::string(nd, '1'));
	}
}


// Calendar boundaries through the offset path: every month end, leap days,
// the first and last days, offsets that roll the date either way.
static void calendar() {
	const char* tzs[] = { "Z", "+00:00", "+00:30", "-00:30", "+23:59", "-23:59", "+01:00", "+02:00", "+00:00", "+05:30", "-05:30", "+12:00", "-12:00" };
	const int years[] = { 1, 2, 4, 100, 400, 1600, 1900, 1969, 1970, 1971, 2000, 2020, 2024, 2100, 2400, 9999 };
	for (int y : years) {
		for (int m = 1; m <= 12; ++m) {
			for (int d : {1, 2, 27, 28, 29, 30, 31}) {
				for (const char* hms : {"00:00:00", "23:59:59", "12:00:00", "00:00:01", "23:59:58", "00:59:59", "01:59:59", "11:59:59"}) {
					char b[64];
					std::snprintf(b, sizeof(b), "%04d-%02d-%02dT%s", y, m, d, hms);
					for (auto tz : tzs) {
						check_string(std::string(b) + tz);
						check_string(std::string(b) + ".25" + tz);
					}
				}
				char b[32];
				std::snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d);
				check_string(b);
				check_string(std::string(b) + "||+1M/M");
				check_string(std::string(b) + "||/w");
				check_string(std::string(b) + "||//w");
				check_string(std::string(b) + "||-1y/y");
			}
		}
	}
}


static void check_math(const Datetime::tm_t& base, const std::string& math, bool direct_op, char unit) {
	feed_tm(base); feed_s(math); feed_u(direct_op); feed_u(static_cast<unsigned char>(unit));
	if (overflow_prone(math)) { return; }
	auto mk = [&](auto& t) { t.year = base.year; t.mon = base.mon; t.day = base.day; t.hour = base.hour; t.min = base.min; t.sec = base.sec; t.fsec = base.fsec; t.is_utc = base.is_utc; };
	const std::string label = math + " unit=" + unit + " base=" + show(pack(base));
	auto new_o = run([&] { Datetime::tm_t t; mk(t); std::string r; try { if (direct_op) Datetime::computeDateMath(t, math, unit); else Datetime::processDateMath(math, t); } catch (const std::exception& e) { r = std::string("throw ") + e.what() + " "; } return r + show(pack(t)); });
	const std::string month_throw = std::string("throw ") + month_msg + " ";
	if (direct_op ? math.size() > grammar_term_chars + 1 && (math[0] == '+' || math[0] == '-') : wide_term_digits(math)) {
		// Class 8: the number of a term is at most 63 characters. The reference's answer depends on
		// strict-stox's width (and it would run a regex over the wide term), so only the oracle is
		// checked: a direct term fails as non-numeric and leaves the tm_t alone; a chain throws.
		++class_wide_term;
		++checks;
		const std::string want = "throw Invalid format in Datetime Math unit: '" + std::string(1, unit) + "'. '" + math.substr(1) + "' must be numeric " + show(pack(base));
		if (direct_op ? new_o.payload != want : new_o.payload.rfind("throw ", 0) != 0) { fail("math wide term", label, "new=[" + new_o.payload + "]"); }
		return;
	}
	auto old_o = run([&] { DatetimeOld::tm_t t; mk(t); std::string r; try { if (direct_op) DatetimeOld::computeDateMath(t, math, unit); else DatetimeOld::processDateMath(math, t); } catch (const std::exception& e) { r = std::string("throw ") + e.what() + " "; } return r + show(pack(t)); });
	const bool base_month_ok = base.mon >= 1 && base.mon <= 12;
	if (old_o.payload.rfind(month_throw, 0) == 0 && (base_month_ok || old_o.payload != new_o.payload)) {
		// (A base month outside 1..12 can legitimately throw it in both, e.g. "/M".)
		// Class 5: the old month-0 throw. The new code must not throw it, and a
		// single valid term must equal the month arithmetic model.
		++class_month;
		++checks;
		if (new_o.payload.rfind(month_throw, 0) == 0) { fail("math month class", label, "still throws"); }
		if (direct_op && math.size() >= 2 && (math[0] == '+' || math[0] == '-') && math.size() <= 10 && std::string("yMwdhms").find(unit) != std::string::npos &&
				math.find_first_not_of("0123456789", 1) == std::string::npos && base.year >= 1 && base.mon >= 1 && base.mon <= 12 && base.day >= 1 && base.day <= dim(base.year, base.mon)) {
			const i128 n = (math[0] == '-' ? -1 : 1) * static_cast<i128>(std::stoll(math.substr(1)));
			const Expect e = ref_add(base, n, unit);
			Datetime::tm_t t = base;
			bool threw = false;
			try { Datetime::computeDateMath(t, math, unit); } catch (const std::exception&) { threw = true; }
			++checks;
			if (threw != e.throws || (!threw && !(t.year == e.y && t.mon == e.mon && t.day == e.day && t.hour == e.hour && t.min == e.min && t.sec == e.sec))) {
				fail("math month class vs reference", label, show(pack(t)));
			}
		}
		return;
	}
	if (base.mon > 12 && base.mon % 12 == 0 && old_o.payload != new_o.payload && old_o.payload.rfind("throw ", 0) == 0 && new_o.payload.rfind("throw ", 0) == 0) {
		// Class 5, base month a multiple of 12 above 12: both throw (e.g. "Year is
		// out of range") but the old code left month 0 behind. Oracle: the old code
		// on the same base with the month already normalized (month 12, years carried).
		auto oracle = run([&] {
			DatetimeOld::tm_t t; mk(t);
			t.year = base.year + base.mon / 12 - 1; t.mon = 12;
			std::string r;
			try { if (direct_op) DatetimeOld::computeDateMath(t, math, unit); else DatetimeOld::processDateMath(math, t); } catch (const std::exception& e) { r = std::string("throw ") + e.what() + " "; }
			return r + show(pack(t));
		});
		++checks;
		if (oracle.payload != new_o.payload) { fail("math throw on a month multiple of 12", label, "oracle " + oracle.payload + " vs new " + new_o.payload); }
		return;
	}
	compare(direct_op ? "computeDateMath" : "processDateMath", label, old_o, new_o);
}

static Datetime::tm_t rand_tm(bool odd) {
	Datetime::tm_t t;
	t.year = odd && chance(15) ? static_cast<int>(rnd(8)) - 2 : static_cast<int>(1 + rnd(chance(50) ? 2200 : 9999));
	t.mon = odd && chance(15) ? static_cast<int>(rnd(30)) - 5 : static_cast<int>(1 + rnd(12));
	t.day = odd && chance(15) ? static_cast<int>(rnd(70)) - 20 : static_cast<int>(1 + rnd(28));
	t.hour = odd && chance(15) ? static_cast<int>(rnd(100)) - 30 : static_cast<int>(rnd(24));
	t.min = odd && chance(15) ? static_cast<int>(rnd(200)) - 70 : static_cast<int>(rnd(60));
	t.sec = odd && chance(15) ? static_cast<int>(rnd(200)) - 70 : static_cast<int>(rnd(60));
	t.fsec = chance(50) ? 0.0 : static_cast<double>(rnd(1000000)) / 1e6;
	t.is_utc = chance(50);
	return t;
}

static void math_tests(std::uint64_t n) {
	for (std::uint64_t i = 0; i < n; ++i) {
		auto base = rand_tm(chance(20));
		std::string m = gen_math();
		if (chance(10)) { m = mutate(m); }
		check_math(base, m, false, 0);
		// computeDateMath with arbitrary operator text
		std::string op;
		int len = static_cast<int>(rnd(5));
		for (int k = 0; k < len; ++k) { op += "+-/ 0123456789\tx"[rnd(chance(70) ? 13 : 16)]; }
		if (op.empty()) { op = "/"; }
		check_math(base, op, true, "ydMwhmsx"[rnd(8)]);
		// numeric edge digits
		static const char* nums[] = { "0", "00", "2147483648", "99999999999999999999", "-0", "+5", " 5", "1e3", "18446744073709551621", "18446744073709551616", "4294967301" };
		const char* o2_sign = chance(50) ? "+" : "-";
		const char* o2_digits = nums[rnd(11)];
		std::string o2 = std::string(o2_sign) + o2_digits;
		check_math(base, o2, true, "ydMhms"[rnd(6)]);
		std::string zeros = std::string(chance(50) ? "+" : "-") + std::string(62, '0') + "1";
		check_math(base, zeros, true, 'd');
		check_math(base, zeros + "0", true, 'd');
	}
}

static void tz_tests(std::uint64_t n) {
	for (std::uint64_t i = 0; i < n; ++i) {
		auto base = rand_tm(chance(30));
		auto word = [&] {
			std::string w;
			int len = static_cast<int>(rnd(4));
			for (int k = 0; k < len; ++k) { w += "0123456789+- x"[rnd(chance(85) ? 10 : 14)]; }
			return w;
		};
		std::string h = word(), m = word();
		char op = "+-x"[rnd(3)];
		feed_tm(base); feed_s(h); feed_s(m); feed_u(static_cast<unsigned char>(op));
		++checks;
		auto fo = run([&] { DatetimeOld::tm_t t; t.year = base.year; t.mon = base.mon; t.day = base.day; t.hour = base.hour; t.min = base.min; t.sec = base.sec; t.fsec = base.fsec; t.is_utc = base.is_utc; std::string r; try { DatetimeOld::computeTimeZone(t, op, h, m); } catch (const std::exception& e) { r = std::string("throw ") + e.what() + " "; } return r + show(pack(t)); });
		auto fn = run([&] { Datetime::tm_t t = base; std::string r; try { Datetime::computeTimeZone(t, op, h, m); } catch (const std::exception& e) { r = std::string("throw ") + e.what() + " "; } return r + show(pack(t)); });
		if (fo.payload.rfind(std::string("throw ") + month_msg + " ", 0) == 0) {
			++class_month;  // class 5 through an odd base month; the new code must not throw it
			++checks;
			if (fn.payload.rfind(std::string("throw ") + month_msg + " ", 0) == 0) { fail("computeTimeZone month class", h + "|" + m, "still throws"); }
			continue;
		}
		compare("computeTimeZone", std::string(1, op) + "|" + h + "|" + m + "| base=" + show(pack(base)), fo, fn);
	}
}


// Class 8, date math: terms at and over the width limit, alone and after valid terms.
static void wide_terms() {
	Datetime::tm_t bases[4];
	const int vals[4][6] = { { 2021, 3, 14, 15, 9, 26 }, { 1, 1, 1, 0, 0, 0 }, { 9999, 12, 31, 23, 59, 59 }, { 1970, 2, 28, 12, 0, 0 } };
	for (int k = 0; k < 4; ++k) {
		bases[k].year = vals[k][0]; bases[k].mon = vals[k][1]; bases[k].day = vals[k][2];
		bases[k].hour = vals[k][3]; bases[k].min = vals[k][4]; bases[k].sec = vals[k][5];
		bases[k].fsec = k % 2 ? 0.0 : 0.25;
		bases[k].is_utc = k < 2;
	}
	const std::size_t widths[] = { 1, 2, 62, 63, 64, 65, 100, 1000 };
	for (const auto& base : bases) {
		for (std::size_t w : widths) {
			// Same value at every width: 1, 10 (leading zeros), 5 behind leading spaces, a value over int, and 9s.
			std::string texts[5];
			texts[0] = std::string(w - 1, '0') + "1";
			texts[1] = w >= 2 ? std::string(w - 2, '0') + "10" : "7";
			texts[2] = w >= 2 ? std::string(w - 1, ' ') + "5" : "5";
			texts[3] = "1" + std::string(w - 1, '0');
			texts[4] = std::string(w, '9');
			for (const auto& text : texts) {
				for (const char* sign : { "+", "-" }) {
					for (char unit : { 'd', 'y', 'M', 'w', 'h', 'm', 's', 'x' }) {
						check_math(base, sign + text, true, unit);
					}
				}
			}
			// A chain: the terms before the wide one apply, then it throws.
			for (const std::string& prefix : { std::string(), std::string("+1d"), std::string("/M"), std::string("-3h//y"), std::string("+2y-1M") }) {
				for (const char* sign : { "+", "-" }) {
					for (char unit : { 'd', 'y', 'M', 'w', 'h', 'm', 's' }) {
						const std::string text = std::string(w - 1, '0') + "1";
						const std::string chain = prefix + sign + text + unit;
						check_math(base, chain, false, 0);
						if (w <= grammar_term_chars) { continue; }
						// The oracle: the reference on the terms before the wide one, then the throw.
						auto before = run([&] {
							DatetimeOld::tm_t t; t.year = base.year; t.mon = base.mon; t.day = base.day; t.hour = base.hour; t.min = base.min; t.sec = base.sec; t.fsec = base.fsec; t.is_utc = base.is_utc;
							std::string r;
							try { DatetimeOld::processDateMath(prefix, t); r = "throw Invalid format in Datetime Math unit: '" + std::string(1, unit) + "'. '" + text + "' must be numeric "; } catch (const std::exception& e) { r = std::string("throw ") + e.what() + " "; }
							return r + show(pack(t));
						});
						auto got = run([&] {
							Datetime::tm_t t = base;
							std::string r;
							try { Datetime::processDateMath(chain, t); r = "no throw "; } catch (const std::exception& e) { r = std::string("throw ") + e.what() + " "; }
							return r + show(pack(t));
						});
						compare("processDateMath wide term", chain + " base=" + show(pack(base)), before, got);
					}
				}
			}
		}
	}
}


template <typename C>
static std::string show_clk(const C& c) {
	char b[160];
	std::snprintf(b, sizeof(b), "{%d:%d:%d fsec=%016llx tz=%d/%d:%d}", c.hour, c.min, c.sec,
		static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(c.fsec)), static_cast<int>(c.tz_s), c.tz_h, c.tz_m);
	return b;
}

// TimeParser and TimedeltaParser, the predicates isTime and isTimedelta (they check the shape, not the
// width of a fraction). A fraction wider than 62 digits is class 8.
static void check_time(const std::string& s, bool delta) {
	feed_s(s);
	feed_u(delta);
	const std::string_view sv(s);
	if (delta && s.empty()) {
		// Class 9: the reference reads timedelta[0] of an empty view (out of bounds, an assertion
		// under libstdc++'s hardening); the library throws. Only the library is run.
		++checks;
		auto o = run([&] { return show_clk(Datetime::TimedeltaParser(sv)); });
		if (!o.threw || o.msg != "Error format in timedelta: , the format must be '[+-]00:00(:00(.0...))'") { fail("TimedeltaParser empty", s, "new=[" + (o.threw ? o.msg : o.payload) + "]"); }
		++class_empty;
		return;
	}
	const auto old_o = run([&] { return show_clk(delta ? DatetimeOld::TimedeltaParser(sv) : DatetimeOld::TimeParser(sv)); });
	const auto new_o = run([&] { return show_clk(delta ? Datetime::TimedeltaParser(sv) : Datetime::TimeParser(sv)); });
	if (wide_fraction(s)) {
		++class_wide_frac;
		++checks;
		const std::string want = as_what(delta ? "Error format in timedelta: " + s + ", the format must be '[+-]00:00(:00(.0...))'"
		                                       : "Error format in time: " + s + ", the format must be '00:00(:00(.0...)([+-]00:00))'");
		if (!new_o.threw || new_o.msg != want) { fail(delta ? "TimedeltaParser wide fraction" : "TimeParser wide fraction", s, "new=[" + (new_o.threw ? new_o.msg : new_o.payload) + "]"); }
	} else {
		compare(delta ? "TimedeltaParser" : "TimeParser", s, old_o, new_o);
	}
	compare(delta ? "isTimedelta" : "isTime", s,
		run([&] { return std::to_string(delta ? DatetimeOld::isTimedelta(sv) : DatetimeOld::isTime(sv)); }),
		run([&] { return std::to_string(delta ? Datetime::isTimedelta(sv) : Datetime::isTime(sv)); }));
}

static std::string gen_time(bool delta) {
	std::string s;
	if (delta) { s += chance(50) ? '+' : (chance(90) ? '-' : rand_char()); }
	const int hour = static_cast<int>(rnd(chance(10) ? 120 : 24));
	s += two(hour);
	s += ':';
	const int min = static_cast<int>(rnd(chance(10) ? 120 : 60));
	s += two(min);
	if (chance(80)) {
		s += ':';
		const int sec = static_cast<int>(rnd(chance(10) ? 120 : 60));
		s += two(sec);
		if (chance(60)) {
			s += '.';
			const int nd = chance(70) ? static_cast<int>(1 + rnd(8)) : static_cast<int>(rnd(chance(15) ? 80 : 20));
			s += digits(nd);
		}
		if (!delta && chance(30)) {
			s += chance(50) ? '+' : '-';
			const int h = static_cast<int>(rnd(chance(10) ? 120 : 24));
			s += two(h);
			s += ':';
			const int m = static_cast<int>(rnd(chance(10) ? 120 : 60));
			s += two(m);
		}
	}
	return s;
}

static void time_tests(std::uint64_t n) {
	for (std::uint64_t i = 0; i < n; ++i) {
		for (bool delta : { false, true }) {
			auto s = gen_time(delta);
			check_time(s, delta);
			check_time(mutate(s), delta);
			if (chance(20)) { check_time(mutate(mutate(s)), delta); }
		}
	}
}

// Every single-byte value at every position of each time and timedelta template, and the
// fractions around the width limit with and without an offset.
static void exhaustive_time() {
	const std::vector<std::string> times = { "12:34", "12:34:56", "12:34:56+05:30", "12:34:56-00:30", "12:34:56.7", "12:34:56.789012", "12:34:56.789-05:30" };
	const std::vector<std::string> deltas = { "+12:34", "-12:34", "+12:34:56", "-12:34:56", "+12:34:56.7", "-12:34:56.789012" };
	for (bool delta : { false, true }) {
		check_time("", delta);
		for (const std::string& base : delta ? deltas : times) {
			check_time(base, delta);
			for (std::size_t pos = 0; pos < base.size(); ++pos) {
				for (int b = 0; b < 256; ++b) {
					std::string s = base;
					s[pos] = static_cast<char>(b);
					check_time(s, delta);
				}
			}
		}
	}
	for (std::size_t nd : {std::size_t(1), std::size_t(6), std::size_t(7), grammar_fraction_digits - 1, grammar_fraction_digits, grammar_fraction_digits + 1, grammar_fraction_digits + 2, std::size_t(100), std::size_t(1000)}) {
		for (const char* digit : { "1", "0", "9" }) {
			const std::string f = std::string(nd, *digit);
			check_time("12:34:56." + f, false);
			check_time("12:34:56." + f + "+05:30", false);
			check_time("12:34:56." + f + "-05:30", false);
			check_time("+12:34:56." + f, true);
			check_time("-12:34:56." + f, true);
		}
	}
}


static void format_check(const Datetime::tm_t& tm, bool trim, char sep) {
	feed_tm(tm); feed_u(trim); feed_u(static_cast<unsigned char>(sep));
	DatetimeOld::tm_t o;
	o.year = tm.year; o.mon = tm.mon; o.day = tm.day; o.hour = tm.hour; o.min = tm.min; o.sec = tm.sec; o.fsec = tm.fsec; o.is_utc = tm.is_utc;
	compare("iso8601(tm_t)", show(pack(tm)) + (trim ? " trim" : " full") + " sep=" + sep,
		run([&] { return DatetimeOld::iso8601(o, trim, sep); }), run([&] { return Datetime::iso8601(tm, trim, sep); }));
}

static void format_tests(std::uint64_t n) {
	std::vector<double> fs = { 0.0, -0.0, -1.0, 0.5, 0.999999, 0.9999995, std::nextafter(0.9999995, 0.0), std::nextafter(0.9999995, 1.0),
		0.9999999, 1.0, 1.5, 12.5, 123.456, 1e300, 1e-320, 4.9e-324, 5e-7, std::nextafter(5e-7, 0.0), std::nextafter(5e-7, 1.0), 4.9999999e-7, 1.5e-6,
		std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 1.0 - 1e-16 };
	for (int k = 0; k < 128; ++k) { fs.push_back(k / 128.0); }          // exact ties at the 7th decimal
	for (int k = 1; k < 128; k += 2) { fs.push_back(k / 128.0 / 1000.0); fs.push_back(k / 4096.0); fs.push_back(k / 1048576.0); fs.push_back(k * 5e-7); }
	for (int bit = 1; bit < 1000; bit += 3) { fs.push_back(std::ldexp(1.0, -bit)); fs.push_back(std::ldexp(1.0, -bit) * 3); }
	for (double f : fs) {
		for (bool trim : {true, false}) {
			for (bool utc : {true, false}) {
				auto t = rand_tm(false);
				t.fsec = f; t.is_utc = utc;
				format_check(t, trim, 'T');
			}
		}
	}
	// Whole micro grid, and its half-way points, in both modes.
	for (unsigned micro = 0; micro < 1000000; ++micro) {
		auto t = rand_tm(false);
		t.fsec = micro / 1e6;
		t.is_utc = micro & 1;
		format_check(t, micro & 2, 'T');
		if (micro % 3 == 0) {
			t.fsec = (2.0 * micro + 1.0) / 2e6;
			format_check(t, micro & 4, ' ');
			t.fsec = std::nextafter(t.fsec, 0.0);
			format_check(t, true, 'T');
			t.fsec = std::nextafter(std::nextafter(t.fsec, 1.0), 1.0);
			format_check(t, false, 'T');
		}
	}
	for (std::uint64_t i = 0; i < n; ++i) {
		auto t = rand_tm(true);
		t.year = chance(30) ? static_cast<int>(rnd(200000)) - 100000 : t.year;
		t.hour = chance(10) ? static_cast<int>(rnd(2000)) - 1000 : t.hour;
		if (chance(50)) {
			t.fsec = std::bit_cast<double>(rng());
		} else if (chance(50)) {
			t.fsec = static_cast<double>(rng() >> 11) * 0x1p-53;
		} else {
			const double unit = static_cast<double>(rng() >> 11) * 0x1p-53;
			t.fsec = unit * (chance(20) ? 3 : 1);
		}
		const bool fmt_trim = chance(50);
		const char sep = "T x"[rnd(3)];
		format_check(t, fmt_trim, sep);
		// std::tm
		std::tm a{};
		a.tm_year = static_cast<int>(rnd(300000)) - 150000; a.tm_mon = static_cast<int>(rnd(14)) - 1; a.tm_mday = static_cast<int>(rnd(40)) - 3;
		a.tm_hour = static_cast<int>(rnd(30)) - 3; a.tm_min = static_cast<int>(rnd(70)) - 5; a.tm_sec = static_cast<int>(rnd(70)) - 5;
		bool trim = chance(50);
		for (int v : { a.tm_year, a.tm_mon, a.tm_mday, a.tm_hour, a.tm_min, a.tm_sec }) { feed_u(static_cast<std::uint32_t>(v)); }
		feed_u(trim);
		compare("iso8601(std::tm)", std::to_string(a.tm_year) + "," + std::to_string(a.tm_mon) + "," + std::to_string(a.tm_mday) + (trim ? " trim" : " full"),
			run([&] { return DatetimeOld::iso8601(a, trim, 'T'); }), run([&] { return Datetime::iso8601(a, trim, 'T'); }));
	}
}


static std::string hexfloat(double v) {
	char b[64];
	std::snprintf(b, sizeof(b), "%a", v);
	return b;
}

static DatetimeOld::tm_t to_old(const Datetime::tm_t& b) {
	DatetimeOld::tm_t t;
	t.year = b.year; t.mon = b.mon; t.day = b.day; t.hour = b.hour; t.min = b.min; t.sec = b.sec; t.fsec = b.fsec; t.is_utc = b.is_utc;
	return t;
}

// toordinal, timegm (both overloads, including the month normalized in place) and
// timestamp against the original, for valid and invalid fields: the same value or the
// same exception for every input, except
//  - class 5, a month that is a multiple of 12 above 12: the original's normalizeMonths
//    leaves month 0 and the call throws "Month must be in 1..12"; the oracle is the
//    original on the month already normalized (month 12 of the year it belongs to).
//  - class 7, a timestamp before 1970 with a fraction: the original subtracted the fraction
//    (23:59:58.9 was -2.9); the library adds it (-1.1). Oracle: whole seconds from the
//    original with no fraction, plus the fraction.
static void calendar_function_tests(std::uint64_t n) {
	auto old_month = [&](int year, int mon, int& oy, int& om) {
		oy = year;
		om = mon;
		if (mon > 12 && mon % 12 == 0) {
			oy = year + mon / 12 - 1;
			om = 12;
			++class_month;
		}
	};
	for (std::uint64_t i = 0; i < n; ++i) {
		const bool odd = chance(25);
		const int year = odd ? static_cast<int>(rnd(14)) - 3 : static_cast<int>(1 + rnd(chance(50) ? 2400 : 12000));
		const int month = odd && chance(50) ? static_cast<int>(rnd(60)) - 12 : static_cast<int>(1 + rnd(12));
		const int day = odd && chance(50) ? static_cast<int>(rnd(70)) - 20 : static_cast<int>(1 + rnd(31));
		const int hour = static_cast<int>(rnd(30)) - 3;
		const int min = static_cast<int>(rnd(70)) - 5;
		const int sec = static_cast<int>(rnd(70)) - 5;
		const double fsec = chance(50) ? 0.0 : static_cast<double>(rnd(1000000)) / 1e6;
		for (int v : { year, month, day, hour, min, sec }) { feed_u(static_cast<std::uint32_t>(v)); }
		feed_u(std::bit_cast<std::uint64_t>(fsec));
		const std::string label = std::to_string(year) + "-" + std::to_string(month) + "-" + std::to_string(day) + " " + std::to_string(hour) + ":" + std::to_string(min) + ":" + std::to_string(sec) + " + " + hexfloat(fsec);

		compare("toordinal", label,
			run([&] { return std::to_string(DatetimeOld::toordinal(year, month, day)); }), run([&] { return std::to_string(Datetime::toordinal(year, month, day)); }));

		int oy, om;
		old_month(year, month, oy, om);
		{
			std::tm a{};
			a.tm_year = year - DATETIME_START_YEAR; a.tm_mon = month - 1; a.tm_mday = day; a.tm_hour = hour; a.tm_min = min; a.tm_sec = sec;
			std::tm o = a;
			o.tm_year = oy - DATETIME_START_YEAR; o.tm_mon = om - 1;
			compare("timegm(std::tm)", label,
				run([&] { return std::to_string(DatetimeOld::timegm(o)); }), run([&] { return std::to_string(Datetime::timegm(a)); }));
		}
		{
			const Datetime::tm_t base(year, month, day, hour, min, sec, fsec);
			auto after = [](auto& t, std::string r) { return r + " " + show(pack(t)); };
			compare("timegm(tm_t)", label,
				run([&] { auto t = to_old(base); t.year = oy; t.mon = om; std::string r; try { r = std::to_string(DatetimeOld::timegm(t)); } catch (const std::exception& e) { r = std::string("throw ") + e.what(); } return after(t, r); }),
				run([&] { auto t = base; std::string r; try { r = std::to_string(Datetime::timegm(t)); } catch (const std::exception& e) { r = std::string("throw ") + e.what(); } return after(t, r); }));

			auto old_ts = [&](double f) { auto t = to_old(base); t.year = oy; t.mon = om; t.fsec = f; return run([&] { return hexfloat(DatetimeOld::timestamp(t)); }); };
			auto new_ts = [&](double f) { auto t = base; t.fsec = f; return run([&] { return hexfloat(Datetime::timestamp(t)); }); };
			const auto o = old_ts(fsec), w = new_ts(fsec);
			if (!o.threw && !w.threw && o.payload != w.payload) {
				// Class 7: the whole seconds are negative and there is a fraction.
				const auto whole = old_ts(0.0);
				++class_pre;
				++checks;
				const double base_secs = std::strtod(whole.payload.c_str(), nullptr);
				if (!(base_secs < 0 && fsec != 0.0 && o.payload == hexfloat(base_secs - fsec) && w.payload == hexfloat(base_secs + fsec))) {
					fail("timestamp class 7", label, "old=" + o.payload + " new=" + w.payload + " whole=" + whole.payload);
				}
			} else {
				compare("timestamp", label, o, w);
			}
		}
	}
}


static void tm_tests(std::uint64_t n) {
	auto cmp_t = [&](std::int64_t t) {
		feed_u(static_cast<std::uint64_t>(t));
		compare("to_tm_t(time_t)", std::to_string(t),
			run([&] { return show(pack(DatetimeOld::to_tm_t(static_cast<std::time_t>(t)))); }),
			run([&] { return show(pack(Datetime::to_tm_t(static_cast<std::time_t>(t)))); }));
	};
	auto cmp_d = [&](double t) {
		feed_u(std::bit_cast<std::uint64_t>(t));
		char b[64];
		std::snprintf(b, sizeof(b), "%.17g", t);
		if (t < 0 && t != std::floor(t)) {
			// Class 7, a fraction before 1970: the original took the whole seconds toward zero
			// and dropped the fraction (it was negative); the library takes them toward minus
			// infinity and keeps the distance above them. Oracle: the reference calendar for
			// the whole seconds (days-from-civil in ref.h), the library's own fraction rounding.
			++class_pre;
			const auto tm_at = [](std::int64_t secs, double fsec) {
				i128 y;
				int m, d;
				const i128 z = floordiv(secs, 86400);
				ref_civil(z, y, m, d);
				const int sod = static_cast<int>(secs - z * 86400);
				return Datetime::tm_t(static_cast<int>(y), m, d, sod / 3600, sod % 3600 / 60, sod % 60, fsec);
			};
			const double whole = std::floor(t);
			const auto want = tm_at(static_cast<std::int64_t>(whole), Datetime::normalize_fsec(t - whole));
			const auto old_want = tm_at(static_cast<std::int64_t>(t), 0.0);
			compare("to_tm_t(double) before 1970", b, run([&] { return show(pack(old_want)); }), run([&] { return show(pack(DatetimeOld::to_tm_t(t))); }));
			compare("to_tm_t(double) before 1970, new", b, run([&] { return show(pack(want)); }), run([&] { return show(pack(Datetime::to_tm_t(t))); }));
			compare("iso8601(double) before 1970", b, run([&] { return Datetime::iso8601(want); }), run([&] { return Datetime::iso8601(t); }));
			++checks;
			if (show(pack(want)) == show(pack(old_want))) { fail("class 7 must change the result", b, ""); }
			return;
		}
		compare("to_tm_t(double)", b,
			run([&] { return show(pack(DatetimeOld::to_tm_t(t))); }), run([&] { return show(pack(Datetime::to_tm_t(t))); }));
		compare("iso8601(double)", b,
			run([&] { return DatetimeOld::iso8601(t); }), run([&] { return Datetime::iso8601(t); }));
	};
	// Every day boundary for years 1..3000 (+-1s), including negative timestamps.
	for (std::int64_t day = -719162; day < 400000; ++day) {
		for (std::int64_t d : {-1, 0, 1}) { cmp_t(day * 86400 + d); }
	}
	for (std::uint64_t i = 0; i < n; ++i) {
		std::int64_t t = chance(50) ? static_cast<std::int64_t>(rng() >> 24) - (std::int64_t{1} << 39) : static_cast<std::int64_t>(rnd(8000000000ULL)) - 3000000000LL;
		cmp_t(t);
		cmp_d(static_cast<double>(t) + (chance(50) ? static_cast<double>(rnd(1000000)) / 1e6 : -static_cast<double>(rnd(1000000)) / 1e6));
		cmp_d(static_cast<double>(t) + std::bit_cast<double>(rng() & 0x3fefffffffffffffULL));
	}
}


int main(int argc, char** argv) {
	const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 300000;
	if (argc > 2) { rng.seed(std::strtoull(argv[2], nullptr, 10)); }

	exhaustive_bytes();
	exhaustive_fields();
	calendar();
	fractions();
	wide_terms();
	exhaustive_time();
	for (std::uint64_t i = 0; i < n; ++i) {
		auto s = gen_date();
		check_string(s);
		check_string(mutate(s));
		if (chance(20)) { check_string(mutate(mutate(s))); }
		std::string r;
		for (int k = static_cast<int>(rnd(30)); k > 0; --k) { r += rand_char(); }
		check_string(r);
	}
	math_tests(n);
	tz_tests(n);
	time_tests(n);
	format_tests(n);
	tm_tests(n);
	calendar_function_tests(n);

	std::printf("changed classes hit: negative field %llu, fraction %llu, loose Z %llu\n",
		static_cast<unsigned long long>(class_neg), static_cast<unsigned long long>(class_frac), static_cast<unsigned long long>(class_z));
	std::printf("                    month %llu, loose offset %llu, fraction before 1970 %llu\n",
		static_cast<unsigned long long>(class_month), static_cast<unsigned long long>(class_off), static_cast<unsigned long long>(class_pre));
	std::printf("                    wide term %llu, wide fraction %llu, empty timedelta %llu\n",
		static_cast<unsigned long long>(class_wide_term), static_cast<unsigned long long>(class_wide_frac), static_cast<unsigned long long>(class_empty));
	if (class_neg == 0 || class_frac == 0 || class_z == 0 || class_month == 0 || class_off == 0 || class_pre == 0 || class_wide_term == 0 || class_wide_frac == 0 || class_empty == 0) { std::printf("a changed class was never exercised\n"); ++failures; }
	std::printf("%llu checks, %llu mismatches, input digest %016llx\n", static_cast<unsigned long long>(checks), static_cast<unsigned long long>(failures),
		static_cast<unsigned long long>(digest));
	// The default CTest run (20000 iterations, the default seed) generates a recorded set of inputs.
	// A different digest means the generator changed (update the constant) or a draw depends on the
	// compiler (an expression with two draws in unsequenced operands).
	if (argc == 2 && n == 20000 && digest != expected_digest_20000) {
		std::printf("the generated inputs differ from the recorded digest %016llx\n", static_cast<unsigned long long>(expected_digest_20000));
		++failures;
	}
	return failures == 0 ? 0 : 1;
}
