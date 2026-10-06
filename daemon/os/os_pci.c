/* SPDX-License-Identifier: MIT */
/*
 * PCI configuration space through the OsPciReadConfig and OsPciWriteConfig
 * functions.
 *
 * The blobs pass an opaque device handle. Backends register a snapshot of
 * the matching device's 256-byte config space under that handle (for the
 * HDA controller: read once from sysfs). Reads are served from the snapshot.
 * Writes are logged and dropped: snd-hda-intel owns the controller.
 */
#include "hsf_os.h"

#include <string.h>

#define MAX_DEVICES 4

static struct {
	void *handle;
	uint8_t config[256];
} devices[MAX_DEVICES];
static pthread_mutex_t pci_lock = PTHREAD_MUTEX_INITIALIZER;

void hsf_pci_register(void *handle, const uint8_t config[256])
{
	pthread_mutex_lock(&pci_lock);
	for (int i = 0; i < MAX_DEVICES; i++) {
		if (!devices[i].handle || devices[i].handle == handle) {
			devices[i].handle = handle;
			memcpy(devices[i].config, config, 256);
			break;
		}
	}
	pthread_mutex_unlock(&pci_lock);
}

void hsf_pci_unregister(void *handle)
{
	pthread_mutex_lock(&pci_lock);
	for (int i = 0; i < MAX_DEVICES; i++)
		if (devices[i].handle == handle)
			devices[i].handle = NULL;
	pthread_mutex_unlock(&pci_lock);
}

bool hsf_pci_lookup(void *handle, uint8_t config[256])
{
	bool found = false;

	pthread_mutex_lock(&pci_lock);
	for (int i = 0; i < MAX_DEVICES; i++) {
		if (devices[i].handle && devices[i].handle == handle) {
			memcpy(config, devices[i].config, 256);
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&pci_lock);
	return found;
}

static void read_config(void *dev, unsigned offset, void *val, unsigned len, const char *who)
{
	bool found = false;

	memset(val, 0, len);
	pthread_mutex_lock(&pci_lock);
	for (int i = 0; i < MAX_DEVICES; i++) {
		if (devices[i].handle && devices[i].handle == dev) {
			if (offset + len <= 256)
				memcpy(val, &devices[i].config[offset], len);
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&pci_lock);
	if (!found)
		hsf_problem("%s(%p, 0x%02x): unknown device handle", who, dev, offset);
}

HSF_EXPORT void OsPciReadConfigb(void *pPciDev, unsigned char Offset, unsigned char *pVal)
{
	read_config(pPciDev, Offset, pVal, 1, "OsPciReadConfigb");
	HSF_TRACE("OsPciReadConfigb", "%p, 0x%02x) = (0x%02x", pPciDev, Offset, *pVal);
}

HSF_EXPORT void OsPciReadConfigw(void *pPciDev, unsigned char Offset, unsigned short *pVal)
{
	read_config(pPciDev, Offset, pVal, 2, "OsPciReadConfigw");
	HSF_TRACE("OsPciReadConfigw", "%p, 0x%02x) = (0x%04x", pPciDev, Offset, *pVal);
}

HSF_EXPORT void OsPciReadConfigdw(void *pPciDev, unsigned char Offset, unsigned int *pVal)
{
	read_config(pPciDev, Offset, pVal, 4, "OsPciReadConfigdw");
	HSF_TRACE("OsPciReadConfigdw", "%p, 0x%02x) = (0x%08x", pPciDev, Offset, *pVal);
}

HSF_EXPORT void OsPciWriteConfigb(void *pPciDev, unsigned char Offset, unsigned char Val)
{
	HSF_COUNT("OsPciWriteConfigb");
	hsf_log(HSF_LOG_INFO, "OsPciWriteConfigb(%p, 0x%02x, 0x%02x) ignored", pPciDev, Offset, Val);
}

HSF_EXPORT void OsPciWriteConfigw(void *pPciDev, unsigned char Offset, unsigned short Val)
{
	HSF_COUNT("OsPciWriteConfigw");
	hsf_log(HSF_LOG_INFO, "OsPciWriteConfigw(%p, 0x%02x, 0x%04x) ignored", pPciDev, Offset, Val);
}

HSF_EXPORT void OsPciWriteConfigdw(void *pPciDev, unsigned char Offset, unsigned int Val)
{
	HSF_COUNT("OsPciWriteConfigdw");
	hsf_log(HSF_LOG_INFO, "OsPciWriteConfigdw(%p, 0x%02x, 0x%08x) ignored", pPciDev, Offset, Val);
}
