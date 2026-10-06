/* SPDX-License-Identifier: MIT */
/*
 * NVM store: on-disk format, reader quirks, defaults and country lookups,
 * against the legacy rules in docs/OS-LAYER.md and the generated static tree.
 */
#include "hsf_os.h"
#include "../tap.h"

#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

static OS_DEVNODE dev;
static char dyn[] = "/tmp/hsf-nvm-XXXXXX";
static char inst[4200];

static void quiet_sink(int level, const char *line)
{
	(void)level;
	(void)line;
}

static void slurp(const char *name, char *buf, size_t n)
{
	char path[4400];
	FILE *f;
	size_t got = 0;

	snprintf(path, sizeof(path), "%s/%s", inst, name);
	buf[0] = '\0';
	f = fopen(path, "r");
	if (f) {
		got = fread(buf, 1, n - 1, f);
		fclose(f);
	}
	buf[got] = '\0';
}

static void put(const char *name, const char *content)
{
	char path[4400];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", inst, name);
	f = fopen(path, "w");
	fputs(content, f);
	fclose(f);
}

static void test_open(void)
{
	struct stat st;

	ok(NVM_Open((ULONG_PTR)&dev) == &dev, "NVM_Open returns the device node as handle");
	ok(stat(inst, &st) == 0 && S_ISDIR(st.st_mode), "the per-device directory exists");
	is_int(st.st_mode & 0777, 0700, "and is private (0700)");
}

static void test_write_formats(void)
{
	char buf[512];
	UINT16 country = 0x003C;
	UINT32 hwid = 0x21CC3BB2, size;
	const char owner[] = "me@example.org";
	PROFILE_DATA prof;

	size = sizeof(country);
	is_int(NVM_Write(&dev, CFGMGR_COUNTRY_CODE, &country, &size), COM_STATUS_SUCCESS, "write COUNTRY_CODE");
	slurp("COUNTRY_CODE", buf, sizeof(buf));
	is_str(buf, "003C\n", "16-bit codes are written as one %%04X value");

	size = sizeof(hwid);
	NVM_Write(&dev, CFGMGR_HARDWARE_ID, &hwid, &size);
	slurp("HARDWARE_ID", buf, sizeof(buf));
	is_str(buf, "21CC3BB2\n", "32-bit codes are written as %%08X");

	size = sizeof(owner) - 1;
	NVM_Write(&dev, CFGMGR_LICENSE_OWNER, (PVOID)owner, &size);
	slurp("LICENSE_OWNER", buf, sizeof(buf));
	is_str(buf, "\"me@example.org\"\n", "string codes are quoted");

	/* the Hayes factory profile, as HEXBYTES with a line break after 16 */
	memset(&prof, 0, sizeof(prof));
	size = sizeof(prof);
	is_int(NVM_Read(&dev, CFGMGR_PROFILE_STORED, &prof, &size), COM_STATUS_SUCCESS,
	       "a missing stored profile reads as the factory profile");
	is_int(size, sizeof(PROFILE_DATA), "of full size");
	ok(prof.Echo == 1 && prof.Level == 3 && prof.AmperD == 2 && prof.S2 == 43 && prof.S29 == 70,
	   "with the Hayes defaults (E1 X3 &D2 S2=43 S29=70)");
	NVM_Write(&dev, CFGMGR_PROFILE_STORED, &prof, &size);
	slurp("PROFILE_STORED", buf, sizeof(buf));
	is_str(buf, "01,01,01,00,00,01,03,00,01,02,00,00,2B,0D,0A,08,\n02,32,02,0E,5F,32,00,00,46\n",
	       "byte codes are %%02X, comma separated, 16 per line");

	size = 3;
	is_int(NVM_Write(&dev, CFGMGR_COUNTRY_CODE, &country, &size), COM_STATUS_FAIL,
	       "an odd size is refused for a 16-bit code");
}

static void test_round_trips(void)
{
	UINT16 country = 0;
	UINT32 hwid = 0, size;
	char owner[MAX_OEM_STR_LEN];
	PROFILE_DATA prof;

	size = sizeof(country);
	is_int(NVM_Read(&dev, CFGMGR_COUNTRY_CODE, &country, &size), COM_STATUS_SUCCESS, "read COUNTRY_CODE");
	ok(country == 0x003C && size == 2, "COUNTRY_CODE round-trips as a native integer");
	size = sizeof(hwid);
	NVM_Read(&dev, CFGMGR_HARDWARE_ID, &hwid, &size);
	ok(hwid == 0x21CC3BB2 && size == 4, "HARDWARE_ID round-trips as a native integer");
	memset(owner, 'x', sizeof(owner));
	size = sizeof(owner);
	NVM_Read(&dev, CFGMGR_LICENSE_OWNER, owner, &size);
	ok(size == 14 && !memcmp(owner, "me@example.org", 14) && owner[14] == 0,
	   "strings round-trip and the rest of the buffer is zeroed");
	size = sizeof(prof);
	NVM_Read(&dev, CFGMGR_PROFILE_STORED, &prof, &size);
	ok(size == sizeof(prof) && prof.S11 == 95, "byte arrays round-trip");
}

