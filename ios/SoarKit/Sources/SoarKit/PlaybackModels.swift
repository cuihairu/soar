import Foundation

/// Playback state machine — mirrors `soar::PlaybackState`
/// (include/soar/core/backend.h). Cases and order are unchanged; the
/// desktop-side transitions are the contract for `MediaEngine`
/// implementations (see NullEngine).
public enum PlaybackState: String, Sendable, Equatable {
    case stopped
    case paused
    case playing
    case ended
    case error
}

/// Event kinds — mirrors `soar::EventType` (include/soar/core/backend.h).
public enum EventType: String, Sendable, Equatable {
    case stateChanged
    case mediaInfoChanged
    case positionChanged
    case error
    /// Network-source progress: the backend stopped receiving data fast
    /// enough to keep playing (started) and resumed doing so (ended).
    /// Local media never emits these.
    case bufferingStarted
    case bufferingEnded
    /// While playing through the http:// disk cache: cached bytes grow —
    /// `downloaded` counts cached bytes out of `total` source bytes,
    /// throttled to 1/16 steps; the series is bounded and monotonic with a
    /// terminal `downloaded == total`. Non-cache sources never emit it.
    case downloadProgress
}

/// A playback event — mirrors `soar::Event`. Field set is frozen to match
/// the desktop contract (the C++ struct carries type / position /
/// downloaded / total / message; see backend.h before adding anything).
public struct SoarEvent: Sendable, Equatable {
    public let type: EventType
    public let position: Int64
    public let downloaded: Int64
    public let total: Int64
    public let message: String

    public init(type: EventType,
                position: Int64 = 0,
                downloaded: Int64 = 0,
                total: Int64 = 0,
                message: String = "") {
        self.type = type
        self.position = position
        self.downloaded = downloaded
        self.total = total
        self.message = message
    }
}

/// What to open — mirrors `soar::MediaSource`.
public struct MediaSource: Sendable, Equatable {
    public var uri: String
    /// When non-empty and `uri` is an http:// url, the backend caches
    /// downloaded data under this directory so the same url can be replayed
    /// offline. Ignored for local paths and https:// (mirrors backend.h).
    public var cacheDir: String

    public init(uri: String, cacheDir: String = "") {
        self.uri = uri
        self.cacheDir = cacheDir
    }
}

/// Track kinds — mirrors `soar::TrackType`.
public enum TrackType: String, Sendable, Equatable {
    case video
    case audio
    case subtitle
}

/// Metadata — mirrors `soar::MediaInfo` (duration in milliseconds;
/// the desktop struct also carries seekability and track lists, which the
/// iOS layer adopts when the core bridge lands).
public struct MediaInfo: Sendable, Equatable {
    public var duration: Int64
    public var seekable: Bool
    public var title: String

    public init(duration: Int64 = 0, seekable: Bool = false, title: String = "") {
        self.duration = duration
        self.seekable = seekable
        self.title = title
    }
}
