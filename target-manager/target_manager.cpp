#include "target_manager.h"
#include "target_manager.hpp"
#include "geolocation.hpp"
#include "target_proto.pb.h"

#include <nvds_tracker_meta.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char *kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string &input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    int val = 0, bits = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(kBase64Alphabet[(val >> bits) & 0x3f]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(kBase64Alphabet[((val << 8) >> (bits + 8)) & 0x3f]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

const char *getenv_or(const char *name, const char *fallback) {
    const char *v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

int getenv_int(const char *name, int fallback) {
    const char *v = std::getenv(name);
    if (!v || !*v) return fallback;
    try { return std::stoi(v); } catch (...) { return fallback; }
}

float getenv_float(const char *name, float fallback) {
    const char *v = std::getenv(name);
    if (!v || !*v) return fallback;
    try { return std::stof(v); } catch (...) { return fallback; }
}

std::int64_t now_unix_seconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

const char *state_name(int s) {
    switch (s) {
        case 0: return "UNKNOWN";
        case 1: return "CONFIRMED";
        case 2: return "NEUTRALIZED";
        case 3: return "INACTIVE";
        default: return "STATE?";
    }
}

// Copy + L2-normalize. Returns empty if input is null/empty/zero-norm.
std::vector<float> l2_normalize_copy(const float *src, std::uint32_t n) {
    if (!src || n == 0) return {};
    std::vector<float> out(src, src + n);
    double sumsq = 0.0;
    for (float x : out) sumsq += static_cast<double>(x) * x;
    double norm = std::sqrt(sumsq);
    if (norm < 1e-6) return {};
    float inv = static_cast<float>(1.0 / norm);
    for (float &x : out) x *= inv;
    return out;
}

float dot(const std::vector<float> &a, const std::vector<float> &b) {
    if (a.size() != b.size()) return -1.0f;
    float acc = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) acc += a[i] * b[i];
    return acc;
}

}  // namespace

TargetManager::TargetManager(std::string beam_url, std::string inbound_socket_path)
    : beam_url_(std::move(beam_url))
    , inbound_socket_path_(std::move(inbound_socket_path))
{
    cadence_sec_         = getenv_int("TM_DELTA_CADENCE_SEC", 5);
    beam_api_url_        = getenv_or("TM_BEAM_URL",       "http://localhost:9091/api/package/async");
    beam_workspace_      = getenv_or("TM_WORKSPACE_ID",   "71556");
    reid_sim_threshold_  = getenv_float("TM_REID_THRESHOLD", 0.7f);
    reid_max_banned_     = static_cast<std::size_t>(getenv_int("TM_REID_MAX_BANNED", 256));
    inactive_ttl_sec_    = getenv_int("TM_INACTIVE_TTL_SEC", 300);
    verbose_frames_      = getenv_int("TM_VERBOSE", 0) != 0;
}

TargetManager::~TargetManager() {
    running_.store(false);
    flusher_cv_.notify_all();
    if (flusher_.joinable()) flusher_.join();

    if (inbound_fd_ >= 0) {
        ::shutdown(inbound_fd_, SHUT_RDWR);
        ::close(inbound_fd_);
        inbound_fd_ = -1;
    }
    if (inbound_thread_.joinable()) inbound_thread_.join();
    if (!inbound_socket_path_.empty()) ::unlink(inbound_socket_path_.c_str());
}