static void test_reader_quirks(void)
{
	unsigned char b[8];
	UINT32 size;

	put("PREVIOUS_COUNTRY_CODE", "0001,0002\n");
	memset(b, 0xEE, sizeof(b));
	size = 4;
	NVM_Read(&dev, CFGMGR_PREVIOUS_COUNTRY_CODE, b, &size);
	ok(size == 2 && b[0] == 0x01 && b[1] == 0x02 && b[2] == 0 && b[3] == 0,
	   "a multi-value file yields one byte per token (legacy quirk)");

	put("PREVIOUS_COUNTRY_CODE", "1234\n");
	size = 3;
	NVM_Read(&dev, CFGMGR_PREVIOUS_COUNTRY_CODE, b, &size);
	ok(size == 1 && b[0] == 0x34, "a long token keeps its low byte unless the special case applies");

	put("LICENSE_STATUS", "\"a,b c\" 0A\n");
	memset(b, 0, sizeof(b));
	size = 8;
	NVM_Read(&dev, CFGMGR_LICENSE_STATUS, b, &size);
	ok(size == 6 && !memcmp(b, "a,b c\x0a", 6), "quoted text is copied verbatim, then hex tokens follow");

	memset(b, 0xEE, sizeof(b));
	size = sizeof(b);
	is_int(NVM_Read(&dev, CFGMGR_BLACK_LIST, b, &size), COM_STATUS_VALUE_NOT_FOUND, "a missing file is not found");
	ok(size == 0 && b[0] == 0 && b[7] == 0, "and leaves a zeroed, empty buffer");

	UINT16 c = 0;
	char path[4400];
	snprintf(path, sizeof(path), "%s/COUNTRY_CODE", inst);
	unlink(path);
	size = sizeof(c);
	NVM_Read(&dev, CFGMGR_COUNTRY_CODE, &c, &size);
	is_int(c, 0xB5, "a missing COUNTRY_CODE defaults to USA (0xB5)");
}

static void test_static_and_country(void)
{
	CtryPrmsStruct ct;
	BYTE daa = 0xFF;
	UINT32 size = sizeof(daa);
	static const unsigned char fi_txlevel[] = { 0x0F, 0x08, 0x0B, 0x0F, 0x08, 0x0B, 0x00, 0x0F, 0x08, 0x00, 0x66, 0x4A };
	static const unsigned char fi_sreg[] = { 0x00, 0x02, 0x08, 0x00, 0x06, 0x03, 0x08, 0x03 };

	is_int(NVM_Read(&dev, CFGMGR_OEM_DAATYPE, &daa, &size), COM_STATUS_SUCCESS, "static parameters come from the hsfhda profile");
	is_int(daa, CAESAR, "the HDA profile uses the CAESAR (smart) DAA");

	memset(&ct, 0xEE, sizeof(ct));
	is_int(NVM_Read(&dev, CFGMGR_COUNTRY_STRUCT, &ct, (PUINT32)(uintptr_t)0x3C), COM_STATUS_SUCCESS,
	       "read the Finland country structure (T.35 code passed as the size argument)");
	is_int(ct.T35Code, 0x3C, "T35Code is set");
	is_str(ct.cInter, "FINLAND", "the name comes from Region/003C_NAME");
	is_str(ct.cIntCode, "00", "INTCODE falls back to Profile/0002 (REFERENCE = 02)");
	ok(!memcmp(&ct.Txlevel, fi_txlevel, sizeof(fi_txlevel)), "CAESAR reads SMART_TXLEVEL through the fallback");
	ok(!memcmp(ct.SRegLimits, fi_sreg, sizeof(fi_sreg)), "SREG limits come from the region itself");

	memset(&ct, 0, sizeof(ct));
	NVM_Read(&dev, CFGMGR_COUNTRY_STRUCT, &ct, (PUINT32)(uintptr_t)0xB5);
	is_str(ct.cInter, "USA", "USA name");
	is_str(ct.cIntCode, "011", "USA international prefix from Region/00B5/INTCODE");
}

static void test_alias_names(void)
{
	unsigned char v = 0x2A, r = 0;
	UINT32 size = 1;

	/* NRINGS_TO_ANSWER == SREG + 0: both name the SREG file */
	NVM_Write(&dev, CFGMGR_NRINGS_TO_ANSWER, &v, &size);
	char buf[64];
	slurp("SREG", buf, sizeof(buf));
	is_str(buf, "2A\n", "an aliased code uses the first enumerator's name");
	size = 1;
	NVM_Read(&dev, CFGMGR_NRINGS_TO_ANSWER, &r, &size);
	ok(size == 1 && r != 0x2A, "a static code is still read from the static profile after a write (legacy)");
}

int main(void)
{
	struct hsf_os_config cfg = HSF_OS_CONFIG_DEFAULT;
	const char *nvm_static = getenv("HSF_TEST_NVM_STATIC");

	if (!nvm_static || !mkdtemp(dyn)) {
		printf("Bail out! need HSF_TEST_NVM_STATIC and a temporary directory\n");
		return 1;
	}
	cfg.bios_source = "zeros";
	hsf_log_set_sink(quiet_sink);
	hsf_os_init(&cfg);
	hsf_nvm_set_dirs(nvm_static, dyn);
	snprintf(dev.hwInstName, sizeof(dev.hwInstName), "HDA-test");
	snprintf(dev.hwProfile, sizeof(dev.hwProfile), "hsfhda");
	snprintf(inst, sizeof(inst), "%s/0-HDA-test", dyn);

	test_open();
	test_write_formats();
	test_round_trips();
	test_reader_quirks();
	test_static_and_country();
	test_alias_names();
	NVM_Close(&dev);
	hsf_os_shutdown();
	return tap_done();
}
