// datetime against independent implementations of the same work: a hand-written ISO 8601 parser and
// formatter, the days-from-civil and civil-from-days algorithms, libc strptime + timegm, and (when
// built with it) abseil's ParseTime, FromCivil and FormatTime.
//
//   datetime_compare           ns per call, one thread, best of 7 passes over 4096 inputs; each row
//                              also reruns the library's own side, the spread of that A/A pair
//   datetime_compare --check   no timing: every row's results must be identical
//
// The hand parser, the formatter and the civil algorithms are written from the public descriptions
// of the algorithms and the ISO 8601 grammar; they are comparators, not library code. The hand
// parser reads one grammar (at most 6 fraction digits) and reports failure with a bool, so its time
// is a floor for the library's per-call cost. The ratio column is the library's time over the
// comparator's: below 1 the library is faster.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "datetime.h"

#ifdef DATETIME_BENCH_ABSL
#include "absl/time/civil_time.h"
#include "absl/time/time.h"
#endif

using namespace Datetime;
using Clk = std::chrono::steady_clock;

template <class T>
static inline void keep(T const& v) {
	asm volatile("" : : "r,m"(v) : "memory");
}

static bool g_check = false;
static int g_bad = 0;

// Fastest of `reps` passes of f, in ns per element; with --check, one untimed pass.
template <class F>
static double bench(F&& f, std::size_t n, int reps = 7) {
	if (g_check) {
		f();
		return 0;
	}
	double best = 1e30;
	for (int r = 0; r < reps; ++r) {
		auto t0 = Clk::now();
		f();
		auto t1 = Clk::now();
		best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(n));
	}
	return best;
}

// One table row; the ratio is the library's time over the comparator's. `expect` marks a comparator
// that must produce the same results; one that cannot (abseil's format has no fraction) prints n/a.
static void row(const char* what, double ours, const char* cmp, double theirs, bool ok, bool expect = true) {
	if (!ok && expect) ++g_bad;
	if (g_check) {
		if (!ok && expect) std::printf("DIFFERENT: %s vs %s\n", what, cmp);
		return;
	}
	std::printf("%-30s %9.1f ns | %-18s %9.1f ns | %6.2fx | identical: %s\n", what, ours, cmp, theirs, ours / theirs, !expect ? "n/a" : ok ? "yes" : "no");
}

static void spread(double a, double b) {
	if (!g_check) std::printf("    A/A spread %.1f%%\n", 100 * std::fabs(a - b) / a);
}

// Civil calendar arithmetic from the public description of the algorithms.
static constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
	y -= m <= 2;
	const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = static_cast<unsigned>(y - era * 400);
	const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct civil {
	int y;
	unsigned m, d;
};

static constexpr civil civil_from_days(std::int64_t z) noexcept {
	z += 719468;
	const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const unsigned doe = static_cast<unsigned>(z - era * 146097);
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
	const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const unsigned mp = (5 * doy + 2) / 153;
	const unsigned d = doy - (153 * mp + 2) / 5 + 1;
	const unsigned m = mp < 10 ? mp + 3 : mp - 9;
	return {static_cast<int>(y + (m <= 2)), m, d};
}

// Hand ISO 8601 parser: YYYY-MM-DD[T ]hh:mm:ss[.f{1,6}][Z|+hh:mm]. Returns the seconds since the epoch.
static inline bool dig2(const char* p, int& v) {
	const unsigned a = static_cast<unsigned>(p[0] - '0'), b = static_cast<unsigned>(p[1] - '0');
	if (a > 9 || b > 9) return false;
	v = static_cast<int>(a * 10 + b);
	return true;
}

