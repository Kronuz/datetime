# datetime

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/datetime/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/datetime/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

Date and time parsing for C++20: ISO 8601, Elasticsearch-style date math, times and durations,
and ISO 8601 output. Hand-written scanners read fixed-width digits directly and validate every
field, and `iso8601` writes into a stack buffer, so a parse or a format costs tens of
nanoseconds and reports exactly what was wrong. Reach for it in query languages and APIs that
take dates as text.

## ✨ Highlights

- **ISO 8601**, date-only through fractional seconds, with `Z` or `±HH:MM` offsets applied to a
  UTC instant; loose forms such as `2020/01/02 03:04:05` too.
- **Date math** after `||`: add and subtract `y M w d h m s`, round up to the end of a unit
  with `/M` or down to its start with `//M`, leap years and month lengths included.
- **Times and durations**: `"15:09:26.5"` and `"+01:30:00"`, to and from seconds.
- **Years 1 to `INT_MAX`.** Date-math terms are 64-bit, and a result outside the range throws.
- **Clear errors**: `DatetimeError` and its subclasses, all `std::runtime_error`.
- **Predicates**: `isDate`, `isDatetime`, `isTime`, `isTimedelta`.

## 🚀 Quick start

```cpp
#include "datetime.h"

#include <cstdio>
#include <string_view>

int main() {
	auto tm = Datetime::DatetimeParser(std::string_view("2021-03-14T15:09:26.5Z"));
	std::printf("%.1f\n", Datetime::timestamp(tm));                 // 1615734566.5
	std::puts(Datetime::iso8601(tm).c_str());                       // 2021-03-14T15:09:26.5Z

	// Date math: one month later, then round up to the end of that month.
	auto end_of_month = Datetime::DatetimeParser(std::string_view("2020-01-10||+1M/M"));
	std::puts(Datetime::iso8601(end_of_month).c_str());             // 2020-02-29T23:59:59.999999
}
```

[`examples/demo.cc`](examples/demo.cc) walks every input form and the rounding rules.

## 📦 Installation

Requires a C++20 compiler and CMake 3.14 or newer; it fetches
[strict-stox](https://github.com/Kronuz/strict-stox) at a pinned commit and compiles one source
file. CI builds it on Linux (x86_64, arm64), macOS (arm64, x86_64), and FreeBSD, plus ASan,
UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(datetime
  GIT_REPOSITORY https://github.com/Kronuz/datetime.git
  GIT_TAG        f2541338c2d689f3ab83c1734f096a6a51a1e6cf)  # main tracks the tip
FetchContent_MakeAvailable(datetime)

target_link_libraries(my_app PRIVATE datetime::datetime)
```

Then `#include "datetime.h"`; everything is in the `Datetime` namespace.

## 🎯 When to use it

It fits dates typed by users or carried in query strings, including relative date math.

Not for:

- **Named time zones** (`America/Mexico_City`): it applies numeric offsets only.
- **Calendars other than the proleptic Gregorian.**
- **"now".** Resolving expressions relative to the current time is the caller's job.

## ⚡ Performance

`datetime_compare` times each entry point against independent implementations: a hand-written
ISO 8601 parser and formatter that reads one grammar and reports failure with a bool, the
days-from-civil and civil-from-days algorithms, libc `strptime` + `timegm`, and abseil 20260817's
`ParseTime`, `FromCivil` and `FormatTime`. ns per call, one thread, 4,096 random inputs, best of
two runs of seven passes, Apple clang 17 with `-O3`:

| Operation | Apple M4 Pro | comparators | Intel Core i9-9980HK | comparators |
| :-- | --: | :-- | --: | :-- |
| parse `2020-01-02` | 10.4 | hand 5.9 | 26.3 | hand 15.1 |
| parse `2020-01-02T03:04:05Z` | 16.0 | hand 7.8, absl 100.8 | 40.2 | hand 22.7, absl 264.1 |
| parse `2020-01-02T03:04:05.123456Z` | 20.5 | hand 10.0, absl 133.1 | 48.6 | hand 28.2, absl 355.7 |
| parse `2020-01-02T03:04:05+05:30` | 28.8 | hand 8.9 | 62.7 | hand 26.7 |
| parse `2020/01/02 03:04:05` | 19.7 | hand 9.1, `strptime`+`timegm` 2192 | 56.3 | hand 24.4, `strptime`+`timegm` 7041 |
| `timestamp(tm_t)` | 2.8 | days-from-civil 1.8, absl 27.8 | 7.4 | days-from-civil 4.8, absl 71.5 |
| `to_tm_t(time_t)` | 8.9 | civil-from-days 6.1 | 24.1 | civil-from-days 14.3 |
| `iso8601(tm_t)` | 22.3 | hand 9.0, absl 1128 | 48.0 | hand 26.3, absl 2008 |
| `isDatetime` | 13.8 | hand 10.2 | 33.6 | hand 27.8 |

`datetime` takes 1.2x to 3.2x the hand-written parser's time, for the loose forms, offsets, date
math, field validation and error reports the hand parser lacks, and about a sixth of abseil's
`ParseTime`. abseil's `FormatTime` writes no fraction, so its row compares time only. Repeated
runs spread under 3% for most rows, up to 8% for two parse rows on the M4 Pro and 30% for
`iso8601` on the i9.

```sh
cmake -B build -DDATETIME_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/datetime_compare
```

## 📚 Documentation

- [docs/API.md](docs/API.md): every parser and formatter, the behavior at the edges, and the
  scope.
- [ARCHITECTURE.md](ARCHITECTURE.md): the grammar and how date math is applied.
- [CHANGELOG.md](CHANGELOG.md): changes with their measurements, newest first.
- [AGENTS.md](AGENTS.md): repository map and traps.
- [Catalog entry](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#datetime) in
  Kronuz C++ Libraries.

## 🛠️ Building and testing

```sh
cmake -B build && cmake --build build && ctest --test-dir build
./build/datetime_demo
cmake -B build-bench -DDATETIME_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build-bench
```

`ctest` runs `datetime_diff_test`, which compares every entry point with a frozen reference
implementation in `test/old/` on random, mutated and exhaustive inputs, and
`datetime_datemath_test`, which checks date math against independent references. The input
classes whose result differs from the reference are listed at the top of `test/diff_test.cc`.
With `-DDATETIME_BUILD_BENCH=ON`, `ctest` also checks that every comparator in
`datetime_compare` returns the library's results, and `datetime_bench` times the library against
the reference.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) and the catalog's
[rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo and (c) 2015-2020 Dubalu LLC. See
[LICENSE](LICENSE). Extracted from [Xapiand](https://github.com/Kronuz/Xapiand).
