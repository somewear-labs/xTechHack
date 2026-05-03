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

    Target(int class_id, std::uint64_t obj_id) {
        this->id = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(class_id)) << 32) ^ obj_id;
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

private:
    std::string beam_url_;
    std::string inbound_socket_path_;
    std::shared_ptr<CURL>       curl_;
    std::shared_ptr<curl_slist> headers_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Target>> targets_;
    std::mutex targets_mu_;
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