bool TargetManager::run() {
    curl_.reset(curl_easy_init(), curl_easy_cleanup);
    if (!curl_) return false;

    headers_.reset(curl_slist_append(nullptr, "Content-Type: application/json"),
                   curl_slist_free_all);
    curl_easy_setopt(curl_.get(), CURLOPT_URL,                beam_api_url_.c_str());
    curl_easy_setopt(curl_.get(), CURLOPT_HTTPHEADER,         headers_.get());
    curl_easy_setopt(curl_.get(), CURLOPT_NOSIGNAL,           1L);
    curl_easy_setopt(curl_.get(), CURLOPT_CONNECTTIMEOUT_MS,  500L);
    curl_easy_setopt(curl_.get(), CURLOPT_TIMEOUT_MS,         2000L);

    geo_ = std::make_shared<CameraGeolocator>(load_camera_config_from_env());

    running_.store(true);
    flusher_ = std::thread(&TargetManager::flusher_loop, this);

    std::printf("[tm] flusher started: cadence=%ds beam_url=%s\n", cadence_sec_, beam_api_url_.c_str());

    if (!inbound_socket_path_.empty()) {
        ::unlink(inbound_socket_path_.c_str());
        inbound_fd_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
        if (inbound_fd_ < 0) {
            std::printf("[tm-inbound] socket() failed: %s\n", std::strerror(errno));
        } else {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, inbound_socket_path_.c_str(), sizeof(addr.sun_path) - 1);
            if (::bind(inbound_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
                std::printf("[tm-inbound] bind(%s) failed: %s\n", inbound_socket_path_.c_str(), std::strerror(errno));
                ::close(inbound_fd_);
                inbound_fd_ = -1;
            } else {
                ::chmod(inbound_socket_path_.c_str(), 0666);
                inbound_thread_ = std::thread(&TargetManager::inbound_loop, this);
                std::printf("[tm-inbound] listening on %s (AF_UNIX SOCK_DGRAM)\n", inbound_socket_path_.c_str());
            }
        }
    }

    return true;
}

void TargetManager::inbound_loop() {
    constexpr size_t kMaxDgram = 65536;
    std::vector<unsigned char> buf(kMaxDgram);

    // stdout is shared with on_batch's high-rate prints; under load the docker
    // log pipe back-pressures and inbound printfs can stall waiting for the
    // FILE lock. Mirror every event into a dedicated file FIRST so the test
    // record survives even if stdout is jammed.
    const char *log_path = getenv_or("TM_INBOUND_LOG", "/tmp/tm-inbound.log");
    std::FILE *flog = nullptr;
    if (log_path && *log_path) {
        flog = std::fopen(log_path, "a");
        if (!flog) {
            std::printf("[tm-inbound] fopen(%s) failed: %s\n", log_path, std::strerror(errno));
            std::fflush(stdout);
        } else {
            std::setvbuf(flog, nullptr, _IOLBF, 0);
            std::fprintf(flog, "[tm-inbound] log opened\n");
            std::fflush(flog);
        }
    }

#define LOG2(...) do { \
    if (flog) { std::fprintf(flog, __VA_ARGS__); std::fflush(flog); } \
    std::printf(__VA_ARGS__); std::fflush(stdout); \
} while (0)

    while (running_.load() && inbound_fd_ >= 0) {
        ssize_t n = ::recv(inbound_fd_, buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (!running_.load()) break;
            LOG2("[tm-inbound] recv error: %s\n", std::strerror(errno));
            break;
        }
        if (n == 0) continue;

        // Always log the arrival — Beam sends arbitrary content via radio and
        // we want a clear "we got SOMETHING" signal even when the bytes aren't
        // a TargetUpdate. First 32 bytes hex-dumped to fingerprint the source.
        {
            char hex[3 * 32 + 4] = {0};
            ssize_t shown = (n < 32) ? n : 32;
            for (ssize_t i = 0; i < shown; ++i) {
                std::snprintf(hex + i * 3, 4, " %02x", buf[i]);
            }
            LOG2("[tm-inbound] recv %zd bytes hex[0..%zd]:%s%s\n",
                 n, shown - 1, hex, (n > shown) ? " ..." : "");
        }

        TargetUpdate u;
        if (!u.ParseFromArray(buf.data(), static_cast<int>(n))) {
            LOG2("[tm-inbound] parse failed (%zd bytes) — likely non-TargetUpdate Beam payload\n", n);
            continue;
        }

        const std::uint64_t id = u.id();
        switch (u.state()) {
            case TARGET_STATE_CONFIRMED:
            case TARGET_STATE_NEUTRALIZED: {
                int new_state = static_cast<int>(u.state());
                const char *name = state_name(new_state);
                bool found = false;
                {
                    std::lock_guard<std::mutex> lk(targets_mu_);
                    auto it = targets_.find(id);
                    if (it != targets_.end() && it->second) {
                        it->second->state = new_state;
                        // NEUTRALIZED is terminal-visible — start the TTL clock.
                        // CONFIRMED clears any prior terminal mark (state can
                        // only arrive here via a fresh inbound update).
                        it->second->state_terminal_at =
                            (new_state == TARGET_STATE_NEUTRALIZED) ? now_unix_seconds() : 0;
                        it->second->dirty = true;   // surface on next flush
                        found = true;
                    }
                }
                LOG2("[tm-inbound] id=%" PRIu64 " -> %s%s\n",
                     id, name, found ? "" : " [unknown id, ignored]");
                break;
            }
            case TARGET_STATE_INACTIVE: {
                // Lift the live ReID feature off the target before we drop it,
                // so future re-entries with a fresh tracker id still get caught.
                std::vector<float> captured;
                {
                    std::lock_guard<std::mutex> lk(targets_mu_);
                    auto it = targets_.find(id);
                    if (it != targets_.end() && it->second) {
                        captured = std::move(it->second->reid_feature);
                    }
                    targets_.erase(id);
                }
                bool reid_banked = false;
                const std::int64_t expiry = now_unix_seconds() + inactive_ttl_sec_;
                {
                    std::lock_guard<std::mutex> lk(inactive_mu_);
                    inactive_ids_[id] = expiry;   // refresh on re-arrival
                    if (!captured.empty() && banned_features_.size() < reid_max_banned_) {
                        banned_features_.emplace_back(std::move(captured), expiry);
                        reid_banked = true;
                    }
                }
                LOG2("[tm-inbound] id=%" PRIu64 " -> INACTIVE (ttl=%ds)%s\n",
                     id, inactive_ttl_sec_, reid_banked ? " [reid banked]" : "");
                break;
            }
            default:
                LOG2("[tm-inbound] id=%" PRIu64 " state=%d (ignored)\n",
                     id, static_cast<int>(u.state()));
                break;
        }
    }

