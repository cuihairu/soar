// seed_torrent: minimal seeder for the P2P walkthrough (docs/mvp.md §5 P4).
// Creates a v1 .torrent from a local file or directory (multi-file), then
// seeds it on a fixed port so `soar --torrent-peer=127.0.0.1:<port>` can
// pull from it without any external tracker. Dev tool only — not shipped,
// not used by tests.
//
// Usage:
//   seed_torrent --file=<path> --out=<file.torrent> [--port=6881] [--rate-kb=N]
//     [--require-encryption]
// --file takes a regular file or a directory (directory => multi-file
// torrent rooted at the directory's base name). Directory entries are added
// name-sorted, so --torrent-index=N names the same file on every machine
// (libtorrent's own add_files follows the platform's readdir order, which
// differs per filesystem).
// --rate-kb caps the upload rate (bytes/s = N*1024), simulating a slow peer
// so the stream-while-downloading behavior is observable at all.
// --require-encryption sets the torrent's incoming *and* outgoing policy to
// pe_forced: the seeder then closes any plaintext peer, exactly like the
// public swarms that refuse unencrypted connections. That makes it the
// walkthrough's negative control for the MSE support (docs/mvp.md §5 P4b-5):
// a build without protocol encryption cannot pull a single byte from it.

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/hex.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace lt = libtorrent;
namespace fs = std::filesystem;

namespace {

// Recursively collects the payload under `root`, sorting each directory's
// entries by name so the resulting file_storage (and therefore the torrent's
// file indices) is identical on every machine. Only regular files are added
// — the payload dirs hold nothing else, and add_files' predicate defaults
// would include a stray symlink/socket the same way, which the walkthrough
// never creates.
void collectSorted(const fs::path& root, const fs::path& base,
                   std::vector<fs::path>* out) {
  std::vector<fs::path> entries;
  std::error_code ec;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    entries.push_back(it->path());
  }
  std::sort(entries.begin(), entries.end());
  for (const fs::path& p : entries) {
    std::error_code kind_ec;
    if (fs::is_directory(p, kind_ec) && !kind_ec) {
      collectSorted(p, base, out);
    } else if (fs::is_regular_file(p, kind_ec) && !kind_ec) {
      out->push_back(fs::relative(p, base));
    }
  }
}

