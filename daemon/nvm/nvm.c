/* SPDX-License-Identifier: MIT */
/*
 * NVM parameter store.
 *
 * Reproduces the on-disk format and lookup rules of the legacy driver so the
 * engine sees exactly the values it used to (docs/OS-LAYER.md, "NVM"):
 *   static parameters  <static>/<hwProfile>/<NAME>
 *   dynamic parameters <dynamic>/<hwInstNum>-<hwInstName>/<NAME>
 *   country parameters <static>/<hwProfile>/Region/<T35><suffix>, falling back
 *                      to <static>/<hwProfile>/Profile/<reference><suffix>
 * Files hold comma-separated hex tokens or a quoted string.
 */
#include "hsf_os.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#ifndef HSF_NVM_STATIC_DIR
#define HSF_NVM_STATIC_DIR "/usr/share/hsfmodem/nvm"
#endif
#ifndef HSF_NVM_DYNAMIC_DIR
#define HSF_NVM_DYNAMIC_DIR "/var/lib/hsfmodem/dynamic"
#endif

static char static_dir[PATH_MAX] = HSF_NVM_STATIC_DIR;
static char dynamic_dir[PATH_MAX] = HSF_NVM_DYNAMIC_DIR;

void hsf_nvm_set_dirs(const char *s, const char *d)
{
	snprintf(static_dir, sizeof(static_dir), "%s", s);
	snprintf(dynamic_dir, sizeof(dynamic_dir), "%s", d);
}

