// Time and timedelta: parsing, conversion to and from clk_t and doubles, and formatting.
/*
 * Copyright © 2026 Germán Méndez Bravo (Kronuz)
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "datetime.h"

#include <cctype>                                 // for std::isdigit
#include <format>                                 // for std::format
#include <string>                                 // for std::string
#include <string_view>                            // for std::string_view
#include "strict_stox.hh"                         // for strict_stoull
#include "datetime_detail.h"                      // for the shared helpers


// The fraction of a time or timedelta, the dot and then digits only: 1 to MAX_FRACTION_DIGITS
// digits, like every other fraction.
static bool parse_dotted_fsec(std::string_view dotted, double& out) {
	if (dotted.size() < 2 || dotted.size() - 1 > MAX_FRACTION_DIGITS) { return false; }
	int errno_save;
	auto fsec = strict_stod(&errno_save, dotted);
	if (errno_save != 0) { return false; }
	out = Datetime::normalize_fsec(fsec);
	return true;
}


Datetime::clk_t
Datetime::TimeParser(std::string_view _time)
{
	int errno_save;
	clk_t clk;
	auto length = _time.length();
	switch (length) {
		case 5: // 00:00
			if (_time[2] == ':') {
				clk.hour = strict_stoul(&errno_save, _time.substr(0, 2));
				if (errno_save != 0) { goto error; }
				clk.min = strict_stoul(&errno_save, _time.substr(3, 2));
				if (errno_save != 0) { goto error; }
				return clk;
			}
			break;
		case 8: // 00:00:00
			if (_time[2] == ':' && _time[5] == ':') {
				clk.hour = strict_stoul(&errno_save, _time.substr(0, 2));
				if (errno_save != 0) { goto error; }
				clk.min = strict_stoul(&errno_save, _time.substr(3, 2));
				if (errno_save != 0) { goto error; }
				clk.sec = strict_stoul(&errno_save, _time.substr(6, 2));
				if (errno_save != 0) { goto error; }
				return clk;
			}
			break;
		default: //  00:00:00[+-]00:00  00:00:00.000...  00:00:00.000...[+-]00:00
			if (length > 9 && (_time[2] == ':' && _time[5] == ':')) {
				clk.hour = strict_stoul(&errno_save, _time.substr(0, 2));
				if (errno_save != 0) { goto error; }
				clk.min = strict_stoul(&errno_save, _time.substr(3, 2));
				if (errno_save != 0) { goto error; }
				clk.sec = strict_stoul(&errno_save, _time.substr(6, 2));
				if (errno_save != 0) { goto error; }
				switch (_time[8]) {
					case '-':
						clk.tz_s = '-';
						[[fallthrough]];
					case '+':
						if (length == 14 && _time[11] == ':') {
							clk.tz_h = strict_stoul(&errno_save, _time.substr(9, 2));
							if (errno_save != 0) { goto error; }
							clk.tz_m = strict_stoul(&errno_save, _time.substr(12, 2));
							if (errno_save != 0) { goto error; }
							return clk;
						}
						break;
					case '.': {
						auto it = _time.begin() + 8;
						const auto it_e = _time.end();
						for (auto aux = it + 1; aux != it_e; ++aux) {
							const auto& c = *aux;
							if (c < '0' || c > '9') {
								switch (c) {
									case '-':
										clk.tz_s = '-';
										[[fallthrough]];
									case '+':
										if ((it_e - aux) == 6) {
											auto aux_end = aux + 3;
											if (*aux_end == ':') {
												clk.tz_h = strict_stoul(&errno_save, std::string_view(aux + 1, aux_end - aux - 1));
												if (errno_save != 0) { goto error; }
												clk.tz_m = strict_stoul(&errno_save, std::string_view(aux_end + 1, it_e - aux_end - 1));
												if (errno_save != 0) { goto error; }
												double fsec;
												if (!parse_dotted_fsec(std::string_view(it, aux - it), fsec)) { goto error; }
												clk.fsec = fsec;
												return clk;
											}
										}
										break;
									default:
										break;
								}
								goto error;
							}
						}
						double fsec;
						if (!parse_dotted_fsec(std::string_view(it, it_e - it), fsec)) { goto error; }
						clk.fsec = fsec;
						return clk;
					}
					default:
						break;
				}
			}
			break;
	}

error:
	throw TimeError(std::format("Error format in time: {}, the format must be '00:00(:00(.0...)([+-]00:00))'", _time));
}


/*
 * Transforms double time to a struct clk_t.
 */
