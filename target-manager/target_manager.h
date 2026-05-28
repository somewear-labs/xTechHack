#ifndef TARGET_MANAGER_H
#define TARGET_MANAGER_H

#include <stdint.h>
#include <nvdsmeta.h>
#include <gst/gst.h>          /* GstBuffer */
#ifdef __cplusplus
extern "C" {
#endif

int  tm_init(const char *beam_url, const char *inbound_socket_path);
void tm_on_batch(NvDsBatchMeta *batch_meta);

/* New: same as tm_on_batch but also passes the GstBuffer carrying this batch.
 * Lets target-manager reach the underlying NvBufSurface so it can extract
 * bbox crops for the on-board VLM (vessel-class + size prior). The buffer
 * pointer is borrowed for the duration of the call only — do not hold across
 * frames. Pass NULL to skip crop extraction (equivalent to tm_on_batch). */
void tm_on_batch_with_buffer(GstBuffer *buf, NvDsBatchMeta *batch_meta);

/* Re-apply per-object bbox colors based on TargetManager state.
 * Stock deepstream-app's process_meta() rewrites rect_params.border_color from
 * [primary-gie] bbox-border-colorN every frame, clobbering anything tm_on_batch
 * sets. Call tm_apply_colors AFTER process_meta() so our writes win. */
void tm_apply_colors(NvDsBatchMeta *batch_meta);

void tm_shutdown(void);

/* Return the canonical unified target ID for a given (source pad, tracker object).
 * Used by the deepstream_app patch to stamp consistent IDs into the per-frame JSON
 * so the browser overlay shows the same ID for the same person across both cameras.
 * Returns 0 if the track hasn't been processed by on_batch yet (use local packed_id
 * as fallback in that case). */
uint64_t tm_get_canonical_id(uint32_t src_id, int class_id, uint64_t obj_id);

#ifdef __cplusplus
}
#endif

#endif