/* snprintf into a path buffer; a truncated path is an error, never a lookup. */
static bool make_path(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static bool make_path(char *out, size_t n, const char *fmt, ...)
{
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(out, n, fmt, ap);
	va_end(ap);
	if (len < 0 || (size_t)len >= n) {
		hsf_problem("NVM path too long: %s...", out);
		return false;
	}
	return true;
}

static const char *const code_names[] = {
#include "cfgnames.inc"
};

static const char *code_name(CFGMGR_CODE code, char *buf, size_t n)
{
	if ((unsigned)code < sizeof(code_names) / sizeof(code_names[0]) && code_names[code])
		return code_names[code];
	/* legacy fallback names, trailing newline included: never an existing file */
	if ((code & 0xffff) == CFGMGR_COUNTRY_STRUCT)
		snprintf(buf, n, "COUNTRY_STRUCT_%02x\n", (unsigned)code >> 16);
	else
		snprintf(buf, n, "code_%d\n", (int)code);
	return buf;
}

enum nvm_format { FMT_HEXBYTES, FMT_HEXSHORTS, FMT_HEXLONGS, FMT_STRING };

static enum nvm_format code_format(CFGMGR_CODE code)
{
	switch (code) {
	case CFGMGR_COUNTRY_CODE:
	case CFGMGR_PREVIOUS_COUNTRY_CODE:
	case CFGMGR_PCI_VENDOR_ID:
	case CFGMGR_PCI_DEVICE_ID:
		return FMT_HEXSHORTS;
	case CFGMGR_HARDWARE_PROFILE:
	case CFGMGR_LICENSE_OWNER:
	case CFGMGR_LICENSE_STATUS:
		return FMT_STRING;
	case CFGMGR_HARDWARE_ID:
	case CFGMGR_LICENSE_KEY:
		return FMT_HEXLONGS;
	default:
		return FMT_HEXBYTES;
	}
}

static bool is_dynamic(CFGMGR_CODE code)
{
	switch (code) {
	case CFGMGR_PROFILE_STORED:
	case CFGMGR_POUND_UD:
	case CFGMGR_BLACK_LIST:
	case CFGMGR_COUNTRY_CODE:
	case CFGMGR_PREVIOUS_COUNTRY_CODE:
	case CFGMGR_VRID_FORMAT:
	case CFGMGR_VRID_CRC:
	case CFGMGR_VRID_TYPE:
	case CFGMGR_VRID_LENGTH:
	case CFGMGR_VRID_DATA:
#if !defined NO_QC_SUPPORT
	case CFGMGR_QC_PROFILE:
#endif
	case CFGMGR_PCI_VENDOR_ID:
	case CFGMGR_PCI_DEVICE_ID:
	case CFGMGR_HARDWARE_PROFILE:
	case CFGMGR_HARDWARE_ID:
	case CFGMGR_LICENSE_OWNER:
	case CFGMGR_LICENSE_KEY:
	case CFGMGR_LICENSE_STATUS:
		return true;
	default:
		return false;
	}
}

static const UINT16 default_country = 0xB5;	/* USA */

static const PROFILE_DATA factory_profile = {	/* standard Hayes defaults */
	.Echo = 1, .Volume = 1, .Speaker = 1, .Pulse = 0, .Quiet = 0, .Verbose = 1,
	.Level = 3, .Connect = 0, .AmperC = 1, .AmperD = 2,
	.S0 = 0, .S1 = 0, .S2 = 43, .S3 = 13, .S4 = 10, .S5 = 8, .S6 = 2, .S7 = 50,
	.S8 = 2, .S10 = 14, .S11 = 95, .S12 = 50, .S16 = 0, .S18 = 0, .S29 = 70,
};

/* ---- file format --------------------------------------------------------- */

static bool all_hex(const char *p, int n)
{
	for (int i = 0; i < n; i++)
		if (!isxdigit((unsigned char)p[i]))
			return false;
	return true;
}

/*
 * Parse a parameter file into buf (capacity *size). On return *size holds the
 * number of bytes produced and the rest of buf is zeroed, also when the file
 * cannot be opened. Hex tokens yield one byte each (longer tokens keep their
 * low byte); the character after a token is consumed unexamined. A file that
 * is exactly one 4- or 8-digit line read into a 2- or 4-byte buffer is
 * stored as a native integer instead.
 */
static bool read_param_file(const char *path, void *buf, UINT32 *size)
{
	UINT32 cap = *size, left = cap;
	unsigned char *dp = buf;
	char *data = NULL;
	size_t n = 0;
	bool in_string = false;
	FILE *f = fopen(path, "rb");

	if (f) {
		size_t alloc = 0, got;

		do {
			if (n + 256 > alloc) {
				char *nd = realloc(data, alloc += 4096);

				if (!nd)
					break;
				data = nd;
			}
			got = fread(data + n, 1, alloc - n, f);
			n += got;
		} while (got > 0);
		fclose(f);
	}

	for (size_t i = 0; f && i < n && left > 0; i++) {
		char ch = data[i];
		char *end;

		if (ch == '"') {
			in_string = !in_string;
			continue;
		}
		if (in_string) {
			*dp++ = (unsigned char)ch;
			left--;
			continue;
		}
		if (!isxdigit((unsigned char)ch))
			continue;
		if (i == 0 && left == cap && cap == 2 && n == 5 && all_hex(data, 4) && data[4] == '\n') {
			UINT16 v = (UINT16)strtoul(data, &end, 16);

			memcpy(dp, &v, 2);
			dp += 2;
			left -= 2;
			i = (size_t)(end - data);
			continue;
		}
		if (i == 0 && left == cap && cap == 4 && n == 9 && all_hex(data, 8) && data[8] == '\n') {
			UINT32 v = (UINT32)strtoul(data, &end, 16);

			memcpy(dp, &v, 4);
			dp += 4;
			left -= 4;
			i = (size_t)(end - data);
			continue;
		}
		{
			char tmp[32];
			size_t j = i, k = 0;

			while (j < n && isxdigit((unsigned char)data[j]) && k < sizeof(tmp) - 1)
				tmp[k++] = data[j++];
			tmp[k] = '\0';
			*dp++ = (unsigned char)strtoul(tmp, NULL, 16);
			left--;
			i = j;	/* loop increment skips the delimiter */
		}
	}
	free(data);
	*size = cap - left;
	memset(dp, 0, left);
	return f != NULL;
}

static bool write_param_file(const char *path, const void *buf, UINT32 size, enum nvm_format fmt)
{
	const unsigned char *p = buf;
	char tmppath[PATH_MAX + 8];
	FILE *f;
	bool ok = true;
	unsigned unit = fmt == FMT_HEXSHORTS ? 2 : fmt == FMT_HEXLONGS ? 4 : 1;

	if (fmt != FMT_STRING && (size == 0 || size % unit)) {
		hsf_problem("NVM write %s: size %u invalid for format %d", path, size, fmt);
		return false;
	}
	if (!make_path(tmppath, sizeof(tmppath), "%s.tmp", path))
		return false;
	f = fopen(tmppath, "w");
	if (!f) {
		hsf_problem("NVM write %s: %s", tmppath, strerror(errno));
		return false;
	}
	fchmod(fileno(f), 0600);
	if (fmt == FMT_STRING) {
		UINT32 len = size < MAX_OEM_STR_LEN ? size : MAX_OEM_STR_LEN;

		fprintf(f, "\"%.*s\"\n", (int)strnlen((const char *)buf, len), (const char *)buf);
	} else {
		for (UINT32 off = unit; off <= size; off += unit, p += unit) {
			unsigned v;

			if (unit == 1) {
				v = *p;
			} else if (unit == 2) {
				UINT16 s;
				memcpy(&s, p, 2);
				v = s;
			} else {
				UINT32 l;
				memcpy(&l, p, 4);
				v = l;
			}
			if (off == size)
				fprintf(f, "%0*X\n", (int)unit * 2, v);
			else
				fprintf(f, "%0*X,%s", (int)unit * 2, v, off % 16 ? "" : "\n");
		}
	}
	if (fclose(f) != 0)
		ok = false;
	if (ok && rename(tmppath, path) != 0)
		ok = false;
	if (!ok)
		hsf_problem("NVM write %s failed: %s", path, strerror(errno));
	return ok;
}

/* ---- lookups ------------------------------------------------------------------ */


static bool read_static(POS_DEVNODE dev, const char *name, void *buf, UINT32 *size)
{
	char path[PATH_MAX];

	if (!make_path(path, sizeof(path), "%s/%s/%s", static_dir, dev->hwProfile, name)) {
		memset(buf, 0, *size);
		*size = 0;
		return false;
	}
	return read_param_file(path, buf, size);
}

static bool dynamic_path(POS_DEVNODE dev, const char *name, char *path, size_t n)
{
	return make_path(path, n, "%s/%d-%s/%s", dynamic_dir, dev->hwInstNum, dev->hwInstName, name);
}

static bool read_dynamic(POS_DEVNODE dev, const char *name, void *buf, UINT32 *size)
{
	char path[PATH_MAX];

	if (!dynamic_path(dev, name, path, sizeof(path))) {
		memset(buf, 0, *size);
		*size = 0;
		return false;
	}
	return read_param_file(path, buf, size);
}

static bool read_country_param(POS_DEVNODE dev, int t35, UINT8 reference, const char *suffix,
			       void *buf, UINT32 size)
{
	char path[PATH_MAX];
	UINT32 sz = size;

	if (make_path(path, sizeof(path), "%s/%s/Region/%04X%s", static_dir, dev->hwProfile, t35, suffix) &&
	    read_param_file(path, buf, &sz))
		return true;
	if (reference == 0xFF)
		return false;
	sz = size;
	if (!make_path(path, sizeof(path), "%s/%s/Profile/%04X%s", static_dir, dev->hwProfile, reference, suffix))
		return false;
	return read_param_file(path, buf, &sz);
}

#define COUNTRY(field, suffix) read_country_param(dev, t35, ref, (suffix), &c->field, sizeof(c->field))

static void read_country(POS_DEVNODE dev, BYTE daa, int t35, CtryPrmsStruct *c)
{
	bool silabs = daa == SI3054_DAA || daa == SI3055_DAA;
	UINT8 ref = 0xFF;

	memset(c, 0, sizeof(*c));
	c->T35Code = (UINT16)t35;
	/* A missing REFERENCE file zeroes ref, so later lookups fall back to
	 * Profile/0000 - preserved from the legacy driver. */
	read_country_param(dev, t35, 0xFF, "/REFERENCE", &ref, sizeof(ref));
	COUNTRY(cInter, "_NAME");
	COUNTRY(cIntCode, "/INTCODE");
	COUNTRY(Txlevel, (silabs || daa == CAESAR) ? "/SMART_TXLEVEL" : "/TXLEVEL");
	if (silabs) {
		/* The legacy driver tested Relays[1] before reading the relays, so
		 * this always took the first branch; keep that behaviour. */
		int low = c->Txlevel.LowDialLevel, high = c->Txlevel.HighDialLevel;

		if (c->Relays[1] < 0x04000) {
			c->Txlevel.LowDialLevel = low - 9;
			c->Txlevel.HighDialLevel = high - 9;
		} else {
			c->Txlevel.LowDialLevel = low - 4;
			c->Txlevel.HighDialLevel = high + 10;
		}
	}
	COUNTRY(Relays, silabs ? "/SILABRELAYS" : daa == CAESAR ? "/SMART_RELAYS" : "/RELAYS");
	COUNTRY(Pulse, "/PULSE");
	COUNTRY(Ring, "/RING");
	COUNTRY(SRegLimits, "/SREG");
	COUNTRY(DTMF, daa == CAESAR ? "/SMART_DTMF" : "/DTMF");
	COUNTRY(Filter, "/FILTER");
	COUNTRY(Threshold, daa == CAESAR ? "/SMART_THRESHOLD" : "/THRESHOLD");
	COUNTRY(RLSD, "/RLSD");
	COUNTRY(Tone, "/TONE");
	COUNTRY(Timing, "/TIMING");
	COUNTRY(Cadence, "/CADENCE");
	COUNTRY(Blacklisting, "/BLACKLISTING");
	COUNTRY(CallerID, "/CALLERID");
	COUNTRY(CallerID2, "/CALLERID2");
	COUNTRY(Flags, "/FLAGS");
	COUNTRY(AgressSpeedIndex, "/SPEEDADJUST");
	COUNTRY(CallWaitingParms, "/CALL_WAITING");
	COUNTRY(V92Control, "/V92_CONTROL");
	COUNTRY(OgcParams, "/V92_OGC");
}

/* ---- Os interface ------------------------------------------------------------- */

HSF_EXPORT HANDLE NVM_Open(ULONG_PTR dwDevNode)
{
	POS_DEVNODE dev = (POS_DEVNODE)dwDevNode;
	char path[PATH_MAX], profile[sizeof(dev->hwProfile)];
	UINT32 size = sizeof(profile);

	if (!make_path(path, sizeof(path), "%s/%d-%s", dynamic_dir, dev->hwInstNum, dev->hwInstName))
		return NULL;
	if (mkdir(path, 0700) != 0 && errno != EEXIST)
		hsf_problem("NVM_Open: mkdir %s: %s", path, strerror(errno));
	if (read_dynamic(dev, "HARDWARE_PROFILE", profile, &size)) {
		profile[sizeof(profile) - 1] = '\0';
		snprintf(dev->hwProfile, sizeof(dev->hwProfile), "%s", profile);
	}
	HSF_TRACE("NVM_Open", "%p) = (instance %s, profile %s", (void *)dev, path, dev->hwProfile);
	return dev;
}

HSF_EXPORT COM_STATUS NVM_Close(HANDLE hNVM)
{
	HSF_TRACE("NVM_Close", "%p", hNVM);
	return COM_STATUS_SUCCESS;
}

HSF_EXPORT COM_STATUS NVM_Read(HANDLE hNVM, CFGMGR_CODE eCode, PVOID pBuf, PUINT32 pdwSize)
{
	POS_DEVNODE dev = hNVM;
	char nbuf[48];
	const char *name = code_name(eCode, nbuf, sizeof(nbuf));
	UINT32 want;
	bool ok;

	if (eCode == CFGMGR_COUNTRY_STRUCT) {
		/* the T.35 country code arrives in place of the size pointer */
		int t35 = (int)(uintptr_t)pdwSize;
		BYTE daa = SIMPLE_DAA;
		UINT32 sz = sizeof(daa);

		NVM_Read(hNVM, CFGMGR_OEM_DAATYPE, &daa, &sz);
		read_country(dev, daa, t35, pBuf);
		HSF_TRACE("NVM_Read", "COUNTRY_STRUCT t35=%04X daa=%u", t35, daa);
		return COM_STATUS_SUCCESS;
	}

	want = *pdwSize;
	if (is_dynamic(eCode)) {
		ok = read_dynamic(dev, name, pBuf, pdwSize);
		if (!ok && eCode == CFGMGR_COUNTRY_CODE && want == sizeof(default_country)) {
			memcpy(pBuf, &default_country, sizeof(default_country));
			*pdwSize = sizeof(default_country);
			ok = true;
		} else if (!ok && eCode == CFGMGR_PROFILE_STORED && want == sizeof(factory_profile)) {
			memcpy(pBuf, &factory_profile, sizeof(factory_profile));
			*pdwSize = sizeof(factory_profile);
			ok = true;
		}
	} else {
		ok = read_static(dev, name, pBuf, pdwSize);
		if (!ok && eCode == CFGMGR_PROFILE_FACTORY && want == sizeof(factory_profile)) {
			memcpy(pBuf, &factory_profile, sizeof(factory_profile));
			*pdwSize = sizeof(factory_profile);
			ok = true;
		}
	}
	HSF_TRACE("NVM_Read", "%s, %u) = (%s, %u", name, want, ok ? "ok" : "missing", *pdwSize);
	return ok ? COM_STATUS_SUCCESS : COM_STATUS_VALUE_NOT_FOUND;
}

HSF_EXPORT COM_STATUS NVM_Write(HANDLE hNVM, CFGMGR_CODE eCode, PVOID pBuf, PUINT32 pdwSize)
{
	POS_DEVNODE dev = hNVM;
	char nbuf[48], path[PATH_MAX];
	const char *name = code_name(eCode, nbuf, sizeof(nbuf));

	if (dev->hwSuspendInProgress)
		return COM_STATUS_FAIL;
	if (!is_dynamic(eCode))
		hsf_log(HSF_LOG_INFO, "NVM_Write: saving static parameter %s (%u bytes)", name, *pdwSize);
	if (!dynamic_path(dev, name, path, sizeof(path)))
		return COM_STATUS_FAIL;
	HSF_TRACE("NVM_Write", "%s, %u bytes", name, *pdwSize);
	return write_param_file(path, pBuf, *pdwSize, code_format(eCode)) ? COM_STATUS_SUCCESS : COM_STATUS_FAIL;
}

HSF_EXPORT void NVM_WriteFlushList(BOOL write)
{
	(void)write;	/* writes are synchronous here */
}
