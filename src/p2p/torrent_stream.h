// TorrentStream: BitTorrent "stream while downloading" over local HTTP
// (docs/mvp.md §5 P4).
//
// libtorrent (vendored under third_party/libtorrent, see VENDOR.md there)
// is the download kernel only: pieces land as real files in a store
// directory. A 127.0.0.1-only HTTP server presents the chosen file of the
// torrent with byte-range support, so the player just opens
// "http://127.0.0.1:<port>/" — the IBackend layer and the FFmpeg backend
// learn nothing about torrents ("核心抽象不动", docs/mvp.md §5).
//
// Reads ahead of the sequential download front turn into per-piece
// deadlines (libtorrent streaming): the HTTP handler asks for exactly the
// pieces the demuxer seeks into, waits for them up to a timeout, then
// serves straight from disk. Seek-backs inside already-downloaded pieces
// never touch the network.
//
// The class owns its threads (session internals, a status monitor, one
// acceptor, one detached thread per HTTP connection). All methods are safe
// to call from the thread that called start(); with startAsync() the
// progress/files callbacks arrive on the worker instead, and playbackUrl()
// /fileName()/fileSize()/lastError() are meant to be read only after
// phase() reports Serving/Failed (their publication is ordered by it).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace soar::p2p {

// Lifecycle of an async start (startAsync): Connecting while the swarm
// delivers the metadata and the file geometry is being applied, then
// Serving once the bridge listens; any failure lands in Failed with
// lastError(). A synchronous start() jumps straight from Idle to Serving
// (or Failed) before it returns.
enum class TorrentPhase { Idle, Connecting, Serving, Failed };

struct TorrentFile {
  int index;
  std::string path;  // path inside the torrent
  std::uint64_t size;
};

struct TorrentStatus {
  std::uint64_t downloaded = 0;  // bytes of the whole torrent
  std::uint64_t total = 0;       // whole torrent size (0 until metadata)
  int peers = 0;
  bool metadata = false;
};

class TorrentStream {
 public:
  struct Params {
    std::string torrent_path;  // .torrent file
    // "magnet:?xt=urn:btih:<hash>&tr=<tracker>&..." — exactly one of
    // torrent_path / magnet_uri must be set. Metadata (the info dictionary)
    // then arrives over the swarm; start() fails explicitly if it never
    // does (see kMetadataWaitMs in the implementation).
    std::string magnet_uri;
    std::string store_dir;     // piece storage; real files materialize here
    int file_index = 0;        // multi-file torrents: which entry to serve
    // "host:port" endpoints to connect to directly (trackerless local
    // swarms / walkthroughs). Trackers and DHT run regardless.
    std::vector<std::string> peers;
    // Invoked from an internal thread at most ~1 Hz with the latest status.
    std::function<void(const TorrentStatus&)> on_progress;
    // Invoked exactly once from start() once the metadata is available,
    // before the file selection is applied: the full file table of the
    // torrent (multi-file listing, --torrent-index picking). A magnet pays
    // the metadata wait before this fires; a .torrent fires immediately.
    std::function<void(const std::vector<TorrentFile>&)> on_files;
  };

  TorrentStream() = default;
  ~TorrentStream();
  TorrentStream(const TorrentStream&) = delete;
  TorrentStream& operator=(const TorrentStream&) = delete;

  // Parses the torrent, starts the session and the local server. Returns
  // false and fills lastError() on failure (unreadable torrent, server
  // cannot listen, metadata never arrives).
  bool start(const Params& params);

  // Same contract as start(), except everything after the torrent is added
  // to the session (metadata wait for magnets, file selection, spawning the
  // bridge threads) runs on an internal worker: the call returns
  // immediately with phase() == Connecting, and the outcome is observed
  // through phase()/lastError(). playbackUrl() is meaningless until
  // Serving. stop() cancels the wait cleanly (phase back to Idle).
  bool startAsync(const Params& params);

  TorrentPhase phase() const { return phase_.load(std::memory_order_acquire); }

  // Stops accepting and tearing down the session; pending reads fail.
  // Called by the destructor; idempotent.
  void stop();

  // Valid after a successful start(): "http://127.0.0.1:<port>/"
  const std::string& playbackUrl() const { return playback_url_; }

  std::uint64_t fileSize() const { return file_size_; }
  const std::string& fileName() const { return file_name_; }

  TorrentStatus status() const;

  const std::string& lastError() const { return last_error_; }

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  std::string playback_url_;
  std::string file_name_;
  std::string last_error_;
  std::atomic<TorrentPhase> phase_{TorrentPhase::Idle};
  std::uint64_t file_size_ = 0;
};

}  // namespace soar::p2p
