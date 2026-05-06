#include "target_manager.h"
#include "target_manager.hpp"
#include "geolocation.hpp"
#include "target_proto.pb.h"

#include <nvds_tracker_meta.h>
#include <nvbufsurface.h>
#include <nvbufsurftransform.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <errno.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <optional>
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
    vlm_crops_dir_           = getenv_or("TM_VLM_CROPS_DIR", "/run/swl/crops");
    vlm_loader_period_sec_   = getenv_int("TM_VLM_LOADER_PERIOD_SEC", 5);
    vlm_size_prior_enabled_  = getenv_int("TM_VLM_SIZE_PRIOR_ENABLED", 1) != 0;
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
    if (vlm_loader_thread_.joinable()) vlm_loader_thread_.join();
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
    if (vlm_size_prior_enabled_) {
        vlm_loader_thread_ = std::thread(&TargetManager::vlm_loader_loop, this);
    }

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

    // Persistent state-transition log. Default points at /run/swl which is
    // bind-mounted from the host (swl-vision/run/), so entries survive
    // container recreate — answers "did this state change ever land?" without
    // chasing ephemeral docker logs. One line per inbound state event.
    const char *xlog_path = getenv_or("TM_STATE_LOG", "/run/swl/tm-state.log");
    std::FILE *xlog = nullptr;
    if (xlog_path && *xlog_path) {
        xlog = std::fopen(xlog_path, "a");
        if (xlog) {
            std::setvbuf(xlog, nullptr, _IOLBF, 0);
        }
    }
    auto log_transition = [&xlog](std::uint64_t id, const char *new_state, const char *flags) {
        if (!xlog) return;
        char ts[32];
        std::time_t now = std::time(nullptr);
        std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
        int class_id = static_cast<int>((id >> 8) & 0xFFu) - 1;
        int obj_id   = static_cast<int>(id & 0xFFu);
        std::fprintf(xlog, "%s id=%" PRIu64 " class=%d obj=%d -> %s%s\n",
                     ts, id, class_id, obj_id, new_state, flags);
        std::fflush(xlog);
    };

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
                log_transition(id, name, found ? "" : " [unknown id, ignored]");
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
                {
                    char flags[64];
                    std::snprintf(flags, sizeof(flags), " ttl=%d%s",
                                  inactive_ttl_sec_, reid_banked ? " [reid banked]" : "");
                    log_transition(id, "INACTIVE", flags);
                }
                break;
            }
            default: {
                LOG2("[tm-inbound] id=%" PRIu64 " state=%d (ignored)\n",
                     id, static_cast<int>(u.state()));
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(u.state()));
                log_transition(id, buf, " [ignored]");
                break;
            }
        }
    }

#undef LOG2

    if (flog) std::fclose(flog);
}

