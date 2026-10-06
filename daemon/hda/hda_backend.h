/* SPDX-License-Identifier: MIT */
/*
 * HD-audio backends behind the OsHdaCodec* interface of the hsfhda blob.
 *
 *   alsa    the real modem: snd-hda-codec-hsfmodem's hwdep node + modem PCM
 *   fake    a scripted stand-in for tests and development
 *   record  wraps another backend and writes every interaction to a file
 *   replay  answers from such a recording
 *
 * The blob calls the OsHdaCodec* function pointers; hda_glue.c routes those
 * to the active backend.
 */
#ifndef HSF_HDA_BACKEND_H
#define HSF_HDA_BACKEND_H

#include "hsf_os.h"

struct hsf_hda_backend;

struct hsf_hda_backend_ops {
	unsigned int (*read)(struct hsf_hda_backend *b, unsigned int nid, int direct,
			     unsigned int verb, unsigned int param);
	unsigned int (*wallclock)(struct hsf_hda_backend *b);
	int (*open_dma)(struct hsf_hda_backend *b, int bytes, void **play, void **cap);
	void (*close_dma)(struct hsf_hda_backend *b, void *play, void *cap);
	void (*dma_info)(struct hsf_hda_backend *b, void *stream, unsigned char *tag,
			 unsigned long *fifo, unsigned short **buf);
	int (*set_dma_state)(struct hsf_hda_backend *b, OSHDA_STREAM_STATE st, void *play, void *cap);
	unsigned long (*get_dma_pos)(struct hsf_hda_backend *b, void *stream);
	/* Start/stop delivering unsolicited responses (hsf_hda_deliver_unsol). */
	int (*start_events)(struct hsf_hda_backend *b);
	void (*stop_events)(struct hsf_hda_backend *b);
	void (*destroy)(struct hsf_hda_backend *b);
};

struct hsf_hda_backend {
	const struct hsf_hda_backend_ops *ops;
	const char *name;
	uint32_t vendor_id, subsystem_id, revision_id;
	unsigned int codec_addr, mfg_nid;
	/* opaque handle under which the controller's PCI config space is
	 * registered (hsf_pci_register); handed to the blob as hwDevLink */
	void *pci_handle;
};

/* ---- glue (hda_glue.c) ------------------------------------------------ */

/* Route the blob's OsHdaCodec* pointers to this backend. */
void hsf_hda_bind(struct hsf_hda_backend *b);
void hsf_hda_unbind(void);
HDAOSHAL *hsf_hda_hal(void);
/* Called by backends for every unsolicited response; forwarded to the blob
 * if its tag matches the registered callback. */
void hsf_hda_deliver_unsol(uint32_t res);
unsigned long hsf_hda_verb_count(void);
/* See every unsolicited response before it is filtered (recording). */
void hsf_hda_set_unsol_observer(void (*fn)(void *ctx, uint32_t res), void *ctx);

/* ---- constructors --------------------------------------------------------- */

struct hsf_hda_backend *hsf_hda_fake_new(void);
/* Open the snd-hda-codec-hsfmodem device; card < 0 searches all cards. */
struct hsf_hda_backend *hsf_hda_alsa_new(int card);
/* Power-management events from the driver: return non-zero from the hook to
 * veto a suspend (HSFMODEM_EVENT_SUSPEND); RESUME follows a completed one. */
void hsf_hda_alsa_set_pm_hook(struct hsf_hda_backend *b, int (*hook)(void *ctx, unsigned int event),
			      void *ctx);
struct hsf_hda_backend *hsf_hda_record_new(struct hsf_hda_backend *inner, const char *path,
					   bool positions);
struct hsf_hda_backend *hsf_hda_replay_new(const char *path);
/* Divergences between the blob's verbs and the recording, so far. */
unsigned long hsf_hda_replay_divergences(struct hsf_hda_backend *b);

#endif /* HSF_HDA_BACKEND_H */
