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

#include <chrono>
#include <cinttypes>
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

std::int64_t now_unix_seconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

}  // namespace

TargetManager::TargetManager(std::string beam_url, std::string inbound_socket_path)
    : beam_url_(std::move(beam_url))
    , inbound_socket_path_(std::move(inbound_socket_path))
{
    cadence_sec_    = getenv_int("TM_DELTA_CADENCE_SEC", 5);
    beam_api_url_   = getenv_or("TM_BEAM_URL",       "http://localhost:9091/api/package/async");
    beam_workspace_ = getenv_or("TM_WORKSPACE_ID",   "71556");
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

    while (running_.load() && inbound_fd_ >= 0) {
        ssize_t n = ::recv(inbound_fd_, buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (!running_.load()) break;
            std::printf("[tm-inbound] recv error: %s\n", std::strerror(errno));
            break;
        }
        if (n == 0) continue;

        std::printf("[tm-inbound] recv %zd bytes:", n);
        for (ssize_t i = 0; i < n && i < 256; ++i) {
            if ((i & 0xF) == 0) std::printf("\n  ");
            std::printf(" %02x", buf[i]);
        }
        if (n > 256) std::printf("\n  ... (%zd more bytes)", n - 256);
        std::printf("\n");
        std::fflush(stdout);
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
            if (kv.second && kv.second->dirty) {
                snapshot.push_back(*kv.second);
                kv.second->dirty = false;
            }
        }
    }
    if (snapshot.empty()) return;

    std::printf("[tm] flush: posting %zu target(s) to beam\n", snapshot.size());
    for (const auto &t : snapshot) {
        post_target(t);
    }
}

void TargetManager::post_target(const Target &t) {
    TargetResponse msg;
    msg.set_id(static_cast<std::int64_t>(t.id));
    auto *upd = msg.mutable_updated_date();
    upd->set_seconds(t.last_seen ? t.last_seen : now_unix_seconds());
    upd->set_nanos(0);
    auto *loc = msg.mutable_tracking_location();
    loc->set_latitude (static_cast<std::int32_t>(t.latitude  * 1e7));
    loc->set_longitude(static_cast<std::int32_t>(t.longitude * 1e7));
    loc->set_timestamp(static_cast<std::uint32_t>(t.last_seen ? t.last_seen : now_unix_seconds()));
    msg.set_state(::TARGET_STATE_UNKNOWN);
    if (!beam_workspace_.empty()) {
        try { msg.set_workspace_id(std::stoull(beam_workspace_)); } catch (...) {}
    }
    // Pack bbox as four uint16 fields: (left<<48) | (top<<32) | (width<<16) | height.
    std::uint64_t bbox_packed =
        (static_cast<std::uint64_t>(static_cast<std::uint16_t>(t.bbox_left))   << 48) |
        (static_cast<std::uint64_t>(static_cast<std::uint16_t>(t.bbox_top))    << 32) |
        (static_cast<std::uint64_t>(static_cast<std::uint16_t>(t.bbox_width))  << 16) |
         static_cast<std::uint64_t>(static_cast<std::uint16_t>(t.bbox_height));
    msg.set_bbox(bbox_packed);

    std::string proto_bytes;
    if (!msg.SerializeToString(&proto_bytes)) {
        std::printf("[tm] post_target: SerializeToString failed for id=%" PRIu64 "\n", t.id);
        return;
    }
    std::string body =
        "{\"message\":{\"content\":\""
        + base64_encode(proto_bytes)
        + "\"},\"channels\":[\"Radio\",\"Cellular\"],\"workspaceId\":\"71556\"}";

    // Detach a worker that owns its own curl handle. Beam's `message send`
    // CLI is synchronous on the radio link, so a single POST can hang for
    // tens of seconds. Fire-and-forget keeps the flusher loop unblocked.
    std::uint64_t id  = t.id;
    std::string   url = beam_api_url_;
    std::thread([id, url = std::move(url), body = std::move(body)]() {
        CURL *c = curl_easy_init();
        if (!c) return;
        curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_URL,                url.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER,         headers);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS,         body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE,      static_cast<long>(body.size()));
        curl_easy_setopt(c, CURLOPT_NOSIGNAL,           1L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS,  1000L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS,         30000L);

        CURLcode rc = curl_easy_perform(c);
        long http = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
        if (rc == CURLE_OK) {
            std::printf("[tm] post id=%" PRIu64 " http=%ld\n", id, http);
        } else {
            std::printf("[tm] post id=%" PRIu64 " curl err: %s (http=%ld)\n",
                        id, curl_easy_strerror(rc), http);
        }
        std::fflush(stdout);

        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
    }).detach();
}

void TargetManager::on_batch(NvDsBatchMeta *batch_meta) {
    if (!batch_meta) return;

    for (NvDsMetaList *lf = batch_meta->frame_meta_list; lf; lf = lf->next) {
        auto *frame_meta = static_cast<NvDsFrameMeta *>(lf->data);
        std::printf("[tm] frame=%u pad=%u objs=%u\n",
                    frame_meta->frame_num, frame_meta->pad_index, frame_meta->num_obj_meta);

        for (NvDsMetaList *lo = frame_meta->obj_meta_list; lo; lo = lo->next) {
            auto *obj      = static_cast<NvDsObjectMeta *>(lo->data);
            const auto &bb = obj->tracker_bbox_info.org_bbox_coords;

            double foot_u = static_cast<double>(bb.left) + bb.width  * 0.5;
            double foot_v = static_cast<double>(bb.top)  + bb.height;

            const float *world_xyz = nullptr;
            const float *visibility = nullptr;
            uint32_t reid_size = 0;

            for (NvDsUserMetaList *lu = obj->obj_user_meta_list; lu; lu = lu->next) {
                auto *um = static_cast<NvDsUserMeta *>(lu->data);
                if (!um || !um->user_meta_data) continue;

                switch (static_cast<int>(um->base_meta.meta_type)) {
                    case NVDS_TRACKER_OBJ_REID_META:
                        reid_size = static_cast<NvDsObjReid *>(um->user_meta_data)->featureSize;
                        break;
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

            auto gp = geo_->pixel_to_gps(foot_u, foot_v);

            bool inserted = false;
            {
                std::lock_guard<std::mutex> lk(targets_mu_);
                auto _tgt = std::make_shared<Target>(obj->class_id, obj->object_id);
                auto [it, ins] = targets_.try_emplace(_tgt->id, _tgt);
                inserted = ins;
                if (gp) {
                    it->second->latitude  = gp->latitude;
                    it->second->longitude = gp->longitude;
                }
                it->second->bbox_left   = bb.left;
                it->second->bbox_top    = bb.top;
                it->second->bbox_width  = bb.width;
                it->second->bbox_height = bb.height;
                it->second->last_seen = now_unix_seconds();
                it->second->dirty     = true;
            }

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
    std::fflush(stdout);
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

void tm_shutdown(void) {
    try {
        g_tm.reset();
        curl_global_cleanup();
    } catch (...) {
    }
}
