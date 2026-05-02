#include "target_manager.h"
#include "geolocation.h"
#include "curl/curl.h"
#include "target_proto.pb.h"


typedef struct {
    
} target_map;

typedef struct {
    char *beam_url;
    char *inbound_socket_path;
    CURL *curl;


} tgt_mgr_obj;

static tgt_mgr_obj tgt_mgr;

int tm_init(const char *beam_url, const char *inbound_socket_path) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    tgt_mgr.curl = curl_easy_init();
    memcpy(tgt_mgr.beam_url, beam_url, strlen(beam_url));
    memcpy(tgt_mgr.inbound_socket_path, inbound_socket_path, strlen(inbound_socket_path));
    (void)inbound_socket_path;
    return 0;
}

void tm_on_batch(NvDsBatchMeta *batch_meta) {
    (void)batch_meta;
}

void tm_shutdown(void) {

}
