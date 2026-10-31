// Differential test: the hand-written parsers/formatters in datetime.cc against
// the frozen original implementation in test/old/ (namespace DatetimeOld).
//
// Every public entry point touched by the rewrite is fed random, grammar-based,
// mutated and exhaustively single-byte-mutated inputs, valid and invalid, and
// the results are compared completely: Format code, every tm_t field (bitwise,
// including the partial writes left by a failing parse), exception message,
// and formatted strings.
//
// Quirks of the original that are preserved on purpose (candidates for owner
// decisions, not fixed here):
//  - Iso8601Parser(str, tm) accepts a negative hour/min/sec field such as "-1"
//    (compared as int) while Iso8601Parser(str) and isDatetime reject it
//    (unsigned long compare), so the two disagree on those inputs.
//  - Iso8601Parser(str) never parses the fraction: "...:05.Z" and a 63+ digit
//    fraction are VALID there but ERROR in the filling overload.
//  - Fields accept strtoul's leading whitespace and sign ("+5", " 5").
//  - Loose-format strings ending in Z ("2020/01/02 03:04:05Z") always throw
//    "Invalid format in Datetime Math unit: 'h'. '' must be numeric".
//  - Iso8601Parser can throw ("Year is out of range") for year 1 with an offset
//    that moves the instant before 0001-01-01, despite returning a Format.
//  - computeDateMath overflows int on huge +N (UB in both implementations).
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


static std::mt19937_64 rng(0x5eed1234abcdULL);
static std::uint64_t checks = 0;
static std::uint64_t failures = 0;

static std::uint64_t rnd(std::uint64_t n) { return rng() % n; }
static bool chance(unsigned pct) { return rnd(100) < pct; }

static std::string show(const std::string& s) {
	std::string r = "\"";
	for (unsigned char c : s) {
		if (c >= 32 && c < 127 && c != '"' && c != '\\') { r += static_cast<char>(c); }
		else { char b[8]; std::snprintf(b, sizeof(b), "\\x%02x", c); r += b; }
	}
	return r + "\"";
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

static void check_string(const std::string& s) {
	if (overflow_prone(s)) { return; }
	std::string_view sv(s);
	compare("DatetimeParser", s,
		run([&] { auto t = DatetimeOld::DatetimeParser(sv); return show(pack(t)); }),
		run([&] { auto t = Datetime::DatetimeParser(sv); return show(pack(t)); }));
	for (unsigned v = 0; v < 2; ++v) {
		compare("Iso8601Parser(tm)", s,
			run([&] { auto t = sentinel<DatetimeOld::tm_t>(v); auto f = DatetimeOld::Iso8601Parser(sv, t); return std::to_string(int(f)) + show(pack(t)); }),
			run([&] { auto t = sentinel<Datetime::tm_t>(v); auto f = Datetime::Iso8601Parser(sv, t); return std::to_string(int(f)) + show(pack(t)); }));
	}
	compare("Iso8601Parser", s,
		run([&] { return std::to_string(int(DatetimeOld::Iso8601Parser(sv))); }),
		run([&] { return std::to_string(int(Datetime::Iso8601Parser(sv))); }));
	compare("isDatetime", s,
		run([&] { return std::to_string(DatetimeOld::isDatetime(sv)); }),
		run([&] { return std::to_string(Datetime::isDatetime(sv)); }));
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
			case 2: m += (chance(50) ? "+" : "-") + (chance(5) ? "1" + digits(10 + static_cast<int>(rnd(3))) : digits(1 + static_cast<int>(rnd(3)))); break;
			default: m += (chance(50) ? "+" : "-") + std::to_string(rnd(40)); break;
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
		s += std::string(rnd(3), ' ') + "||" + std::string(rnd(3), ' ') + gen_math();
	}
	return s;
}

