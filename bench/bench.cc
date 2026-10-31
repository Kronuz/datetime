// Benchmark: current datetime vs the frozen reference (test/old, namespace
// DatetimeOld) on the same inputs. 4096 random inputs per row, best of 7 passes.
// Each row first checks that old and new agree on every input.
//
// Build: cmake -B build -DDATETIME_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build
// Run:   ./build/datetime_bench
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "datetime.h"
#include "old/datetime_old.h"

static std::mt19937_64 rng(42);
static unsigned r(unsigned n) { return static_cast<unsigned>(rng() % n); }
static std::string num(unsigned v, int w) { char b[16]; std::snprintf(b, sizeof(b), "%0*u", w, v); return b; }

static std::string date() { return num(1971 + r(129), 4) + "-" + num(1 + r(12), 2) + "-" + num(1 + r(28), 2); }
static std::string dt() { return date() + "T" + num(r(24), 2) + ":" + num(r(60), 2) + ":" + num(r(60), 2); }
static std::string frac() { return "." + num(r(1000000), 6); }
static std::string tz() { return std::string(r(2) ? "+" : "-") + num(r(24), 2) + ":" + num(r(60), 2); }

static volatile std::size_t sink;

static double best_ns(const std::vector<std::string>& in, const std::function<std::size_t(std::string_view)>& f) {
	double best = 1e30;
	for (int pass = 0; pass < 7; ++pass) {
		auto t0 = std::chrono::steady_clock::now();
		std::size_t acc = 0;
		for (const auto& s : in) { acc += f(s); }
		auto t1 = std::chrono::steady_clock::now();
		sink = acc;
		best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(in.size()));
	}
	return best;
}

static std::size_t mix(const DatetimeOld::tm_t& t) { return static_cast<std::size_t>(t.year ^ t.mon ^ t.day ^ t.hour ^ t.min ^ t.sec ^ int(t.fsec * 1e6) ^ t.is_utc); }
static std::size_t mix(const Datetime::tm_t& t) { return static_cast<std::size_t>(t.year ^ t.mon ^ t.day ^ t.hour ^ t.min ^ t.sec ^ int(t.fsec * 1e6) ^ t.is_utc); }

static void parse_row(const char* name, const std::vector<std::string>& in) {
	for (const auto& s : in) {
		auto a = DatetimeOld::DatetimeParser(s);
		auto b = Datetime::DatetimeParser(s);
		if (mix(a) != mix(b)) { std::printf("DIFFERENT on %s\n", s.c_str()); std::exit(1); }
	}
	double o = best_ns(in, [](std::string_view s) { return mix(DatetimeOld::DatetimeParser(s)); });
	double n = best_ns(in, [](std::string_view s) { return mix(Datetime::DatetimeParser(s)); });
	std::printf("%-34s old %8.1f ns  new %7.1f ns  %6.1fx\n", name, o, n, o / n);
}

int main() {
	std::vector<std::string> v;
	auto gen = [&](auto f) { v.clear(); for (int i = 0; i < 4096; ++i) { v.push_back(f()); } return v; };
	parse_row("parse YYYY-MM-DD", gen([] { return date(); }));
	parse_row("parse ...T00:00:00", gen([] { return dt(); }));
	parse_row("parse ...Z", gen([] { return dt() + "Z"; }));
	parse_row("parse .ffffffZ", gen([] { return dt() + frac() + "Z"; }));
	parse_row("parse ...+hh:mm", gen([] { return dt() + tz(); }));
	parse_row("parse .ffffff+hh:mm", gen([] { return dt() + frac() + tz(); }));
	parse_row("parse 2020/01/02 03:04:05", gen([] { auto d = date(); std::replace(d.begin(), d.end(), '-', '/'); return d + " " + num(r(24), 2) + ":" + num(r(60), 2) + ":" + num(r(60), 2); }));
	parse_row("parse ...||+1d/d", gen([] { return dt() + "||+" + std::to_string(1 + r(30)) + "d/d"; }));

	auto in = gen([] { return dt() + frac() + "Z"; });
	double o = best_ns(in, [](std::string_view s) { return DatetimeOld::isDatetime(s) ? 1u : 0u; });
	double n = best_ns(in, [](std::string_view s) { return Datetime::isDatetime(s) ? 1u : 0u; });
	std::printf("%-34s old %8.1f ns  new %7.1f ns  %6.1fx\n", "isDatetime .ffffffZ", o, n, o / n);

	std::vector<Datetime::tm_t> tms;
	std::vector<DatetimeOld::tm_t> olds;
	std::vector<std::string> keys(4096);
	for (int i = 0; i < 4096; ++i) {
		Datetime::tm_t t(1971 + r(129), 1 + r(12), 1 + r(28), r(24), r(60), r(60), r(2) ? r(1000000) / 1e6 : 0.0, true);
		tms.push_back(t);
		olds.push_back(DatetimeOld::tm_t(t.year, t.mon, t.day, t.hour, t.min, t.sec, t.fsec, t.is_utc));
	}
	auto fmt = [&](const char* name, auto&& fo, auto&& fn) {
		for (int i = 0; i < 4096; ++i) { if (fo(i) != fn(i)) { std::printf("DIFFERENT %s\n", name); std::exit(1); } }
		auto run = [&](auto&& f) {
			double best = 1e30;
			for (int pass = 0; pass < 7; ++pass) {
				auto t0 = std::chrono::steady_clock::now();
				std::size_t acc = 0;
				for (int i = 0; i < 4096; ++i) { acc += f(i).size(); }
				auto t1 = std::chrono::steady_clock::now();
				sink = acc;
				best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / 4096);
			}
			return best;
		};
		double a = run(fo), b = run(fn);
		std::printf("%-34s old %8.1f ns  new %7.1f ns  %6.1fx\n", name, a, b, a / b);
	};
	fmt("iso8601 trim, mixed fsec, UTC", [&](int i) { return DatetimeOld::iso8601(olds[i]); }, [&](int i) { return Datetime::iso8601(tms[i]); });
	fmt("iso8601 full .000000", [&](int i) { return DatetimeOld::iso8601(olds[i], false); }, [&](int i) { return Datetime::iso8601(tms[i], false); });

	std::vector<std::time_t> ts;
	for (int i = 0; i < 4096; ++i) { ts.push_back(static_cast<std::time_t>(r(4000000000u))); }
	auto run_t = [&](auto&& f) {
		double best = 1e30;
		for (int pass = 0; pass < 7; ++pass) {
			auto t0 = std::chrono::steady_clock::now();
			std::size_t acc = 0;
			for (auto t : ts) { acc += static_cast<std::size_t>(f(t).sec); }
			auto t1 = std::chrono::steady_clock::now();
			sink = acc;
			best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / 4096);
		}
		return best;
	};
	double a = run_t([](std::time_t t) { return DatetimeOld::to_tm_t(t); });
	double b = run_t([](std::time_t t) { return Datetime::to_tm_t(t); });
	std::printf("%-34s old %8.1f ns  new %7.1f ns  %6.1fx\n", "to_tm_t(time_t)", a, b, a / b);
}