Datetime::clk_t
Datetime::time_to_clk_t(double t)
{
	if (isvalidTime(t)) {
		clk_t clk;
		if (t < 0) {
			auto _time = static_cast<int>(-t);
			clk.fsec = t + _time;
			if (clk.fsec < 0.0) {
				++_time;
			}
			clk.tz_h = _time / 3600;
			int aux;
			if (clk.tz_h < 100) {
				aux = clk.tz_h * 3600;
			} else {
				clk.tz_h = 99;
				aux = clk.tz_h * 3600;
			}
			clk.tz_m = (_time - aux) / 60;
			clk.sec = _time - aux - clk.tz_m * 60;
			if (clk.sec != 0) {
				clk.sec = 60 - clk.sec;
				++clk.tz_m;
			}
			clk.tz_s = '+';
			if (clk.fsec < 0) {
				clk.fsec += 1.0;
			}
		} else {
			auto _time = static_cast<int>(t);
			clk.hour = _time / 3600;
			if (clk.hour < 100) {
				auto aux = _time - clk.hour * 3600;
				clk.min = aux / 60;
				clk.sec = aux - clk.min * 60;
			} else {
				if (clk.hour < 199) {
					auto aux = _time - clk.hour * 3600;
					clk.tz_h = clk.hour - 99;
					clk.hour = 99;
					clk.min = aux / 60;
					clk.sec = aux - clk.min * 60;
				} else {
					clk.tz_h = 99;
					clk.hour = 99;
					auto aux = _time - 712800; // 198 * 3600
					clk.min = aux / 60;
					if (clk.min < 100) {
						clk.sec = aux - clk.min * 60;
					} else {
						if (clk.min < 199) {
							clk.sec = aux - clk.min * 60;
							clk.tz_m = 198 - clk.min;
							clk.min = 99;
						} else {
							clk.tz_m = 99;
							clk.min = 99;
							clk.sec = aux - 11880; // 198 * 60
						}
					}
				}
			}
			clk.tz_s = '-';
			clk.fsec = t - _time;
		}

		return clk;
	}

	throw TimeError("Bad serialized time value");
}


double
Datetime::time_to_double(const clk_t& clk)
{
	int hour, min;
	if (clk.tz_s == '-') {
		hour = clk.hour + clk.tz_h;
		min = clk.min + clk.tz_m;
	} else {
		hour = clk.hour - clk.tz_h;
		min = clk.min - clk.tz_m;
	}
	return clk.sec + clk.fsec + (hour * 60 + min) * 60;
}


std::string
Datetime::time_to_string(const clk_t& clk, bool trim)
{
	if (trim && clk.tz_h == 0 && clk.tz_m == 0) {
		auto res = std::format("{:02}:{:02}:{:02}",
			clk.hour, clk.min, clk.sec);
		if (clk.fsec > 0) {
			res += std::format("{:.6f}", clk.fsec).erase(0, 1);
			auto it_e = res.end();
			auto it = it_e - 1;
			for (; *it == '0'; --it) { }
			if (*it != '.') {
				++it;
			}
			res.erase(it, it_e);
		}
		return res;
	}

	if (clk.fsec > 0) {
		auto res = std::format("{:02}:{:02}:{:02}",
			clk.hour, clk.min, clk.sec);
		res += std::format("{:.6f}{}{:02}:{:02}", clk.fsec,
			clk.tz_s, clk.tz_h, clk.tz_m).erase(0, 1);
		return res;
	}

	return std::format("{:02}:{:02}:{:02}.000000{}{:02}:{:02}",
		clk.hour, clk.min, clk.sec,
		clk.tz_s, clk.tz_h, clk.tz_m);
}


std::string
Datetime::time_to_string(double t, bool trim)
{
	return time_to_string(time_to_clk_t(t), trim);
}


