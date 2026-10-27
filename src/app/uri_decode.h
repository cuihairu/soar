// Launcher-sourced source strings into the form the rest of the app
// speaks: a file:// URI becomes its local path with percent-encoding
// undone; anything else is returned byte-for-byte. A desktop environment
// hands the opened file over as a URI (file:///home/user/a.mkv, from the
// packaged .desktop entry's %U field code) and drag-and-drop delivers the
// same text/uri-list entries, while the playback core and the UI store,
// display and resume from plain paths. Percent-encoding is a URI
// encoding, not a filename: a name with a space arrives as
// file:///home/user/My%20Video.mkv.

#ifndef SOAR_APP_URI_DECODE_H_
#define SOAR_APP_URI_DECODE_H_

#include <string>

namespace soar::app {

// Undo one launcher hop on `uri`. Only file:// is translated: plain and
// relative paths, http(s)://, asset://, magnet: and every other scheme
// come back unchanged, so applying it to a string that is already a path
// is a no-op and it is safe at every intake point.
//
// The authority is honoured: file:///abs/path and file://localhost/abs
// yield /abs/path, and a Windows URI (file:///C:/movie.mkv) yields
// C:/movie.mkv — the Windows form has no authority either. A non-local
// authority (file://host/share) is returned untouched: it is not a local
// path, so rewriting it would invent one. Percent-escapes that are not
// valid escapes (%ZZ, a trailing %) stay literal rather than being
// dropped, and '+' is not a space — only %XX is.
std::string decodeSourceUri(const std::string& uri);

}  // namespace soar::app

#endif  // SOAR_APP_URI_DECODE_H_