#undef LOG2

    if (flog) std::fclose(flog);
}

void TargetManager::flusher_loop() {
    while (running_.load()) {
        std::unique_lock<std::mutex> lk(flusher_mu_);
        flusher_cv_.wait_for(lk, std::chrono::seconds(cadence_sec_),
                             [this] { return !running_.load(); });
        if (!running_.load()) break;
        lk.unlock();
        try { flush_once(); }
        catch (const std::exception &e) {
            std::printf("[tm] flush_once error: %s\n", e.what());
        } catch (...) {
            std::printf("[tm] flush_once unknown error\n");
        }
    }
}

void TargetManager::flush_once() {
    std::vector<Target> snapshot;
    {
        std::lock_guard<std::mutex> lk(targets_mu_);
        snapshot.reserve(targets_.size());
        for (auto &kv : targets_) {
            if (!kv.second || !kv.second->dirty) continue;
            // Skip targets whose pixel→GPS projection has never succeeded —
            // their lat/lon would be the default (0,0) which (a) plots at the
            // equator on any consumer's map and (b) wrecks the spatial-delta
            // base picker by giving us a "lowest id" target with zero geo.
            if (kv.second->latitude == 0.0 && kv.second->longitude == 0.0) {
                kv.second->dirty = false;   // still consume the dirty bit
                continue;
            }
            snapshot.push_back(*kv.second);
            kv.second->dirty = false;
        }
    }
    if (snapshot.empty()) return;

    constexpr std::size_t MAX_PER_BATCH = 8;
    const std::size_t total = snapshot.size();
    const std::size_t n_batches = (total + MAX_PER_BATCH - 1) / MAX_PER_BATCH;
    std::printf("[tm] flush: posting %zu target(s) in %zu batch(es) of <=%zu\n",
                total, n_batches, MAX_PER_BATCH);

    for (std::size_t off = 0; off < total; off += MAX_PER_BATCH) {
        std::size_t end = std::min(off + MAX_PER_BATCH, total);
        std::vector<Target> chunk(snapshot.begin() + off, snapshot.begin() + end);
        post_batch(chunk);
    }
}