// add_files, but with a deterministic (name-sorted) traversal instead of the
// platform's readdir order. The payload root's parent is the torrent root, so
// stored paths are relative to `file`'s parent — matching add_files' layout.
void addFilesSorted(lt::file_storage& fs, const std::string& file) {
  const fs::path path(file);
  const fs::path base = path.parent_path();
  std::vector<fs::path> rels;
  std::error_code ec;
  if (fs::is_directory(path, ec)) {
    collectSorted(path, base, &rels);
  } else {
    rels.push_back(fs::relative(path, base));
  }
  for (const fs::path& rel : rels) {
    const std::string rel_str = rel.generic_string();
    const std::uintmax_t size = fs::file_size(base / rel, ec);
    if (ec) {
      std::fprintf(stderr, "seed_torrent: cannot stat %s\n", (base / rel).c_str());
      std::exit(1);
    }
    fs.add_file(rel_str, static_cast<std::int64_t>(size));
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string file, out;
  int port = 6881;
  int rate_kb = 0;  // 0 = unlimited
  bool require_encryption = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a(argv[i]);
    if (a.rfind("--file=", 0) == 0) {
      file = a.substr(7);
    } else if (a.rfind("--out=", 0) == 0) {
      out = a.substr(6);
    } else if (a.rfind("--port=", 0) == 0) {
      port = std::atoi(a.c_str() + 7);
    } else if (a.rfind("--rate-kb=", 0) == 0) {
      rate_kb = std::atoi(a.c_str() + 10);
    } else if (a == "--require-encryption") {
      require_encryption = true;
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
      return 2;
    }
  }
  if (file.empty() || out.empty()) {
    std::fprintf(stderr,
                 "usage: seed_torrent --file=<path> --out=<file.torrent> "
                 "[--port=6881] [--rate-kb=N] [--require-encryption]\n");
    return 2;
  }

  lt::file_storage fs;
  // libtorrent's add_files walks with the platform's raw readdir order,
  // which is filesystem-dependent (hash order on ext4) — the same directory
  // yields a different file table on different machines, so --torrent-index
  // would name an arbitrary file. Sort the walk so a given directory always
  // produces the same indices; the walkthrough (and the tests driving it)
  // can then point at a file by name and know its index.
  addFilesSorted(fs, file);
  if (fs.num_files() < 1) {
    std::fprintf(stderr, "seed_torrent: --file has no seedable content\n");
    return 1;
  }
  // v1_only keeps the walkthrough deterministic across libtorrent versions
  // and swarm clients; piece size 0 lets create_torrent pick by size.
  lt::create_torrent ct(fs, 0, lt::create_torrent::v1_only);
  ct.set_creator("soar-seed_torrent/1");
  // create_torrent never reads file contents on its own: piece hashes are
  // computed here (against the file's parent dir, which is the torrent root).
  {
    const size_t slash = file.find_last_of("/\\");
    const std::string root = slash == std::string::npos ? std::string(".") : file.substr(0, slash);
    lt::set_piece_hashes(ct, root, [](lt::piece_index_t) {});
  }

  std::vector<char> buf;
  lt::bencode(std::back_inserter(buf), ct.generate());
  {
    std::ofstream f(out, std::ios::binary);
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (!f) {
      std::fprintf(stderr, "seed_torrent: cannot write %s\n", out.c_str());
      return 1;
    }
  }

  // Seed mode: the payload is complete on disk, skip the re-check.
  lt::add_torrent_params atp;
  atp.ti = std::make_shared<lt::torrent_info>(out);
  const size_t slash = file.find_last_of("/\\");
  atp.save_path = slash == std::string::npos ? "." : file.substr(0, slash);
  atp.flags |= lt::torrent_flags::seed_mode;

  lt::settings_pack sp;
  sp.set_str(lt::settings_pack::listen_interfaces,
             "0.0.0.0:" + std::to_string(port) + ",[::]:" + std::to_string(port));
  if (rate_kb > 0) {
    sp.set_int(lt::settings_pack::upload_rate_limit, rate_kb * 1024);
  }
#ifdef TORRENT_DISABLE_ENCRYPTION
  if (require_encryption) {
    // Refusing the flag beats silently seeding in plaintext while claiming
    // otherwise: the walkthrough's negative control would prove nothing.
    std::fprintf(stderr,
                 "seed_torrent: built without protocol encryption "
                 "(SOAR_ENABLE_TORRENT_ENCRYPTION=OFF); --require-encryption "
                 "cannot be honored\n");
    return 2;
  }
#else
  if (require_encryption) {
    // pe_forced both ways: incoming plaintext peers are closed (the
    // public-swarm behavior being reproduced) and the encrypted handshake is
    // never retried in the clear.
    sp.set_int(lt::settings_pack::in_enc_policy, lt::settings_pack::pe_forced);
    sp.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_forced);
  }
#endif
  lt::session_params sparams;
  sparams.settings = std::move(sp);
  lt::session session(sparams);
  lt::torrent_handle th;
  try {
    th = session.add_torrent(atp);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "seed_torrent: cannot seed: %s\n", e.what());
    return 1;
  }
  if (rate_kb > 0) {
    // Torrent-level cap: the session-wide upload_rate_limit alone proved
    // not to bite on localhost loops, this one is enforced in the picker.
    th.set_upload_limit(rate_kb * 1024);
  }

  std::fprintf(stderr, "seed_torrent: %s\n", out.c_str());
  std::fprintf(stderr, "seed_torrent: info hash %s\n",
               lt::aux::to_hex(th.info_hashes().v1).c_str());
  std::fprintf(stderr, "seed_torrent: seeding on port %d (payload dir: %s)\n", port,
               atp.save_path.c_str());
#ifdef TORRENT_DISABLE_ENCRYPTION
  std::fprintf(stderr, "seed_torrent: protocol encryption not compiled in\n");
#else
  std::fprintf(stderr, "seed_torrent: protocol encryption %s\n",
               require_encryption ? "required (pe_forced)" : "preferred");
#endif

  // Loop forever; upload accounting goes to stderr as a heartbeat.
  long long last_uploaded = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const lt::torrent_status st = th.status();
    const long long uploaded = static_cast<long long>(st.total_upload);
    if (uploaded != last_uploaded) {
      std::fprintf(stderr, "seed_torrent: uploaded %lld/%lld bytes (peers %d)\n",
                   uploaded, static_cast<long long>(st.total_done), st.num_peers);
      last_uploaded = uploaded;
    }
  }
}