static bool hand_iso(std::string_view s, double& out) {
	const char* p = s.data();
	const std::size_t n = s.size();
	if (n < 10 || p[4] != '-' || p[7] != '-') return false;
	int c, yy, mo, d;
	if (!dig2(p, c) || !dig2(p + 2, yy) || !dig2(p + 5, mo) || !dig2(p + 8, d)) return false;
	const int y = c * 100 + yy;
	static const unsigned char dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (y < 1 || mo < 1 || mo > 12) return false;
	const int dim = dm[mo - 1] + (mo == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
	if (d < 1 || d > dim) return false;
	std::int64_t secs = days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400;
	double fsec = 0;
	if (n > 10) {
		int h, mi, se;
		if (n < 19 || (p[10] != 'T' && p[10] != ' ') || p[13] != ':' || p[16] != ':') return false;
		if (!dig2(p + 11, h) || !dig2(p + 14, mi) || !dig2(p + 17, se) || h > 23 || mi > 59 || se > 59) return false;
		secs += h * 3600 + mi * 60 + se;
		std::size_t i = 19;
		if (i < n && p[i] == '.') {
			++i;
			const std::size_t start = i;
			std::uint64_t f = 0;
			int digits = 0;
			while (i < n && static_cast<unsigned>(p[i] - '0') < 10) {
				if (digits < 6) {
					f = f * 10 + static_cast<unsigned>(p[i] - '0');
					++digits;
				}
				++i;
			}
			if (i == start) return false;
			static const double pw[7] = {1, 10, 100, 1e3, 1e4, 1e5, 1e6};
			fsec = static_cast<double>(f) / pw[digits];  // the benchmark inputs have at most 6 digits
		}
		if (i < n) {
			if (p[i] == 'Z' && i + 1 == n) {
			} else if ((p[i] == '+' || p[i] == '-') && n - i == 6 && p[i + 3] == ':') {
				int th, tm_;
				if (!dig2(p + i + 1, th) || !dig2(p + i + 4, tm_) || th > 23 || tm_ > 59) return false;
				const int off = th * 3600 + tm_ * 60;
				secs += (p[i] == '+') ? -off : off;
			} else {
				return false;
			}
		}
	}
	out = static_cast<double>(secs) + fsec;
	return true;
}

// Hand ISO formatter: the trimmed form of iso8601(tm_t), the fraction without trailing zeros.
static inline char* put2(char* o, int v) {
	o[0] = static_cast<char>('0' + v / 10);
	o[1] = static_cast<char>('0' + v % 10);
	return o + 2;
}

static std::size_t hand_format(char* o0, const tm_t& t) {
	char* o = o0;
	const int y = t.year;
	o[0] = static_cast<char>('0' + y / 1000);
	o[1] = static_cast<char>('0' + y / 100 % 10);
	o[2] = static_cast<char>('0' + y / 10 % 10);
	o[3] = static_cast<char>('0' + y % 10);
	o += 4;
	*o++ = '-';
	o = put2(o, t.mon);
	*o++ = '-';
	o = put2(o, t.day);
	*o++ = 'T';
	o = put2(o, t.hour);
	*o++ = ':';
	o = put2(o, t.min);
	*o++ = ':';
	o = put2(o, t.sec);
	if (t.fsec > 0.0) {
		unsigned us = static_cast<unsigned>(std::llround(t.fsec * 1e6));
		if (us >= 1000000) us = 999999;
		if (us) {
			*o++ = '.';
			char b[6];
			for (int i = 5; i >= 0; --i) {
				b[i] = static_cast<char>('0' + us % 10);
				us /= 10;
			}
			int len = 6;
			while (len > 1 && b[len - 1] == '0') --len;
			std::memcpy(o, b, static_cast<std::size_t>(len));
			o += len;
		}
	}
	if (t.is_utc) *o++ = 'Z';
	return static_cast<std::size_t>(o - o0);
}

static std::vector<std::string> make_inputs(std::mt19937_64& rng, int kind, std::size_t count) {
	std::vector<std::string> v;
	std::uniform_int_distribution<int> Y(1971, 2099), M(1, 12), D(1, 28), H(0, 23), Mi(0, 59), Ms(0, 999999), Z(0, 14);
	for (std::size_t i = 0; i < count; ++i) {
		const int y = Y(rng), m = M(rng), d = D(rng), h = H(rng), mi = Mi(rng), s = Mi(rng);
		char b[64];
		switch (kind) {
			case 0: std::snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d); break;
			case 1: std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d", y, m, d, h, mi, s); break;
			case 2: std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, m, d, h, mi, s); break;
			case 3: std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%06dZ", y, m, d, h, mi, s, Ms(rng)); break;
			case 4: std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", y, m, d, h, mi, s, i & 1 ? '+' : '-', Z(rng), Mi(rng)); break;
			case 5: std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%06d+%02d:%02d", y, m, d, h, mi, s, Ms(rng), Z(rng), Mi(rng)); break;
			default: std::snprintf(b, sizeof b, "%04d/%02d/%02d %02d:%02d:%02d", y, m, d, h, mi, s); break;  // the loose form
		}
		v.push_back(b);
	}
	return v;
}

