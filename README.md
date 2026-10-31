# datetime

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/datetime/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/datetime/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

Date and time parsing for C++20: ISO 8601 timestamps, Elasticsearch-style date
math (`2021-01-01||+1M-1d/d`), times of day, and durations, with ISO 8601
formatting back out. For query languages and APIs that take dates as text.

![datetime: ISO 8601, date math, times, and deltas parse into structured values and format back](docs/datetime.svg)

## ✨ Highlights

- **ISO 8601**, date-only through fractional seconds, with `Z` or `±HH:MM`
  offsets.
- **Date math** after `||`: add and subtract `y M w d h m s`, round up to the end
  of a unit with `/M`, or down to its start with `//M`, leap years included.
- **Times and durations**: `"15:09:26.5"` and `"+01:30:00"`, to and from seconds.
- **Clear errors**: `DatetimeError` and its subclasses, all `std::runtime_error`.
- **Predicates**: `isDate`, `isDatetime`, `isTime`, `isTimedelta`.

## Behavior changes

These inputs changed result (correctness fixes); everything else parses and
formats as before.

- `Iso8601Parser(str, tm)` and `Iso8601Parser(str)` (and so `isDatetime` and
  `DatetimeParser`) now agree on every input. A negative hour, minute or second
  (`T-3:04:05`) is `OUT_OF_RANGE` in both (the filling overload used to accept
  it). A fraction dot with no digits (`...:05.`, `...:05.Z`) or with more than 62
  digits is `ERROR` in both (the validating overload used to accept it).
- A loose-format string ending in `Z` (`2020/01/02 03:04:05Z`) parses as UTC
  (`is_utc` set), like the ISO form; it used to always throw.
- Date-math terms such as `+2147483647d` are computed in 64-bit arithmetic; they
  used to overflow `int` (undefined behavior). The supported years are 1 to
  `INT_MAX`; a result beyond them throws `Year is out of range`.
- `+12M` (any month count that lands on a multiple of 12 past December) no longer
  throws `Month must be in 1..12`: `2020-12-15||+12M` is `2021-12-15`.
  `normalizeMonths` now maps any month to 1..12 and carries the years.
- A loose-format offset (`2020/01/02 03:04:05+05:30`) sets `is_utc`, as the ISO
  form does: an applied offset means the result is the UTC instant.
- Unchanged on purpose: year 1 with an offset that moves the instant before
  0001-01-01 throws `Year is out of range` (the instant is outside the supported
  years).

## 📦 Installation

Requires a C++20 compiler and CMake 3.14 or newer; it fetches
[strict-stox](https://github.com/Kronuz/strict-stox) and compiles one source file.
CI builds it on Linux (x86_64, arm64), macOS (arm64, x86_64), and FreeBSD, plus
ASan, UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(datetime
  GIT_REPOSITORY https://github.com/Kronuz/datetime.git
  GIT_TAG        main)
FetchContent_MakeAvailable(datetime)

target_link_libraries(my_app PRIVATE datetime::datetime)
```

## 🚀 Quick start

```cpp
#include "datetime.h"

#include <cstdio>

int main() {
	auto tm = Datetime::DatetimeParser(std::string_view("2021-03-14T15:09:26.5Z"));
	std::printf("%f\n", Datetime::timestamp(tm));                   // Unix seconds
	std::puts(Datetime::iso8601(tm).c_str());                       // 2021-03-14T15:09:26.5Z

	auto end_of_feb = Datetime::DatetimeParser(std::string_view("2020-02-10||/M"));   // 2020-02-29, end of day
	std::puts(Datetime::iso8601(end_of_feb).c_str());
}
```

[`examples/demo.cc`](examples/demo.cc) walks every input form and the rounding
rules.

## 🎯 When to use it

- ✅ Parsing dates from users or query strings, with relative date math.
- ❌ Time zones by name (`America/Mexico_City`) or calendars other than
  Gregorian: offsets only.
- ❌ "now": resolving relative-to-now expressions is the caller's job.

## 📚 Documentation

- [docs/API.md](docs/API.md): usage, every parser and formatter, and the scope.
- [ARCHITECTURE.md](ARCHITECTURE.md): the grammar and how date math is applied.
- [AGENTS.md](AGENTS.md): repository map and traps.

## 🛠️ Building and testing

```sh
cmake -B build && cmake --build build && ctest --test-dir build
./build/datetime_demo
```

## ⚡ How it is fast

Parsing and formatting avoid per-field library calls and allocations: fixed-width
digits are read directly, fractions up to 6 digits are integer microseconds, a
UTC offset is one integer add, the loose formats (`2020/01/02 3:04`) and
`|| date math` use hand scanners instead of `std::regex`, `iso8601` writes digits
into a stack buffer (fractions rounded exactly, ties to even, like `{:.6f}`), and
`to_tm_t` uses an integer civil-from-days conversion instead of `gmtime_r`.
Results are identical to the earlier implementation on every input, accepted or
rejected (`ctest` runs a differential test against a frozen copy). Typical
speedups on one core: ISO parse 5-9x, loose format ~80x, date math ~15x,
`iso8601` ~9x, `to_tm_t` ~5x. Run `-DDATETIME_BUILD_BENCH=ON` for the numbers.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) first. CI must
pass on every platform.

## 📄 License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE). Extracted from
[Xapiand](https://github.com/Kronuz/Xapiand).