bool
Datetime::isTime(std::string_view _time) {
	auto length = _time.length();
	switch (length) {
		case 5: // 00:00
			return (std::isdigit(_time[0]) != 0) && (std::isdigit(_time[1]) != 0) && _time[2] == ':' &&
				(std::isdigit(_time[3]) != 0) && (std::isdigit(_time[4]) != 0);
		case 8: // 00:00:00
			return (std::isdigit(_time[0]) != 0) && (std::isdigit(_time[1]) != 0) && _time[2] == ':' &&
				(std::isdigit(_time[3]) != 0) && (std::isdigit(_time[4]) != 0) && _time[5] == ':' &&
				(std::isdigit(_time[6]) != 0) && (std::isdigit(_time[7]) != 0);
		default: //  00:00:00[+-]00:00  00:00:00.000...  00:00:00.000...[+-]00:00
			if (length > 9 && (std::isdigit(_time[0]) != 0) && (std::isdigit(_time[1]) != 0) && _time[2] == ':' &&
				(std::isdigit(_time[3]) != 0) && (std::isdigit(_time[4]) != 0) && _time[5] == ':' &&
				(std::isdigit(_time[6]) != 0) && (std::isdigit(_time[7]) != 0)) {
				switch (_time[8]) {
					case '+':
					case '-':
						return length == 14 && (std::isdigit(_time[9]) != 0) && (std::isdigit(_time[10]) != 0) && _time[11] == ':' &&
							(std::isdigit(_time[12]) != 0) && (std::isdigit(_time[13]) != 0);
					case '.':
						for (size_t i = 9; i < length; ++i) {
							const auto c = _time[i];
							if (std::isdigit(c) == 0) {
								return (c == '+' || c == '-') && length == (i + 6) && (std::isdigit(_time[i + 1]) != 0) &&
									(std::isdigit(_time[i + 2]) != 0) && _time[i + 3] == ':' && (std::isdigit(_time[i + 4]) != 0) &&
									(std::isdigit(_time[i + 5]) != 0);
							}
						}
						return true;
				}
			}
			return false;
	}
}


Datetime::clk_t
Datetime::TimedeltaParser(std::string_view timedelta)
{
	int errno_save;
	clk_t clk;
	auto size = timedelta.size();
	switch (size) {
		case 6: // [+-]00:00
			switch (timedelta[0]) {
				case '-':
					clk.tz_s = '-';
					[[fallthrough]];
				case '+':
					if (timedelta[3] == ':') {
						clk.hour = strict_stoul(&errno_save, timedelta.substr(1, 2));
						if (errno_save != 0) { goto error; }
						clk.min = strict_stoul(&errno_save, timedelta.substr(4, 2));
						if (errno_save != 0) { goto error; }
						return clk;
					}
					[[fallthrough]];
				default:
					break;
			}
			break;

		case 9: // [+-]00:00:00
			switch (timedelta[0]) {
				case '-':
					clk.tz_s = '-';
					[[fallthrough]];
				case '+':
					if (timedelta[3] == ':' && timedelta[6] == ':') {
						clk.hour = strict_stoul(&errno_save, timedelta.substr(1, 2));
						if (errno_save != 0) { goto error; }
						clk.min = strict_stoul(&errno_save, timedelta.substr(4, 2));
						if (errno_save != 0) { goto error; }
						clk.sec = strict_stoul(&errno_save, timedelta.substr(7, 2));
						if (errno_save != 0) { goto error; }
						return clk;
					}
					[[fallthrough]];
				default:
					break;
			}
			break;

		case 0: // nothing to read
			break;

		default: //  [+-]00:00:00.000...
			switch (timedelta[0]) {
				case '-':
					clk.tz_s = '-';
					[[fallthrough]];
				case '+':
					if (size > 10 && (timedelta[3] == ':' && timedelta[6] == ':' && timedelta[9] == '.')) {
						clk.hour = strict_stoul(&errno_save, timedelta.substr(1, 2));
						if (errno_save != 0) { goto error; }
						clk.min = strict_stoul(&errno_save, timedelta.substr(4, 2));
						if (errno_save != 0) { goto error; }
						clk.sec = strict_stoul(&errno_save, timedelta.substr(7, 2));
						if (errno_save != 0) { goto error; }
						auto it = timedelta.begin() + 9;
						const auto it_e = timedelta.end();
						for (auto aux = it + 1; aux != it_e; ++aux) {
							const auto& c = *aux;
							if (c < '0' || c > '9') {
								goto error;
							}
						}
						double fsec;
						if (!parse_dotted_fsec(std::string_view(it, it_e - it), fsec)) { goto error; }
						clk.fsec = fsec;
						return clk;
					}
					[[fallthrough]];
				default:
					break;
			}
			break;
	}

error:
	throw TimedeltaError(std::format("Error format in timedelta: {}, the format must be '[+-]00:00(:00(.0...))'", timedelta));
}


