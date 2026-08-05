#ifndef __fork_version_h__
#define __fork_version_h__

// The MRView++ fork version. This is deliberately separate from
// MRTRIX_BASE_VERSION in core/version.h, which tracks the upstream MRtrix3
// release this fork is based on.
//
// This is the single source of truth: the About box, the update check, the
// macOS Info.plist, the Windows installer and the Debian package all read it.
//
// Bump it in the same commit that creates the release tag; the tag must be
// "v" + this string, which CI enforces before publishing a release:
//   edit core/fork_version.h
//   git commit -am "release 3.0.7"
//   git tag v3.0.7
//   git push --follow-tags myfork perspective-fork

#define MRVIEWPP_VERSION "3.0.6"

#endif
