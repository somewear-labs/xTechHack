#ifndef TARGET_MANAGER_HPP
#define TARGET_MANAGER_HPP

#include "geolocation.hpp"

#include <nvdsmeta.h>
#include <gst/gst.h>

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

    // Which source pad last updated this target. Used by cross-camera ReID to
    // skip entries from the same camera when searching for a match.
    int last_seen_src = -1;

    // Persistent TargetState (mirrors the proto enum values; using plain int
    // here so the header doesn't have to pull in protobuf):
    //   0 = UNKNOWN   (default — bbox painted blue)
    //   1 = ACTIVE    (acknowledged — bbox painted red)
    //   5 = NEUTRALIZED (terminal-visible — bbox painted green, TTL-bounded)
    // INACTIVE (=2) is *not* stored here; it's the terminal-suppressed state,
    // which lives in `inactive_ids_` and removes the obj_meta from the frame.
    int state = 0;

    // Unix-seconds when state transitioned into a terminal-visible value
    // (currently only NEUTRALIZED). 0 means "not in a terminal state."
    // After inactive_ttl_sec_ has elapsed, on_batch resets state→UNKNOWN
    // so the same packed id can re-progress through the lifecycle.
    std::int64_t state_terminal_at = 0;

    // Last `state` value apply_colors painted onto the bbox. -1 = never
    // painted yet. Used to log only on state-edge transitions, since
    // apply_colors runs at ~30 Hz per object and per-frame logging would
    // flood. Updated under targets_mu_ alongside the read of `state`.
    int last_painted_state = -1;

    Target(int class_id, std::uint64_t obj_id) {
        // 16-bit packed id: upper byte = class_id+1 (1–255), lower byte =
        // track number (object_id mod 256). The +1 on the class lane
        // guarantees id is never 0, so it survives proto3's default-value
        // wire-omission rule and stays distinguishable from "field absent."
        // Receiver decodes:
        //   class_id = ((id >> 8) & 0xFF) - 1
        //   obj_id   =   id       & 0xFF
        // Class range is 0–254 (255 distinct classes); object_id wraps
        // modulo 256.
        this->id = (((static_cast<std::uint64_t>(class_id) + 1) & 0xFFull) << 8)
                 |   (static_cast<std::uint64_t>(obj_id)        & 0xFFull);
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
    // Same as on_batch but also takes the GstBuffer so we can reach the
    // NvBufSurface for bbox-crop extraction (VLM pipeline). buf may be null,
    // in which case crop extraction is skipped.
    void on_batch_with_buffer(GstBuffer *buf, NvDsBatchMeta *batch_meta);
    void apply_colors(NvDsBatchMeta *batch_meta);

    // Return the canonical targets_ key for a given (source, local tracker object).
    // Used by the deepstream_app patch to stamp canonical IDs into the per-frame JSON.
    // Returns 0 if the track isn't in the gallery yet.
    std::uint64_t get_canonical_id(std::uint32_t src_id, int class_id, std::uint64_t obj_id) const;

private:
    std::string beam_url_;
    std::string inbound_socket_path_;
    std::shared_ptr<CURL>       curl_;
    std::shared_ptr<curl_slist> headers_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Target>> targets_;
    std::mutex targets_mu_;

    // VLM crop dedup: one bbox crop per target lifetime. Set lives independent
    // of `targets_` so a target that briefly drops out and returns under the
    // same id doesn't get re-cropped — the original ReID/state is preserved.
    std::unordered_set<std::uint64_t> vlm_cropped_ids_;
    std::mutex                        vlm_mu_;

    // VLM size prior: per packed_id length-in-meters, populated by the
    // background loader from <id>.json sidecars written by bin/vlm_watch.py
    // next to /run/swl/crops/<id>.ppm. Used by on_batch to pick the
    // angular-subtense geolocation branch (range = length_m * fx / bbox_w_px)
    // when a value is available, falling back to foot-pixel projection
    // otherwise. Empty until the watchdog finishes inferring on a target.
    std::unordered_map<std::uint64_t, double> size_priors_m_;
    std::mutex                                size_priors_mu_;
    std::thread                               vlm_loader_thread_;
    std::string                               vlm_crops_dir_;
    int                                       vlm_loader_period_sec_ = 5;
    bool                                      vlm_size_prior_enabled_ = true;
    void vlm_loader_loop();

    // Cross-camera ReID: maps src_packed=(source_id<<16|packed_id) → canonical targets_ key.
    // targets_ is the single source of truth.  When a new local track appears we search
    // targets_ directly for an embedding match from a different source — same object on
    // both cameras → one shared entry, one shared state.
    std::unordered_map<std::uint64_t, std::uint64_t> xc_id_map_;  // src_packed → canonical_id
    mutable std::mutex xc_mu_;

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
    // When true, post_batch builds the proto + prints a human-readable
    // summary instead of running curl. Useful for local-only sanity checks
    // against the live tracker without firing Beam packages. Toggled by
    // the TM_DRY_RUN env var.
    bool                              dry_run_            = false;
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
    // Build a single TargetResponseList containing all dirty targets in the
    // snapshot and POST it as one Beam package. Collapses N round trips into
    // 1 (each Beam package = a separate radio TX/ack cycle), at the cost of
    // a slightly larger single payload.
    void post_batch(const std::vector<Target> &snapshot);

    // Inbound Unix-domain SOCK_DGRAM listener — wss writes raw proto bytes
    // (or anything else) to inbound_socket_path_; we print whatever lands.
    int         inbound_fd_ = -1;
    std::thread inbound_thread_;
    void inbound_loop();

    void update_target(NvDsBatchMeta *tgtData);
    void unix_sock_handler();
};

#endif
