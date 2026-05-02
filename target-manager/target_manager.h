#ifndef TARGET_MANAGER_H
#define TARGET_MANAGER_H

#include <nvdsmeta.h>
#ifdef __cplusplus
extern "C" {
#endif

int  tm_init(const char *beam_url, const char *inbound_socket_path);
void tm_on_batch(NvDsBatchMeta *batch_meta);
void tm_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