static std::string mutate(std::string s) {
	int n = 1 + (chance(30) ? static_cast<int>(rnd(3)) : 0);
	for (int i = 0; i < n; ++i) {
		switch (rnd(5)) {
			case 0: if (!s.empty()) { s[rnd(s.size())] = rand_char(); } break;
			case 1: if (!s.empty()) { s.erase(rnd(s.size()), 1); } break;
			case 2: s.insert(rnd(s.size() + 1), 1, rand_char()); break;
			case 3: if (!s.empty()) { s.resize(rnd(s.size())); } break;
			default: if (!s.empty()) { s[rnd(s.size())] = static_cast<char>(rnd(256)); } break;
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
	auto mk = [&](auto& t) { t.year = base.year; t.mon = base.mon; t.day = base.day; t.hour = base.hour; t.min = base.min; t.sec = base.sec; t.fsec = base.fsec; t.is_utc = base.is_utc; };
	compare(direct_op ? "computeDateMath" : "processDateMath", math + " unit=" + unit + " base=" + show(pack(base)),
		run([&] { DatetimeOld::tm_t t; mk(t); std::string r; try { if (direct_op) DatetimeOld::computeDateMath(t, math, unit); else DatetimeOld::processDateMath(math, t); } catch (const std::exception& e) { r = std::string("throw ") + e.what() + " "; } return r + show(pack(t)); }),
		run([&] { Datetime::tm_t t; mk(t); std::string r; try { if (direct_op) Datetime::computeDateMath(t, math, unit); else Datetime::processDateMath(math, t); } catch (const std::exception& e) { r = std::string("throw ") + e.what() + " "; } return r + show(pack(t)); }));
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
		std::string o2 = std::string(chance(50) ? "+" : "-") + nums[rnd(11)];
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
		++checks;
		auto fo = run([&] { DatetimeOld::tm_t t; t.year = base.year; t.mon = base.mon; t.day = base.day; t.hour = base.hour; t.min = base.min; t.sec = base.sec; t.fsec = base.fsec; t.is_utc = base.is_utc; std::string r; try { DatetimeOld::computeTimeZone(t, op, h, m); } catch (const std::exception& e) { r = std::string("throw ") + e.what() + " "; } return r + show(pack(t)); });
		auto fn = run([&] { Datetime::tm_t t = base; std::string r; try { Datetime::computeTimeZone(t, op, h, m); } catch (const std::exception& e) { r = std::string("throw ") + e.what() + " "; } return r + show(pack(t)); });
		compare("computeTimeZone", std::string(1, op) + "|" + h + "|" + m + "| base=" + show(pack(base)), fo, fn);
	}
}


static void format_check(const Datetime::tm_t& tm, bool trim, char sep) {
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
		t.fsec = chance(50) ? std::bit_cast<double>(rng()) : (chance(50) ? static_cast<double>(rng() >> 11) * 0x1p-53 : (rng() >> 11) * 0x1p-53 * (chance(20) ? 3 : 1));
		format_check(t, chance(50), "T x"[rnd(3)]);
		// std::tm
		std::tm a{};
		a.tm_year = static_cast<int>(rnd(300000)) - 150000; a.tm_mon = static_cast<int>(rnd(14)) - 1; a.tm_mday = static_cast<int>(rnd(40)) - 3;
		a.tm_hour = static_cast<int>(rnd(30)) - 3; a.tm_min = static_cast<int>(rnd(70)) - 5; a.tm_sec = static_cast<int>(rnd(70)) - 5;
		bool trim = chance(50);
		compare("iso8601(std::tm)", std::to_string(a.tm_year) + "," + std::to_string(a.tm_mon) + "," + std::to_string(a.tm_mday) + (trim ? " trim" : " full"),
			run([&] { return DatetimeOld::iso8601(a, trim, 'T'); }), run([&] { return Datetime::iso8601(a, trim, 'T'); }));
	}
}


static void tm_tests(std::uint64_t n) {
	auto cmp_t = [&](std::int64_t t) {
		compare("to_tm_t(time_t)", std::to_string(t),
			run([&] { return show(pack(DatetimeOld::to_tm_t(static_cast<std::time_t>(t)))); }),
			run([&] { return show(pack(Datetime::to_tm_t(static_cast<std::time_t>(t)))); }));
	};
	auto cmp_d = [&](double t) {
		char b[64];
		std::snprintf(b, sizeof(b), "%.17g", t);
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
	format_tests(n);
	tm_tests(n);

	std::printf("%llu checks, %llu mismatches\n", static_cast<unsigned long long>(checks), static_cast<unsigned long long>(failures));
	return failures == 0 ? 0 : 1;
}