void TargetManager::post_batch(const std::vector<Target> &chunk) {
    if (chunk.empty()) return;

    // Pick base = lowest non-terminal id. Non-terminal here means
    // state != NEUTRALIZED (5). INACTIVE (2) targets are erased from
    // targets_ on the inbound transition, so they never reach this path.
    const Target *base = nullptr;
    for (const auto &t : chunk) {
        if (t.state == 5 /*NEUTRALIZED*/) continue;
        if (!base || t.id < base->id) base = &t;
    }
    // Fallback: every dirty target in the chunk is NEUTRALIZED — degenerate
    // edge case; pick the lowest id regardless so the batch still ships.
    if (!base) {
        for (const auto &t : chunk) {
            if (!base || t.id < base->id) base = &t;
        }
    }
    if (!base) return;

    std::uint64_t workspace_id_u64 = 0;
    bool          have_workspace_id = false;
    if (!beam_workspace_.empty()) {
        try { workspace_id_u64 = std::stoull(beam_workspace_); have_workspace_id = true; }
        catch (...) {}
    }

    TargetResponseDeltaList batch;
    if (have_workspace_id) batch.set_workspace_id(workspace_id_u64);

    // Pack base full state. bbox intentionally omitted — bbox lives only in
    // the per-frame UDS path. tracking_location.timestamp also omitted (was
    // duplicating updated_date.seconds); updated_date is now the sole time.
    const std::int64_t  base_ts_i64 = base->last_seen ? base->last_seen : now_unix_seconds();
    {
        TargetResponse *b = batch.mutable_base_target();
        b->set_id(base->id);
        b->mutable_updated_date()->set_seconds(base_ts_i64);
        auto *bloc = b->mutable_tracking_location();
        bloc->set_latitude (static_cast<std::int32_t>(base->latitude  * 1e7));
        bloc->set_longitude(static_cast<std::int32_t>(base->longitude * 1e7));
        b->set_state(static_cast<TargetState>(base->state));
        if (have_workspace_id) b->set_workspace_id(workspace_id_u64);
    }

    // Snapshot base scalars for delta computation.
    const std::int32_t base_id_i32    = static_cast<std::int32_t>(base->id);
    const std::int32_t base_lat_i32   = static_cast<std::int32_t>(base->latitude  * 1e7);
    const std::int32_t base_lon_i32   = static_cast<std::int32_t>(base->longitude * 1e7);
    const int          base_state_int = base->state;

    // Per-target deltas relative to base. Skip base itself.
    for (const auto &t : chunk) {
        if (&t == base) continue;
        TargetResponseDelta *d = batch.add_deltas();
        d->set_id_delta(static_cast<std::int32_t>(t.id) - base_id_i32);

        const std::int32_t this_lat_i32 = static_cast<std::int32_t>(t.latitude  * 1e7);
        const std::int32_t this_lon_i32 = static_cast<std::int32_t>(t.longitude * 1e7);

        // Location delta — only emit if any component differs from base.
        // Note: apt protoc 3.12 keeps camelCase fields literal-lowercase, so
        // accessors are set_longitudedelta / set_latitudedelta (no underscore).
        if (this_lat_i32 != base_lat_i32 || this_lon_i32 != base_lon_i32) {
            auto *dloc = d->mutable_location_delta();
            dloc->set_longitudedelta(this_lon_i32 - base_lon_i32);
            dloc->set_latitudedelta (this_lat_i32 - base_lat_i32);
        }

        // State only when it differs from base.
        if (t.state != base_state_int) {
            d->set_state(static_cast<TargetState>(t.state));
        }
    }

    std::string proto_bytes;
    if (!batch.SerializeToString(&proto_bytes)) {
        std::printf("[tm] post_batch: SerializeToString failed (n=%zu)\n", chunk.size());
        return;
    }
    std::string body =
        "{\"message\":{\"content\":\""
        + base64_encode(proto_bytes)
        + "\"},\"channels\":[\"Radio\",\"Cellular\"],\"workspaceId\":\"71556\"}";

    // Detach a worker that owns its own curl handle. Beam's `message send`
    // CLI is synchronous on the radio link, so a single POST can hang for
    // tens of seconds. Fire-and-forget keeps the flusher loop unblocked.
    std::size_t   n          = chunk.size();
    std::uint64_t base_id_u  = base->id;
    std::size_t   body_size  = body.size();
    std::size_t   proto_size = proto_bytes.size();
    std::string   url        = beam_api_url_;
    std::thread([n, base_id_u, body_size, proto_size, url = std::move(url), body = std::move(body)]() {
        CURL *c = curl_easy_init();
        if (!c) return;
        curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_URL,                url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER,         headers);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS,         body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE,      static_cast<long>(body_size));
        curl_easy_setopt(c, CURLOPT_NOSIGNAL,           1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS,  1000L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS,         30000L);

        CURLcode rc = curl_easy_perform(c);
        long http = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
        if (rc == CURLE_OK) {
            std::printf("[tm] post chunk=%zu base_id=%" PRIu64 " proto=%zuB body=%zuB http=%ld\n",
                        n, base_id_u, proto_size, body_size, http);
        } else {
            std::printf("[tm] post chunk=%zu base_id=%" PRIu64 " curl err: %s (http=%ld)\n",
                        n, base_id_u, curl_easy_strerror(rc), http);
        }
        std::fflush(stdout);

        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
    }).detach();
}

