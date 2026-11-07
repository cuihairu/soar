# Layering rationale

The layer map is an abstraction exercise; this page records why those
boundaries are the ones worth naming.

Terms are defined in [Glossary](/glossary); state and channels are in
[Architecture overview](/architecture). Chinese version:
[拆分理由](/zh/rationale).

1. **Entry and UI are separated from playback because the UI is already
   compiled and run without a real decoder.** `runPlayerWindow` still exists
   and can present a bare fallback when ImGui is absent
   (`src/app/ui/player_window.cpp:5099`, `player_window.cpp:5205`), and
   `NullBackend` supplies the same `Player` contract without threads
   (`src/core/null_backend.cpp:12`). Keeping actions behind
   `Player` therefore has an executable payoff, not only a stylistic one.
2. **`Player` exists because the UI must not learn backend lifetime or
   thread topology.** The facade is deliberately non-copyable/non-movable
   and owns exactly one backend plus callback
   (`include/soar/core/player.h:16`, `player.h:59`), while FFmpeg ownership
   is hidden behind `IBackend` (`include/soar/core/backend.h:108`). The
   backend exposes ASS as an optional borrowed capability, with an explicit
   null contract when the build lacks libass
   (`backend.h:142`).
3. **`HttpCache` is separate because its only playback integration is a
   custom `AVIOContext`, and its file format has no FFmpeg dependency.** The
   cache contract states that it knows nothing about FFmpeg
   (`include/soar/core/http_cache.h:6`); the sole construction site wires it
   into the backend at `src/core/ffmpeg_backend.cpp:2576`. That split is why
   cache files can be diagnosed or replayed without instantiating the decode
   path.
4. **Subtitle discovery is split into three providers because their failure
   semantics and wire schemas differ, while playback tolerates all of them.**
   The shared contract requires unreachable or malformed sources to become
   “no candidates” rather than playback failure
   (`include/soar/core/subtitle_provider.h:29`); the sidecar rule depends on
   the local directory (`subtitle_provider.h:60`), the HTTP client has an
   http-only search/fetch protocol (`subtitle_provider.h:88`), and
   translation has a batched OpenAI-compatible response contract
   (`subtitle_provider.h:193`).
5. **The torrent bridge is separate because libtorrent can stop at the
   boundary of a byte-range HTTP URL.** The bridge contract explicitly keeps
   `IBackend` and FFmpeg ignorant of torrents
   (`src/p2p/torrent_stream.h:1`); playback sees
   `http://127.0.0.1:<port>/` (`torrent_stream.cpp:601`). Casting is the
   one sanctioned exception, expressed as a temporary listener rebind and URL
   rewrite (`torrent_stream.cpp:763`, `torrent_stream.cpp:784`).
6. **Casting is a separate control point because its remote peer speaks
   UPnP, not the in-process playback contract.** `CastSession`’s start
   sequence composes discovery, XML, bridge exposure, SOAP, optional GENA,
   and polling (`src/cast/cast_session.cpp:80`–`cast_session.cpp:150`);
   each protocol has a standalone parser/builder (`src/cast/ssdp.cpp:10`,
   `src/cast/device_xml.h:37`, `src/cast/soap.h:37`,
   `src/cast/eventing.cpp:44`). The media path is still the bridge’s HTTP
   URL, so the playback core remains unmodified.
7. **P2P and cast state outlive UI state because their lifecycles are
   resource lifecycles, not preference lifecycles.** Main declares the
   bridge before Player so playback is destroyed first
   (`src/app/main.cpp:353`), and declares cast after it so the session can
   stop first and restore the bridge (`main.cpp:473`). The HUD destructor
   repeats the cast-before-store teardown order
   (`src/app/ui/player_window.cpp:879`). Preferences are separate, best-effort
   text stores (`ui_state.cpp:173`, `library.cpp:540`,
   `shortcuts.cpp:448`).
