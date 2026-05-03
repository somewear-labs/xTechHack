#ifndef TARGET_MANAGER_H
#define TARGET_MANAGER_H

#include <nvdsmeta.h>
#ifdef __cplusplus
extern "C" {
#endif

int  tm_init(const char *beam_url, const char *inbound_socket_path);
void tm_on_batch(NvDsBatchMeta *batch_meta);

/* Re-apply per-object bbox colors based on TargetManager state.
 * Stock deepstream-app's process_meta() rewrites rect_params.border_color from
 * [primary-gie] bbox-border-colorN every frame, clobbering anything tm_on_batch
 * sets. Call tm_apply_colors AFTER process_meta() so our writes win. */
void tm_apply_colors(NvDsBatchMeta *batch_meta);

void tm_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