void TargetManager::on_batch(NvDsBatchMeta *batch_meta) {
    if (!batch_meta) return;

    // Evict expired suppression entries once per batch (≤30 Hz). Keeps the
    // per-object lookups below correct without checking expiry inline.
    const std::int64_t now = now_unix_seconds();
    {
        std::lock_guard<std::mutex> lk(inactive_mu_);
        for (auto it = inactive_ids_.begin(); it != inactive_ids_.end();) {
            if (it->second <= now) it = inactive_ids_.erase(it);
            else ++it;
        }
        banned_features_.erase(
            std::remove_if(banned_features_.begin(), banned_features_.end(),
                [now](const auto &p) { return p.second <= now; }),
            banned_features_.end());
    }
    // TTL on terminal-visible NEUTRALIZED: reset to UNKNOWN so the same
    // packed id can re-progress (UNKNOWN→ACTIVE→NEUTRALIZED/INACTIVE again).
    {
        std::lock_guard<std::mutex> lk(targets_mu_);
        for (auto &kv : targets_) {
            auto &t = kv.second;
            if (!t) continue;
            if (t->state == 5 /*NEUTRALIZED*/ && t->state_terminal_at > 0
                && (now - t->state_terminal_at) >= inactive_ttl_sec_) {
                t->state             = 0;
                t->state_terminal_at = 0;
                t->dirty             = true;   // surface the demotion on next flush
            }
        }
    }

    for (NvDsMetaList *lf = batch_meta->frame_meta_list; lf; lf = lf->next) {
        auto *frame_meta = static_cast<NvDsFrameMeta *>(lf->data);
        if (verbose_frames_) {
            std::printf("[tm] frame=%u pad=%u objs=%u\n",
                        frame_meta->frame_num, frame_meta->pad_index, frame_meta->num_obj_meta);
        }

        // Cache next before the body since we may remove the current node.
        for (NvDsMetaList *lo = frame_meta->obj_meta_list, *lo_next = nullptr; lo; lo = lo_next) {
            lo_next = lo->next;
            auto *obj      = static_cast<NvDsObjectMeta *>(lo->data);
            const auto &bb = obj->tracker_bbox_info.org_bbox_coords;

            // Mirrors the Target ctor packing in target_manager.hpp:
            // upper byte = class_id+1, lower byte = object_id mod 256.
            const std::uint64_t packed_id =
                (((static_cast<std::uint64_t>(obj->class_id) + 1) & 0xFFull) << 8) |
                  (static_cast<std::uint64_t>(obj->object_id)     & 0xFFull);
            {
                std::lock_guard<std::mutex> lk(inactive_mu_);
                if (inactive_ids_.count(packed_id)) {
                    std::printf("  obj id=%" PRIu64 " class=%d SUPPRESSED (inactive) -> remove from frame\n",
                                obj->object_id, obj->class_id);
                    nvds_remove_obj_meta_from_frame(frame_meta, obj);
                    continue;
                }
            }

            double foot_u = static_cast<double>(bb.left) + bb.width  * 0.5;
            double foot_v = static_cast<double>(bb.top)  + bb.height;

            const float *world_xyz       = nullptr;
            const float *visibility      = nullptr;
            const float *reid_feature    = nullptr;
            uint32_t     reid_size       = 0;

            for (NvDsUserMetaList *lu = obj->obj_user_meta_list; lu; lu = lu->next) {
                auto *um = static_cast<NvDsUserMeta *>(lu->data);
                if (!um || !um->user_meta_data) continue;

                switch (static_cast<int>(um->base_meta.meta_type)) {
                    case NVDS_TRACKER_OBJ_REID_META: {
                        auto *r        = static_cast<NvDsObjReid *>(um->user_meta_data);
                        reid_size      = r->featureSize;
                        reid_feature   = r->ptr_host;
                        break;
                    }
                    case NVDS_OBJ_IMAGE_FOOT_LOCATION: {
                        auto *uv = static_cast<float *>(um->user_meta_data);
                        foot_u = uv[0];
                        foot_v = uv[1];
                        break;
                    }
                    case NVDS_OBJ_WORLD_FOOT_LOCATION:
                        world_xyz = static_cast<float *>(um->user_meta_data);
                        break;
                    case NVDS_OBJ_VISIBILITY:
                        visibility = static_cast<float *>(um->user_meta_data);
                        break;
                    default: break;
                }
            }

            // ReID-feature blacklist: if this track's embedding is close to any
            // banked feature, treat it as inactive forever (covers re-entry under
            // a fresh tracker id). Promote the hit to inactive_ids_ so the next
            // frame short-circuits without recomputing dot products.
            std::vector<float> reid_norm = l2_normalize_copy(reid_feature, reid_size);
            if (!reid_norm.empty()) {
                bool reid_match = false;
                float best_sim = -1.0f;
                {
                    std::lock_guard<std::mutex> lk(inactive_mu_);
                    for (const auto &banned : banned_features_) {
                        float s = dot(banned.first, reid_norm);
                        if (s > best_sim) best_sim = s;
                        if (s >= reid_sim_threshold_) { reid_match = true; break; }
                    }
                    if (reid_match) {
                        inactive_ids_[packed_id] = now_unix_seconds() + inactive_ttl_sec_;
                    }
                }
                if (reid_match) {
                    std::printf("  obj id=%" PRIu64 " ReID match (sim=%.3f >= %.3f) -> SUPPRESS\n",
                                obj->object_id, best_sim, reid_sim_threshold_);
                    nvds_remove_obj_meta_from_frame(frame_meta, obj);
                    continue;
                }
            }

            auto gp = geo_->pixel_to_gps(foot_u, foot_v);

            // Threshold below which a lat/lon update is considered "no real
            // motion." 1e-5 deg ≈ 1 m at our latitudes. Keeps stationary
            // targets out of every 10s flush.
            constexpr double GEO_MOVE_THRESHOLD_DEG = 1e-5;

            bool inserted     = false;
            bool moved        = false;
            bool geo_now_set  = false;   // gp succeeded this frame
            {
                std::lock_guard<std::mutex> lk(targets_mu_);
                auto _tgt = std::make_shared<Target>(obj->class_id, obj->object_id);
                auto [it, ins] = targets_.try_emplace(_tgt->id, _tgt);
                inserted = ins;
                if (gp) {
                    geo_now_set = true;
                    const double new_lat = gp->latitude;
                    const double new_lon = gp->longitude;
                    const bool first_real_geo =
                        it->second->latitude == 0.0 && it->second->longitude == 0.0;
                    moved = first_real_geo
                         || std::abs(new_lat - it->second->latitude) > GEO_MOVE_THRESHOLD_DEG
                         || std::abs(new_lon - it->second->longitude) > GEO_MOVE_THRESHOLD_DEG;
                    it->second->latitude  = new_lat;
                    it->second->longitude = new_lon;
                }
                it->second->bbox_left   = bb.left;
                it->second->bbox_top    = bb.top;
                it->second->bbox_width  = bb.width;
                it->second->bbox_height = bb.height;
                it->second->last_seen   = now_unix_seconds();
                // Only flag dirty when the target actually moved (or this
                // is its first detection). State transitions from the inbound
                // path set dirty independently, so they still surface.
                if (moved) it->second->dirty = true;
                if (!reid_norm.empty()) it->second->reid_feature = reid_norm;
            }
            if (inserted) {
                std::printf("[tm] new id=%" PRIu64 " class=%d first_seen%s\n",
                            packed_id, obj->class_id,
                            geo_now_set ? "" : " [geo fail — foot above horizon]");
            } else if (!geo_now_set) {
                // Persistent geo failure on a known target — log once per
                // batch, not per frame, to keep noise down.
                static thread_local std::uint64_t last_geo_fail_id = 0;
                if (last_geo_fail_id != packed_id) {
                    std::printf("[tm] geo fail id=%" PRIu64 " foot=(%.0f,%.0f)\n",
                                packed_id, foot_u, foot_v);
                    last_geo_fail_id = packed_id;
                }
            }
            // Redundant color write here in on_batch — apply_colors (the
            // post-process_meta hook) is supposed to be authoritative, but
            // we've observed it not always firing. Writing in both places
            // means whichever runs last wins, and the visible bbox gets
            // colored. Reads state from targets_ under the lock we just
            // released; race is benign (worst case: one frame paints stale).
            int t_state = 0;
            {
                std::lock_guard<std::mutex> lk(targets_mu_);
                auto it = targets_.find(packed_id);
                if (it != targets_.end() && it->second) t_state = it->second->state;
            }
            {
                // Hex from client/public/app.js — must stay in sync with web UI.
                // UNKNOWN     #5F666C gray (default)
                // CONFIRMED   #226FEE blue
                // NEUTRALIZED #E4591D orange
                // INACTIVE never reaches OSD — removed from frame upstream.
                double r = 0.373, g = 0.400, b = 0.424, a = 1.0;   // UNKNOWN gray
                unsigned int width = 3;
                switch (t_state) {
                    case 1: r = 0.133; g = 0.435; b = 0.933; width = 6; break;  // CONFIRMED
                    case 2: r = 0.894; g = 0.349; b = 0.114; width = 6; break;  // NEUTRALIZED
                    default: break;
                }
                auto &rp = obj->rect_params;
                rp.border_color.red   = r;
                rp.border_color.green = g;
                rp.border_color.blue  = b;
                rp.border_color.alpha = a;
                rp.border_width       = width;
                rp.has_color_info     = 1;   // tell OSD to honor our color
            }


            if (verbose_frames_) {
                std::printf("  obj id=%" PRIu64 " class=%s(%d) det=%.2f trk=%.2f bbox=(%.0f,%.0f %.0fx%.0f)%s\n",
                            obj->object_id, obj->obj_label, obj->class_id,
                            obj->confidence, obj->tracker_confidence,
                            bb.left, bb.top, bb.width, bb.height,
                            inserted ? " [new]" : "");
                if (gp) std::printf("    geo=(%.7f,%.7f) range=%.1fm foot_uv=(%.1f,%.1f)\n",
                                    gp->latitude, gp->longitude, gp->range_m, foot_u, foot_v);
                if (world_xyz)  std::printf("    world_foot=(%.2f,%.2f,%.2f)\n", world_xyz[0], world_xyz[1], world_xyz[2]);
                if (visibility) std::printf("    visibility=%.2f\n", *visibility);
                if (reid_size)  std::printf("    reid=%uf\n", reid_size);
            }
        }
    }
    if (verbose_frames_) std::fflush(stdout);
}