/*
 * Transforms double timedelta to a struct clk_t.
 */
Datetime::clk_t
Datetime::timedelta_to_clk_t(double t)
{
	if (isvalidTimedelta(t)) {
		clk_t clk;
		if (t < 0) {
			t *= -1.0;
			clk.tz_s = '-';
		}

		auto _time = static_cast<int>(t);
		clk.hour = _time / 3600;
		if (clk.hour < 100) {
			auto aux = _time - clk.hour * 3600;
			clk.min = aux / 60;
			clk.sec = aux - clk.min * 60;
		} else {
			clk.hour = 99;
			auto aux = _time - clk.hour * 3600;
			clk.min = aux / 60;
			if (clk.min < 100) {
				clk.sec =  aux - clk.min * 60;
			} else {
				clk.min = 99;
				clk.sec =  aux - clk.min * 60;
			}
		}
		clk.fsec = t - _time;

		return clk;
	}

	throw TimedeltaError("Bad serialized timedelta value");
}


double
Datetime::timedelta_to_double(const clk_t& clk)
{
	double t = (clk.hour * 60 + clk.min) * 60 + clk.sec + clk.fsec;
	return clk.tz_s == '-' ? -t : t;
}


std::string
Datetime::timedelta_to_string(const clk_t& clk, bool trim)
{
	if (trim) {
		auto res = std::format("{}{:02}:{:02}:{:02}",
			clk.tz_s, clk.hour, clk.min, clk.sec);
		if (clk.fsec > 0) {
			res += std::format("{:.6f}", clk.fsec).erase(0, 1);
			auto it_e = res.end();
			auto it = it_e - 1;
			for (; *it == '0'; --it) { }
			if (*it != '.') {
				++it;
			}
			res.erase(it, it_e);
		}
		return res;
	}

	if (clk.fsec > 0) {
		return std::format("{}{:02}:{:02}:{:02}",
			clk.tz_s, clk.hour, clk.min, clk.sec);
	}

	return std::format("{}{:02}:{:02}:{:02}.000000",
		clk.tz_s, clk.hour, clk.min, clk.sec);
}


std::string
Datetime::timedelta_to_string(double t, bool trim)
{
	return timedelta_to_string(timedelta_to_clk_t(t), trim);
}


bool
Datetime::isTimedelta(std::string_view timedelta)
{
	auto size = timedelta.size();
	switch (size) {
		case 6: // [+-]00:00
			return (timedelta[0] == '+' || timedelta[0] == '-') && (std::isdigit(timedelta[1]) != 0) && (std::isdigit(timedelta[2]) != 0) &&
				timedelta[3] == ':' && (std::isdigit(timedelta[4]) != 0) && (std::isdigit(timedelta[5]) != 0);
		case 9: // [+-]00:00:00
			return (timedelta[0] == '+' || timedelta[0] == '-') && (std::isdigit(timedelta[1]) != 0) && (std::isdigit(timedelta[2]) != 0) &&
				timedelta[3] == ':' && (std::isdigit(timedelta[4]) != 0) && (std::isdigit(timedelta[5]) != 0) &&
				timedelta[6] == ':' && (std::isdigit(timedelta[7]) != 0) && (std::isdigit(timedelta[8]) != 0);
		default: // [+-]00:00:00  [+-]00:00:00.000...
			if (size > 10 && (timedelta[0] == '+' || timedelta[0] == '-') && (std::isdigit(timedelta[1]) != 0) &&
				(std::isdigit(timedelta[2]) != 0) && timedelta[3] == ':' && (std::isdigit(timedelta[4]) != 0) &&
				(std::isdigit(timedelta[5]) != 0) && timedelta[6] == ':' && (std::isdigit(timedelta[7]) != 0) &&
				(std::isdigit(timedelta[8]) != 0) && timedelta[9] == '.') {
				for (size_t i = 10; i < size; ++i) {
					if (std::isdigit(timedelta[i]) == 0) {
						return false;
					}
				}
				return true;
			}
			return false;
	}
}
