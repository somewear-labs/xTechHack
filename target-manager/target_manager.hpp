#ifndef TARGET_MANAGER_HPP
#define TARGET_MANAGER_HPP

#include "geolocation.hpp"

#include <nvdsmeta.h>

#include <curl/curl.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>



struct TargetId {
    int           class_id;
    std::uint64_t object_id;
    bool operator==(const TargetId&) const = default;
};

struct Target {
    std::uint64_t id;

    // Latest projected location (lat/lng) — overwritten each on_batch.
    double  latitude       = 0.0;
    double  longitude      = 0.0;
    std::int64_t last_seen = 0;   // unix seconds; 0 = never sent / never seen
    bool    dirty          = false;

    // Latest bbox in pixel space at source resolution (1280x720).
    float bbox_left   = 0.0f;
    float bbox_top    = 0.0f;
    float bbox_width  = 0.0f;
    float bbox_height = 0.0f;

    // Most recent ReID embedding, L2-normalized. Empty until a tracker frame
    // delivers NVDS_TRACKER_OBJ_REID_META for this id. Captured at the moment
    // we go INACTIVE so the same person can't sneak back under a new track id.
    std::vector<float> reid_feature;

    // Persistent TargetState (mirrors the proto enum values; using plain int
    // here so the header doesn't have to pull in protobuf):
    //   0 = UNKNOWN   (default — bbox painted blue)
    //   1 = ACTIVE    (acknowledged — bbox painted red)
    //   5 = NEUTRALIZED (terminal-visible — bbox painted green)
    // INACTIVE (=2) is *not* stored here; it's the terminal-suppressed state,
    // which lives in `inactive_ids_` and removes the obj_meta from the frame.
    int state = 0;

    Target(int class_id, std::uint64_t obj_id) {
        // (class_id + 1) in high 32 bits guarantees high half >= 1, so id is
        // never 0 even when class_id=0 and obj_id=0. Receiver recovers the
        // original class with `(id >> 32) - 1`.
        this->id = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(class_id) + 1) << 32) | obj_id;
    }
};

class TargetManager {
public:
    TargetManager(std::string beam_url, std::string inbound_socket_path);
    ~TargetManager();

    TargetManager(const TargetManager&)            = delete;
    TargetManager& operator=(const TargetManager&) = delete;

    bool run();
    void on_batch(NvDsBatchMeta *batch_meta);
    void apply_colors(NvDsBatchMeta *batch_meta);

private:
    std::string beam_url_;
    std::string inbound_socket_path_;
    std::shared_ptr<CURL>       curl_;
    std::shared_ptr<curl_slist> headers_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Target>> targets_;
    std::mutex targets_mu_;

    // Time-bounded suppression: ids that arrived over the inbound socket with
    // state=INACTIVE, mapped to their expiry (unix seconds). Once expired, the
    // id is evicted on the next on_batch tick and a new track under the same
    // id can re-enter normally. Re-arrivals refresh the expiry.
    std::unordered_map<std::uint64_t, std::int64_t> inactive_ids_;
    // ReID embeddings captured at the moment of INACTIVE, paired with the
    // same expiry semantics. New tracks within `reid_sim_threshold_` cosine
    // similarity of any non-expired entry get suppressed too.
    std::vector<std::pair<std::vector<float>, std::int64_t>> banned_features_;
    std::mutex                        inactive_mu_;
    float                             reid_sim_threshold_ = 0.7f;
    std::size_t                       reid_max_banned_    = 256;
    int                               inactive_ttl_sec_   = 300;   // 5 min
    bool                              verbose_frames_     = false;
    std::shared_ptr<CameraGeolocator> geo_;

    // Flusher thread — every TM_DELTA_CADENCE_SEC, POST dirty targets to Beam.
    std::thread             flusher_;
    std::atomic<bool>       running_{false};
    std::condition_variable flusher_cv_;
    std::mutex              flusher_mu_;
    int                     cadence_sec_     = 5;
    std::string             beam_api_url_;
    std::string             beam_workspace_;

    void flusher_loop();
    void flush_once();
    void post_target(const Target &t);

    // Inbound Unix-domain SOCK_DGRAM listener — wss writes raw proto bytes
    // (or anything else) to inbound_socket_path_; we print whatever lands.
    int         inbound_fd_ = -1;
    std::thread inbound_thread_;
    void inbound_loop();

    void update_target(NvDsBatchMeta *tgtData);
    void unix_sock_handler();
};

#endif