int main(int argc, char** argv) {
	g_check = argc > 1 && std::strcmp(argv[1], "--check") == 0;
	std::mt19937_64 rng(42);
	const std::size_t N = 4096;
	if (!g_check) std::printf("datetime against hand-written, libc and abseil comparators; %s\n", __VERSION__);

	// ---- parse
	const char* names[] = {"parse YYYY-MM-DD", "parse ...T00:00:00", "parse ...Z", "parse .ffffffZ", "parse ...+hh:mm", "parse .ffffff+hh:mm", "parse 2020/01/02 03:04:05"};
	for (int kind = 0; kind < 7; ++kind) {
		const auto v = make_inputs(rng, kind, N);
		std::vector<double> ref(N), got(N);
		auto library = [&] {
			for (std::size_t i = 0; i < N; ++i) got[i] = timestamp(DatetimeParser(v[i]));
		};
		const int reps = kind == 6 ? 3 : 7;
		const double a = bench(library, N, reps);
		ref = got;
		const double a2 = bench(library, N, reps);
		std::vector<double> other(N);
		if (kind == 6) {
			auto strptime_timegm = [&] {
				for (std::size_t i = 0; i < N; ++i) {
					struct tm t{};
					const char* e = strptime(v[i].c_str(), "%Y/%m/%d %H:%M:%S", &t);
					other[i] = static_cast<double>(timegm(&t));
					keep(e);
				}
			};
			const double timed = bench(strptime_timegm, N);  // not in the call: argument order is unspecified
			row(names[kind], a, "strptime+timegm", timed, other == ref);
			auto hand = [&] {  // the same hand parser, after rewriting the two separators
				for (std::size_t i = 0; i < N; ++i) {
					char t[32];
					std::memcpy(t, v[i].data(), v[i].size());
					t[4] = t[7] = '-';
					hand_iso({t, v[i].size()}, other[i]);
				}
			};
			const double timed_hand = bench(hand, N);
			row(names[kind], a, "hand ISO", timed_hand, other == ref);
			spread(a, a2);
			continue;
		}
		auto hand = [&] {
			for (std::size_t i = 0; i < N; ++i) hand_iso(v[i], other[i]);
		};
		const double timed_hand = bench(hand, N);
		row(names[kind], a, "hand ISO", timed_hand, other == ref);
#ifdef DATETIME_BENCH_ABSL
		if (kind >= 1 && kind != 4 && kind != 5) {
			const char* format = kind == 1 ? "%Y-%m-%dT%H:%M:%S" : kind == 2 ? "%Y-%m-%dT%H:%M:%SZ" : "%Y-%m-%dT%H:%M:%E*SZ";
			auto abseil = [&] {
				for (std::size_t i = 0; i < N; ++i) {
					absl::Time t;
					std::string err;
					absl::ParseTime(format, v[i], absl::UTCTimeZone(), &t, &err);
					other[i] = static_cast<double>(absl::ToUnixMicros(t)) / 1e6;
				}
			};
			const double c = bench(abseil, N, 3);
			bool ok = true;
			for (std::size_t i = 0; i < N; ++i) ok &= std::fabs(other[i] - ref[i]) < 1e-6;
			row(names[kind], a, "absl::ParseTime", c, ok);
		}
#endif
		spread(a, a2);
	}

	// ---- timestamp(tm_t) and timegm
	{
		std::vector<tm_t> tms(N);
		std::uniform_int_distribution<int> Y(1971, 2099), M(1, 12), D(1, 28), H(0, 23), Mi(0, 59);
		for (auto& t : tms) t = tm_t(Y(rng), M(rng), D(rng), H(rng), Mi(rng), Mi(rng));
		std::vector<double> lib(N), civil_secs(N);
		auto f1 = [&] {
			for (std::size_t i = 0; i < N; ++i) lib[i] = timestamp(tms[i]);
		};
		auto f2 = [&] {
			for (std::size_t i = 0; i < N; ++i) {
				const auto& t = tms[i];
				civil_secs[i] = static_cast<double>(days_from_civil(t.year, static_cast<unsigned>(t.mon), static_cast<unsigned>(t.day)) * 86400 + t.hour * 3600 + t.min * 60 + t.sec);
			}
		};
		const double a = bench(f1, N), a2 = bench(f1, N), b = bench(f2, N);
		row("timestamp(tm_t)", a, "days_from_civil", b, lib == civil_secs);
		spread(a, a2);
#ifdef DATETIME_BENCH_ABSL
		auto f3 = [&] {
			for (std::size_t i = 0; i < N; ++i) {
				const auto& t = tms[i];
				absl::CivilSecond cs(t.year, t.mon, t.day, t.hour, t.min, t.sec);
				civil_secs[i] = static_cast<double>(absl::ToUnixSeconds(absl::FromCivil(cs, absl::UTCTimeZone())));
			}
		};
		const double timed_abseil = bench(f3, N);
		row("timestamp(tm_t)", a, "absl::FromCivil", timed_abseil, lib == civil_secs);
#endif
	}

	// ---- to_tm_t(time_t)
	{
		std::vector<std::time_t> ts(N);
		std::uniform_int_distribution<std::int64_t> T(0, 4102444800LL);
		for (auto& t : ts) t = static_cast<std::time_t>(T(rng));
		std::vector<std::int64_t> r1(N), r2(N);
		auto f1 = [&] {
			for (std::size_t i = 0; i < N; ++i) {
				const auto t = to_tm_t(ts[i]);
				r1[i] = ((((t.year * 100LL + t.mon) * 100 + t.day) * 100 + t.hour) * 100 + t.min) * 100 + t.sec;
			}
		};
		auto f2 = [&] {
			for (std::size_t i = 0; i < N; ++i) {
				const std::int64_t s = ts[i];
				const auto c = civil_from_days(s / 86400);
				const std::int64_t rem = s % 86400;
				r2[i] = ((((c.y * 100LL + c.m) * 100 + c.d) * 100 + rem / 3600) * 100 + rem % 3600 / 60) * 100 + rem % 60;
			}
		};
		const double a = bench(f1, N), a2 = bench(f1, N), b = bench(f2, N);
		row("to_tm_t(time_t)", a, "civil_from_days", b, r1 == r2);
		spread(a, a2);
	}

	// ---- iso8601(tm_t)
	{
		std::vector<tm_t> tms(N);
		std::uniform_int_distribution<int> Y(1971, 2099), M(1, 12), D(1, 28), H(0, 23), Mi(0, 59), Ms(0, 999999);
		for (std::size_t i = 0; i < N; ++i) tms[i] = tm_t(Y(rng), M(rng), D(rng), H(rng), Mi(rng), Mi(rng), (i % 2) ? Ms(rng) / 1e6 : 0.0, true);
		std::vector<std::string> r1(N), r2(N);
		auto f1 = [&] {
			for (std::size_t i = 0; i < N; ++i) r1[i] = iso8601(tms[i]);
		};
		auto f2 = [&] {
			for (std::size_t i = 0; i < N; ++i) {
				char b[40];
				r2[i].assign(b, hand_format(b, tms[i]));
			}
		};
		const double a = bench(f1, N), a2 = bench(f1, N), b = bench(f2, N);
		row("iso8601(tm_t), mixed fsec", a, "hand formatter", b, r1 == r2);
		spread(a, a2);
#ifdef DATETIME_BENCH_ABSL
		auto f3 = [&] {
			for (std::size_t i = 0; i < N; ++i) {
				const auto& t = tms[i];
				absl::CivilSecond cs(t.year, t.mon, t.day, t.hour, t.min, t.sec);
				r2[i] = absl::FormatTime("%Y-%m-%dT%H:%M:%SZ", absl::FromCivil(cs, absl::UTCTimeZone()), absl::UTCTimeZone());
			}
		};
		// abseil's format has no fraction, so only the timing is comparable
		row("iso8601(tm_t), mixed fsec", a, "absl::FormatTime", bench(f3, N, 3), false, false);
#endif
	}

	// ---- isDatetime
	{
		const auto v = make_inputs(rng, 3, N);
		std::size_t lib = 0, hand = 0;
		auto f1 = [&] {
			lib = 0;
			for (const auto& s : v) lib += isDatetime(s);
			keep(lib);
		};
		auto f2 = [&] {
			hand = 0;
			for (const auto& s : v) {
				double o;
				hand += hand_iso(s, o);
			}
			keep(hand);
		};
		const double a = bench(f1, N), a2 = bench(f1, N), b = bench(f2, N);
		row("isDatetime(.ffffffZ)", a, "hand ISO", b, lib == hand);
		spread(a, a2);
	}

	if (g_check) {
		if (g_bad) return 1;
		std::puts("datetime_compare: every comparator agrees");
	}
	return 0;
}