void TargetManager::apply_colors(NvDsBatchMeta *batch_meta) {
    if (!batch_meta) return;

    for (NvDsMetaList *lf = batch_meta->frame_meta_list; lf; lf = lf->next) {
        auto *frame_meta = static_cast<NvDsFrameMeta *>(lf->data);
        for (NvDsMetaList *lo = frame_meta->obj_meta_list; lo; lo = lo->next) {
            auto *obj = static_cast<NvDsObjectMeta *>(lo->data);
            // MUST match the +1 packing used in on_batch / Target ctor.
            // Earlier this was missing the +1 → lookup miss → never painted.
            const std::uint64_t packed_id =
                (((static_cast<std::uint64_t>(obj->class_id) + 1) & 0xFFull) << 8) |
                  (static_cast<std::uint64_t>(obj->object_id)     & 0xFFull);

            int  state         = 0;   // UNKNOWN default
            bool state_changed = false;
            {
                std::lock_guard<std::mutex> lk(targets_mu_);
                auto it = targets_.find(packed_id);
                if (it != targets_.end() && it->second) {
                    state = it->second->state;
                    if (state != it->second->last_painted_state) {
                        it->second->last_painted_state = state;
                        state_changed = true;
                    }
                }
            }

            // Color/width per state — hex from client/public/app.js, must
            // stay in sync with web UI. INACTIVE never reaches here (removed
            // from frame upstream in on_batch).
            //   UNKNOWN     #5F666C  gray   (default)
            //   CONFIRMED   #226FEE  blue
            //   NEUTRALIZED #E4591D  orange
            double r = 0.373, g = 0.400, b = 0.424, a = 1.0;   // UNKNOWN gray
            unsigned int width = 3;
            switch (state) {
                case 1: r = 0.133; g = 0.435; b = 0.933; width = 6; break;  // CONFIRMED
                case 2: r = 0.894; g = 0.349; b = 0.114; width = 6; break;  // NEUTRALIZED
                default: break;
            }

            auto &rp = obj->rect_params;
            rp.border_color.red   = r;
            rp.border_color.green = g;
            rp.border_color.blue  = b;
            rp.border_color.alpha = a;
            rp.border_width       = width;
            rp.has_color_info     = 1;   // tell nvosd to use our color

            if (state_changed) {
                std::printf("[tm-color] id=%" PRIu64 " -> %s (rgba=%.1f,%.1f,%.1f,%.1f w=%u)\n",
                            packed_id, state_name(state), r, g, b, a, width);
                std::fflush(stdout);
            }
        }
    }
}

namespace {
std::shared_ptr<TargetManager> g_tm;
}

int tm_init(const char *beam_url, const char *inbound_socket_path) {
    try {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        auto tm = std::make_shared<TargetManager>(
            std::string{beam_url            ? beam_url            : ""},
            std::string{inbound_socket_path ? inbound_socket_path : ""});
        if (!tm->run()) return -1;
        g_tm = std::move(tm);
        return 0;
    } catch (...) {
        return -1;
    }
}

void tm_on_batch(NvDsBatchMeta *batch_meta) {
    try {
        if (g_tm) g_tm->on_batch(batch_meta);
    } catch (...) {
    }
}

void tm_apply_colors(NvDsBatchMeta *batch_meta) {
    try {
        if (g_tm) g_tm->apply_colors(batch_meta);
    } catch (...) {
    }
}

void tm_shutdown(void) {
    try {
        g_tm.reset();
        curl_global_cleanup();
    } catch (...) {
    }
}
