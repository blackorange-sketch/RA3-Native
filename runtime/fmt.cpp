// printf / scanf for guest argument lists.
#include <cmath>

#include "rt.h"

namespace rt
{
namespace
{
struct Spec { bool left = false, plus = false, space = false, alt = false, zero = false; int width = -1, prec = -1; int size = 0; /*h=-1 l=1 ll=2*/ char conv = 0; };

std::string pad(std::string s, const Spec& sp, bool numeric)
{
	if (sp.width > 0 && (int)s.size() < sp.width)
	{
		size_t n = sp.width - s.size();
		if (sp.left) s += std::string(n, ' ');
		else if (sp.zero && numeric)
		{
			size_t sign = (!s.empty() && (s[0] == '-' || s[0] == '+' || s[0] == ' ')) ? 1 : 0;
			if (s.size() > sign + 1 && s[sign] == '0' && (s[sign + 1] == 'x' || s[sign + 1] == 'X')) sign += 2;
			s.insert(sign, std::string(n, '0'));
		}
		else s.insert(0, std::string(n, ' '));
	}
	return s;
}

template <class Ch, class Str>
Str run(const Str& fmt, ArgReader& ap, bool wideStrDefault)
{
	Str out;
	auto put = [&](const std::string& s) { for (unsigned char ch : s) out += (Ch)ch; };
	for (size_t i = 0; i < fmt.size(); i++)
	{
		if (fmt[i] != '%') { out += fmt[i]; continue; }
		i++;
		if (i >= fmt.size()) break;
		if (fmt[i] == '%') { out += (Ch)'%'; continue; }
		Spec sp;
		for (;; i++)
		{
			if (fmt[i] == '-') sp.left = true; else if (fmt[i] == '+') sp.plus = true; else if (fmt[i] == ' ') sp.space = true;
			else if (fmt[i] == '#') sp.alt = true; else if (fmt[i] == '0') sp.zero = true; else break;
		}
		if (fmt[i] == '*') { int w = (int)ap.u32(); if (w < 0) { sp.left = true; w = -w; } sp.width = w; i++; }
		else { int w = 0; bool any = false; while (fmt[i] >= '0' && fmt[i] <= '9') { w = w * 10 + (fmt[i] - '0'); i++; any = true; } if (any) sp.width = w; }
		if (fmt[i] == '.')
		{
			i++;
			if (fmt[i] == '*') { sp.prec = (int)ap.u32(); i++; }
			else { int p = 0; while (fmt[i] >= '0' && fmt[i] <= '9') { p = p * 10 + (fmt[i] - '0'); i++; } sp.prec = p; }
		}
		for (;; i++)
		{
			if (fmt[i] == 'h') sp.size = -1; else if (fmt[i] == 'l') sp.size = sp.size == 1 ? 2 : 1; else if (fmt[i] == 'L') sp.size = 2;
			else if (fmt[i] == 'I' && fmt[i + 1] == '6' && fmt[i + 2] == '4') { sp.size = 2; i += 2; }
			else if (fmt[i] == 'I' && fmt[i + 1] == '3' && fmt[i + 2] == '2') { sp.size = 0; i += 2; }
			else if (fmt[i] == 'z' || fmt[i] == 't' || fmt[i] == 'j') sp.size = 0;
			else break;
		}
		sp.conv = (char)fmt[i];
		char buf[512];
		switch (sp.conv)
		{
		case 'd': case 'i':
		{
			int64_t v = sp.size == 2 ? (int64_t)ap.u64() : sp.size == -1 ? (int16_t)ap.u32() : (int32_t)ap.u32();
			std::string s = std::to_string(v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v);
			if (sp.prec >= 0 && (int)s.size() < sp.prec) s.insert(0, sp.prec - s.size(), '0');
			if (v < 0) s.insert(0, "-"); else if (sp.plus) s.insert(0, "+"); else if (sp.space) s.insert(0, " ");
			Spec s2 = sp; if (sp.prec >= 0) s2.zero = false;
			put(pad(s, s2, true));
			break;
		}
		case 'u': case 'x': case 'X': case 'o':
		{
			uint64_t v = sp.size == 2 ? ap.u64() : sp.size == -1 ? (uint16_t)ap.u32() : (uint32_t)ap.u32();
			const char* f = sp.conv == 'u' ? "%llu" : sp.conv == 'x' ? "%llx" : sp.conv == 'X' ? "%llX" : "%llo";
			snprintf(buf, sizeof buf, f, (unsigned long long)v);
			std::string s = buf;
			if (sp.prec >= 0 && (int)s.size() < sp.prec) s.insert(0, sp.prec - s.size(), '0');
			if (sp.alt && v) { if (sp.conv == 'x') s.insert(0, "0x"); else if (sp.conv == 'X') s.insert(0, "0X"); else if (sp.conv == 'o') s.insert(0, "0"); }
			Spec s2 = sp; if (sp.prec >= 0) s2.zero = false;
			put(pad(s, s2, true));
			break;
		}
		case 'p': { snprintf(buf, sizeof buf, "%08X", ap.u32()); put(pad(buf, sp, true)); break; }
		case 'c':
		{
			uint32_t v = ap.u32();
			std::string s(1, (char)v);
			put(pad(s, sp, false));
			break;
		}
		case 's': case 'S':
		{
			uint32_t a = ap.u32();
			bool wide = sp.conv == 's' ? (wideStrDefault ? sp.size != -1 : sp.size == 1) : (wideStrDefault ? sp.size == 1 : sp.size != -1);
			std::string s;
			if (!a) s = "(null)";
			else if (wide)
			{
				std::u16string w = gwstr(a);
				if (sp.prec >= 0 && (int)w.size() > sp.prec) w.resize(sp.prec);
				s = utf16to8(w);
			}
			else
			{
				s = gstr(a);
				if (sp.prec >= 0 && (int)s.size() > sp.prec) s.resize(sp.prec);
			}
			put(pad(s, sp, false));
			break;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
		{
			double v = ap.f64();
			char f[16];
			snprintf(f, sizeof f, "%%%s%s%s%s.*%c", sp.plus ? "+" : "", sp.space ? " " : "", sp.alt ? "#" : "", "", sp.conv);
			if (std::isnan(v)) { put(pad(std::signbit(v) ? "-1.#IND00" : "1.#QNAN0", sp, false)); break; }
			if (std::isinf(v)) { put(pad(v < 0 ? "-1.#INF00" : "1.#INF00", sp, false)); break; }
			snprintf(buf, sizeof buf, f, sp.prec < 0 ? 6 : sp.prec, v);
			put(pad(buf, sp, true));
			break;
		}
		case 'n': ap.u32(); break;
		default: out += (Ch)'%'; out += (Ch)sp.conv; break;
		}
	}
	return out;
}
}

std::string formatA(const std::string& fmt, ArgReader& ap) { return run<char, std::string>(fmt, ap, false); }
std::u16string formatW(const std::u16string& fmt, ArgReader& ap) { return run<char16_t, std::u16string>(fmt, ap, true); }

// Minimal sscanf: %d %i %u %x %f %lf %g %s %c %[...] with width and %*; enough for config parsing.
int scanA(const std::string& in, const std::string& fmt, ArgReader& ap, bool secure)
{
	size_t p = 0;
	int count = 0;
	auto skipWs = [&] { while (p < in.size() && isspace((unsigned char)in[p])) p++; };
	for (size_t i = 0; i < fmt.size(); i++)
	{
		if (isspace((unsigned char)fmt[i])) { skipWs(); continue; }
		if (fmt[i] != '%') { if (p < in.size() && in[p] == fmt[i]) { p++; continue; } return count; }
		i++;
		if (fmt[i] == '%') { skipWs(); if (p < in.size() && in[p] == '%') { p++; continue; } return count; }
		bool suppress = false;
		if (fmt[i] == '*') { suppress = true; i++; }
		int width = 0;
		while (isdigit((unsigned char)fmt[i])) width = width * 10 + (fmt[i++] - '0');
		int size = 0;
		while (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'L') { size = fmt[i] == 'l' ? size + 1 : fmt[i] == 'h' ? -1 : 2; i++; }
		char conv = fmt[i];
		if (conv != 'c' && conv != '[') skipWs();
		if (p >= in.size() && conv != 'n') return count ? count : -1;
		switch (conv)
		{
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
		{
			const char* s = in.c_str() + p;
			char* end;
			int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
			errno = 0;
			long long v = conv == 'u' || base == 16 ? (long long)strtoull(s, &end, base) : strtoll(s, &end, base);
			if (end == s) return count;
			p += end - s;
			if (!suppress)
			{
				uint32_t dst = ap.u32();
				if (size == 2) guest::store<int64_t>(dst, v); else if (size == -1) guest::store<int16_t>(dst, (int16_t)v); else guest::store<int32_t>(dst, (int32_t)v);
				count++;
			}
			break;
		}
		case 'f': case 'g': case 'e': case 'E': case 'G':
		{
			const char* s = in.c_str() + p;
			char* end;
			double v = strtod(s, &end);
			if (end == s) return count;
			p += end - s;
			if (!suppress)
			{
				uint32_t dst = ap.u32();
				if (size >= 1) guest::store<double>(dst, v); else guest::store<float>(dst, (float)v);
				count++;
			}
			break;
		}
		case 's':
		{
			size_t st = p;
			while (p < in.size() && !isspace((unsigned char)in[p]) && (width == 0 || (int)(p - st) < width)) p++;
			if (!suppress) { uint32_t dst = ap.u32(); if (secure) ap.u32(); putStr(dst, in.substr(st, p - st)); count++; }
			break;
		}
		case 'c':
		{
			int n = width ? width : 1;
			if (p + n > in.size()) return count ? count : -1;
			if (!suppress) { uint32_t dst = ap.u32(); if (secure) ap.u32(); memcpy(guest::host(dst), in.data() + p, n); count++; }
			p += n;
			break;
		}
		case '[':
		{
			i++;
			bool neg = false;
			if (fmt[i] == '^') { neg = true; i++; }
			std::string set;
			if (fmt[i] == ']') { set += ']'; i++; }
			while (i < fmt.size() && fmt[i] != ']')
			{
				if (fmt[i + 1] == '-' && i + 2 < fmt.size() && fmt[i + 2] != ']') { for (int ch = (unsigned char)fmt[i]; ch <= (unsigned char)fmt[i + 2]; ch++) set += (char)ch; i += 3; }
				else set += fmt[i++];
			}
			size_t st = p;
			while (p < in.size() && ((set.find(in[p]) != std::string::npos) != neg) && (width == 0 || (int)(p - st) < width)) p++;
			if (p == st) return count;
			if (!suppress) { uint32_t dst = ap.u32(); if (secure) ap.u32(); putStr(dst, in.substr(st, p - st)); count++; }
			break;
		}
		default: return count;
		}
	}
	return count;
}
}