// Background loader for VLM size priors. Polls vlm_crops_dir_ every
// vlm_loader_period_sec_ seconds for <id>.json sidecars, parses out the
// length_m field, and inserts into size_priors_m_. One-shot per id —
// once a prior is recorded it isn't refreshed (the watchdog only writes
// once per target lifetime anyway). Tolerates a missing dir / missing
// watchdog: just keeps polling.
void TargetManager::vlm_loader_loop() {
    while (running_.load()) {
        DIR *dir = ::opendir(vlm_crops_dir_.c_str());
        if (dir) {
            struct dirent *ent;
            while ((ent = ::readdir(dir)) != nullptr) {
                std::string name = ent->d_name;
                if (name.size() < 6) continue;
                if (name.compare(name.size() - 5, 5, ".json") != 0) continue;
                // Parse <id>.json — id is decimal packed_id.
                std::uint64_t packed_id = 0;
                try { packed_id = std::stoull(name.substr(0, name.size() - 5)); }
                catch (...) { continue; }
                {
                    std::lock_guard<std::mutex> lk(size_priors_mu_);
                    if (size_priors_m_.count(packed_id)) continue;
                }
                std::string path = vlm_crops_dir_ + "/" + name;
                std::FILE *f = std::fopen(path.c_str(), "r");
                if (!f) continue;
                char buf[1024];
                size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
                std::fclose(f);
                buf[n] = '\0';
                // Find "length_m": <number>. Cheap hand parse to avoid
                // dragging in a JSON dep.
                const char *p = std::strstr(buf, "\"length_m\"");
                if (!p) continue;
                p = std::strchr(p, ':');
                if (!p) continue;
                ++p;
                while (*p == ' ' || *p == '\t') ++p;
                char *endp = nullptr;
                double length_m = std::strtod(p, &endp);
                if (endp == p || length_m <= 0.0) continue;
                {
                    std::lock_guard<std::mutex> lk(size_priors_mu_);
                    size_priors_m_[packed_id] = length_m;
                }
                std::printf("[tm-vlm] size prior id=%" PRIu64 " length_m=%.1f\n",
                            packed_id, length_m);
                std::fflush(stdout);
            }
            ::closedir(dir);
        }
        // Sleep with cv so destructor can wake us early.
        std::unique_lock<std::mutex> lk(flusher_mu_);
        flusher_cv_.wait_for(lk, std::chrono::seconds(vlm_loader_period_sec_),
                             [this] { return !running_.load(); });
    }
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
        + "\"},\"channels\":[\"Radio\"],\"workspaceId\":\"71556\"}";  // mesh-only; workspace-broadcast (no targetUserId).

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

            // Prefer the VLM-driven size prior when available — at our
            // horizon-grazing pitch (CAM_PITCH=0) the foot-pixel method is
            // ill-conditioned (1 px of v ≈ tens of meters of range), but
            // angular-subtense from a known vessel length is well-behaved.
            // Fall back to foot-pixel projection until the prior arrives.
            std::optional<GeoPoint> gp;
            if (vlm_size_prior_enabled_) {
                double length_m = 0.0;
                {
                    std::lock_guard<std::mutex> lk(size_priors_mu_);
                    auto it = size_priors_m_.find(packed_id);
                    if (it != size_priors_m_.end()) length_m = it->second;
                }
                if (length_m > 0.0 && bb.width > 0.5f) {
                    const double u_center = static_cast<double>(bb.left) + bb.width  * 0.5;
                    const double v_center = static_cast<double>(bb.top)  + bb.height * 0.5;
                    gp = geo_->pixel_to_gps_from_size(u_center, v_center,
                                                     static_cast<double>(bb.width),
                                                     length_m);
                }
            }
            if (!gp) gp = geo_->pixel_to_gps(foot_u, foot_v);

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

// Bbox crop via NvBufSurfTransform — crops + color-converts (NV12_709 → RGBA)
// + scales the bbox region into a CPU-mappable destination surface. Source
// memory on Jetson DeepStream is NVBUF_MEM_SURFACE_ARRAY (GPU-only), so we
// can't NvBufSurfaceMap it directly; the Transform path is the supported way.
// Output: PPM (P6) at /run/swl/crops/<id>.ppm, sized 256x256 for VLM input.
static constexpr int kVLMCropSize = 256;

static bool tm_vlm_write_ppm_crop(NvBufSurface *src,
                                  uint32_t      frame_idx,
                                  int bbox_l, int bbox_t, int bbox_w, int bbox_h,
                                  std::uint64_t target_id) {
    if (!src || frame_idx >= src->numFilled) return false;

    const int W = static_cast<int>(src->surfaceList[frame_idx].width);
    const int H = static_cast<int>(src->surfaceList[frame_idx].height);
    const int pad = 8;
    int x0 = std::max(0, bbox_l - pad);
    int y0 = std::max(0, bbox_t - pad);
    int x1 = std::min(W, bbox_l + bbox_w + pad);
    int y1 = std::min(H, bbox_t + bbox_h + pad);
    if (x1 <= x0 || y1 <= y0) return false;
    const uint32_t crop_w = static_cast<uint32_t>(x1 - x0);
    const uint32_t crop_h = static_cast<uint32_t>(y1 - y0);

    // Allocate destination surface (RGBA, system-mappable, 256x256).
    NvBufSurfaceCreateParams cparams{};
    cparams.gpuId       = 0;
    cparams.width       = kVLMCropSize;
    cparams.height      = kVLMCropSize;
    cparams.size        = 0;
    cparams.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
    cparams.layout      = NVBUF_LAYOUT_PITCH;
    cparams.memType     = NVBUF_MEM_DEFAULT;       // Jetson-recommended; Tegra DMABUF

    NvBufSurface *dst = nullptr;
    if (NvBufSurfaceCreate(&dst, 1, &cparams) != 0 || !dst) {
        std::printf("[tm-vlm] NvBufSurfaceCreate failed (id=%" PRIu64 ")\n", target_id);
        return false;
    }
    dst->numFilled = 1;

    // Init transform session once per process; harmless if called repeatedly.
    static bool s_xfm_inited = false;
    if (!s_xfm_inited) {
        NvBufSurfTransformConfigParams sp{};
        sp.gpu_id = 0;
        sp.compute_mode = NvBufSurfTransformCompute_Default;
        if (NvBufSurfTransformSetSessionParams(&sp) != NvBufSurfTransformError_Success) {
            std::printf("[tm-vlm] NvBufSurfTransformSetSessionParams failed\n");
        }
        s_xfm_inited = true;
    }

    NvBufSurfTransformRect src_rect{};
    src_rect.top    = static_cast<uint32_t>(y0);
    src_rect.left   = static_cast<uint32_t>(x0);
    src_rect.width  = crop_w;
    src_rect.height = crop_h;
    NvBufSurfTransformRect dst_rect{};
    dst_rect.top    = 0;
    dst_rect.left   = 0;
    dst_rect.width  = kVLMCropSize;
    dst_rect.height = kVLMCropSize;

    NvBufSurfTransformParams tp{};
    tp.transform_flag    = NVBUFSURF_TRANSFORM_FILTER
                         | NVBUFSURF_TRANSFORM_CROP_SRC
                         | NVBUFSURF_TRANSFORM_CROP_DST;
    tp.transform_filter  = NvBufSurfTransformInter_Default;
    tp.src_rect          = &src_rect;
    tp.dst_rect          = &dst_rect;

    NvBufSurfTransform_Error xerr = NvBufSurfTransform(src, dst, &tp);
    if (xerr != NvBufSurfTransformError_Success) {
        std::printf("[tm-vlm] NvBufSurfTransform failed err=%d (id=%" PRIu64
                    ") src memType=%d colorFmt=%d %ux%u  rect=%d,%d %ux%u\n",
                    static_cast<int>(xerr), target_id,
                    static_cast<int>(src->memType),
                    static_cast<int>(src->surfaceList[frame_idx].colorFormat),
                    src->surfaceList[frame_idx].width,
                    src->surfaceList[frame_idx].height,
                    x0, y0, crop_w, crop_h);
        NvBufSurfaceDestroy(dst);
        return false;
    }

    if (NvBufSurfaceMap(dst, 0, 0, NVBUF_MAP_READ) != 0) {
        std::printf("[tm-vlm] dst Map failed (id=%" PRIu64 ")\n", target_id);
        NvBufSurfaceDestroy(dst);
        return false;
    }
    NvBufSurfaceSyncForCpu(dst, 0, 0);

    const uint8_t *rgba = reinterpret_cast<uint8_t*>(dst->surfaceList[0].mappedAddr.addr[0]);
    const int dpitch = static_cast<int>(dst->surfaceList[0].planeParams.pitch[0]);
    if (!rgba) {
        std::printf("[tm-vlm] dst mapped null (id=%" PRIu64 ")\n", target_id);
        NvBufSurfaceUnMap(dst, 0, 0);
        NvBufSurfaceDestroy(dst);
        return false;
    }

    // mkdir -p /run/swl/crops on first call.
    static bool s_dir_made = false;
    const char *dir = "/run/swl/crops";
    if (!s_dir_made) { ::mkdir(dir, 0755); s_dir_made = true; }
    char path[128];
    std::snprintf(path, sizeof(path), "%s/%" PRIu64 ".ppm", dir, target_id);

    std::FILE *f = std::fopen(path, "wb");
    if (!f) {
        std::printf("[tm-vlm] fopen(%s) failed: %s\n", path, std::strerror(errno));
        NvBufSurfaceUnMap(dst, 0, 0);
        NvBufSurfaceDestroy(dst);
        return false;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", kVLMCropSize, kVLMCropSize);

    // RGBA → RGB row-by-row (drop alpha).
    std::vector<uint8_t> rgb(kVLMCropSize * 3);
    for (int y = 0; y < kVLMCropSize; ++y) {
        const uint8_t *r = rgba + y * dpitch;
        for (int x = 0; x < kVLMCropSize; ++x) {
            rgb[x * 3 + 0] = r[x * 4 + 0];
            rgb[x * 3 + 1] = r[x * 4 + 1];
            rgb[x * 3 + 2] = r[x * 4 + 2];
        }
        std::fwrite(rgb.data(), 1, rgb.size(), f);
    }
    std::fclose(f);

    NvBufSurfaceUnMap(dst, 0, 0);
    NvBufSurfaceDestroy(dst);
    std::printf("[tm-vlm] crop id=%" PRIu64 " src=%ux%u → %s\n",
                target_id, crop_w, crop_h, path);
    std::fflush(stdout);
    return true;
}

void TargetManager::on_batch_with_buffer(GstBuffer *buf, NvDsBatchMeta *batch_meta) {
    // Run all the normal on_batch processing first (suppression, geo, dirty,
    // etc.). The buffer side-channel below extracts bbox crops from the
    // NvBufSurface for the on-board VLM (vessel-class + size prior).
    on_batch(batch_meta);

    if (!buf || !batch_meta) return;

    GstMapInfo info;
    if (!gst_buffer_map(buf, &info, GST_MAP_READ)) return;
    NvBufSurface *surface = reinterpret_cast<NvBufSurface*>(info.data);

    // Walk the batch and extract one crop per never-before-seen target id.
    if (surface) {
        uint32_t frame_idx = 0;
        for (NvDsMetaList *lf = batch_meta->frame_meta_list;
             lf && frame_idx < surface->numFilled; lf = lf->next, ++frame_idx) {
            NvDsFrameMeta *frame_meta = static_cast<NvDsFrameMeta*>(lf->data);
            if (!frame_meta) continue;
            for (NvDsMetaList *lo = frame_meta->obj_meta_list; lo; lo = lo->next) {
                NvDsObjectMeta *obj = static_cast<NvDsObjectMeta*>(lo->data);
                if (!obj) continue;
                std::uint64_t packed_id =
                    (((static_cast<std::uint64_t>(obj->class_id) + 1) & 0xFFull) << 8)
                    | (static_cast<std::uint64_t>(obj->object_id) & 0xFFull);
                {
                    std::lock_guard<std::mutex> lk(vlm_mu_);
                    if (vlm_cropped_ids_.count(packed_id)) continue;
                    vlm_cropped_ids_.insert(packed_id);
                }
                const auto &bb = obj->tracker_bbox_info.org_bbox_coords;
                tm_vlm_write_ppm_crop(surface, frame_idx,
                                      static_cast<int>(bb.left),
                                      static_cast<int>(bb.top),
                                      static_cast<int>(bb.width),
                                      static_cast<int>(bb.height),
                                      packed_id);
            }
        }
    }
    gst_buffer_unmap(buf, &info);
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

void tm_on_batch_with_buffer(GstBuffer *buf, NvDsBatchMeta *batch_meta) {
    try {
        if (g_tm) g_tm->on_batch_with_buffer(buf, batch_meta);
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
