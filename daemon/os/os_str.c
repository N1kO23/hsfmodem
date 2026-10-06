/* SPDX-License-Identifier: MIT */
/*
 * String and memory helpers the blobs import. They behave like their libc
 * counterparts, with two legacy quirks kept: OsAtoi parses hexadecimal, and
 * OsSprintf writes without a size limit.
 */
#include "hsf_os.h"

#include <string.h>

HSF_EXPORT PVOID OsMemSet(PVOID pBuf, UINT8 c, UINT32 Count)
{
	HSF_TRACE_HOT("OsMemSet", "%p, %u, %u", pBuf, c, Count);
	return memset(pBuf, c, Count);
}

HSF_EXPORT PVOID OsMemCpy(PVOID pDest, PVOID pSrc, UINT32 Count)
{
	HSF_TRACE_HOT("OsMemCpy", "%p, %p, %u", pDest, pSrc, Count);
	return memcpy(pDest, pSrc, Count);
}

HSF_EXPORT PVOID OsMemMove(PVOID pDest, PVOID pSrc, UINT32 Count)
{
	HSF_TRACE_HOT("OsMemMove", "%p, %p, %u", pDest, pSrc, Count);
	return memmove(pDest, pSrc, Count);
}

HSF_EXPORT int OsMemCmp(PVOID pBuff1, PVOID pBuff2, UINT32 Count)
{
	HSF_TRACE_HOT("OsMemCmp", "%p, %p, %u", pBuff1, pBuff2, Count);
	return memcmp(pBuff1, pBuff2, Count);
}

HSF_EXPORT PVOID OsStrCpy(LPSTR szDest, LPCSTR szSrc)
{
	HSF_TRACE_HOT("OsStrCpy", "%p", (void *)szDest);
	return strcpy(szDest, szSrc);
}

HSF_EXPORT PVOID OsStrnCpy(LPSTR szDest, LPCSTR szSrc, int MaxSize)
{
	HSF_TRACE_HOT("OsStrnCpy", "%p, %d", (void *)szDest, MaxSize);
	return strncpy(szDest, szSrc, (size_t)MaxSize);
}

HSF_EXPORT LPSTR OsStrCat(LPSTR szDest, LPCSTR szSrc)
{
	HSF_TRACE_HOT("OsStrCat", "%p", (void *)szDest);
	return strcat(szDest, szSrc);
}

HSF_EXPORT LPSTR OsStrnCat(LPSTR szDest, LPCSTR szSrc, int MaxSize)
{
	HSF_TRACE_HOT("OsStrnCat", "%p, %d", (void *)szDest, MaxSize);
	return strncat(szDest, szSrc, (size_t)MaxSize);
}

HSF_EXPORT int OsStrCmp(LPCSTR szStr1, LPCSTR szStr2)
{
	HSF_TRACE_HOT("OsStrCmp", "\"%s\", \"%s\"", szStr1, szStr2);
	return strcmp(szStr1, szStr2);
}

HSF_EXPORT int OsStrnCmp(LPCSTR szStr1, LPCSTR szStr2, UINT32 Count)
{
	HSF_TRACE_HOT("OsStrnCmp", "%u", Count);
	return strncmp(szStr1, szStr2, Count);
}

HSF_EXPORT int OsStrLen(LPCSTR szStr)
{
	HSF_TRACE_HOT("OsStrLen", "%p", (const void *)szStr);
	return (int)strlen(szStr);
}

HSF_EXPORT int OsToupper(int c)
{
	HSF_TRACE_HOT("OsToupper", "%d", c);
	return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

HSF_EXPORT int OsTolower(int c)
{
	HSF_TRACE_HOT("OsTolower", "%d", c);
	return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

HSF_EXPORT int OsIsDigit(int c)
{
	HSF_TRACE_HOT("OsIsDigit", "%d", c);
	return c >= '0' && c <= '9';
}

HSF_EXPORT int OsAtoi(LPCSTR szStr)
{
	/* Hexadecimal despite the name: every character shifts in 4 bits, and
	 * characters that are not hex digits contribute 0. */
	int v = 0;

	HSF_TRACE_HOT("OsAtoi", "\"%s\"", szStr);
	for (; *szStr; szStr++) {
		v = (int)((unsigned)v << 4);
		if (*szStr >= '0' && *szStr <= '9')
			v += *szStr - '0';
		else if (*szStr >= 'a' && *szStr <= 'f')
			v += *szStr - 'a' + 10;
		else if (*szStr >= 'A' && *szStr <= 'F')
			v += *szStr - 'A' + 10;
	}
	return v;
}

HSF_EXPORT int OsVSprintf(LPSTR buffer, LPCSTR format, va_list ap)
{
	HSF_TRACE_HOT("OsVSprintf", "\"%s\"", format);
	return vsprintf(buffer, format, ap);
}

HSF_EXPORT int OsSprintf(LPSTR buffer, LPCSTR format, ...)
{
	va_list ap;
	int n;

	HSF_TRACE_HOT("OsSprintf", "\"%s\"", format);
	va_start(ap, format);
	n = vsprintf(buffer, format, ap);
	va_end(ap);
	return n;
}
