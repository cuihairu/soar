// HTTP disk cache: play from the network, keep the bytes.
//
// One HttpCache instance serves exactly one source URL. Data lives in two
// files under cache_dir — a small meta file (plaintext url, total size,
// one bit per 256 KiB block) and a sparse data file. Reads are served from
// disk; blocks that are not cached yet are fetched with HTTP Range
// requests, stored, and only then returned. The bitmap format supports
// holes in the middle of the file (seek-past-end in P3b fills them the
// same way a sequential read does today).
//
// The component deliberately knows nothing about FFmpeg: it takes a URL
// and serves bytes, so a non-FFmpeg consumer could reuse it unchanged.
//
// Thread-safety contract: exactly one thread may call read()/fetchAll()
// at a time. Behind the FFmpeg backend that is the decode thread inside
// av_read_frame — the same thread the network watchdog timestamps, so a
// slow block fetch feeds the existing BufferingStarted/Ended events and
// the 60 s abort without any extra plumbing. The socket recv inside
// fetchBlock() is NOT interruptible by that watchdog (it never enters
// FFmpeg's poll loop), so the socket carries SO_RCVTIMEO (kRecvTimeoutSec)
// as the hard stop: a dead server degrades into an IO error, never an
// infinite block.
//
// http only by design for now: https:// sources are not passed to this
// component (the backend routes them to the direct path), because a TLS
// dependency (OpenSSL) is deliberately out of scope for this batch.

#ifndef SOAR_CORE_HTTP_CACHE_H_
#define SOAR_CORE_HTTP_CACHE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace soar {

class HttpCache {
 public:
  // Prepares (or reopens) the cache files for `url` under `cache_dir`.
  // The constructor probes the source size with a "Range: bytes=0-0"
  // request; a meta file whose url does not match is rebuilt from
  // scratch. Check valid() (and error()) after construction.
  HttpCache(std::string cache_dir, std::string url);
  ~HttpCache();

  HttpCache(const HttpCache&) = delete;
  HttpCache& operator=(const HttpCache&) = delete;

  bool valid() const { return valid_; }
  const std::string& error() const { return last_error_; }
  uint64_t size() const { return size_; }
  uint64_t cachedBytes() const;

  // Reads up to len bytes at offset into dst, fetching uncached blocks
  // from the network as needed. Returns the number of bytes actually
  // read (short only at end of stream), 0 at/after EOF, and npos on
  // error (last_error_ then carries the reason).
  static constexpr size_t npos = static_cast<size_t>(-1);
  size_t read(uint64_t offset, uint8_t* dst, size_t len);

  // Fetches every not-yet-cached block (whole-file warm-up / resume).
  bool fetchAll();

  // Paths of the two cache files (exposed for tests and diagnostics).
  const std::string& metaPath() const { return meta_path_; }
  const std::string& dataPath() const { return data_path_; }

 private:
  void openFiles();
  void rebuildMeta();   // (re)write meta from scratch
  bool loadMeta();      // returns true when an existing meta matches url
  bool saveMeta();      // writes meta atomically-ish via <path>.tmp + rename
  bool ensureBlock(uint32_t block);
  bool fetchBlock(uint32_t block);   // HTTP Range fetch for one block
  bool fetchRange(uint64_t begin, uint32_t len, std::vector<uint8_t>* out);

  std::string cache_dir_;
  std::string url_;
  std::string meta_path_;
  std::string data_path_;
  uint64_t size_ = 0;
  uint32_t block_count_ = 0;
  std::vector<uint8_t> bitmap_;  // 1 bit per block, bit i = block i
  int data_fd_ = -1;  // POSIX fd / Windows CRT handle, open for the lifetime
  bool valid_ = false;
  std::string last_error_;
};

}  // namespace soar

#endif  // SOAR_CORE_HTTP_CACHE_H_
