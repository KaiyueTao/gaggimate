#include "MuseRemote.h"

// The two Phase 2 shared instances (MuseRemote.h): MuseAvatarPlugin drives
// them (snapshot publication, telemetry timestamps, mailbox servicing),
// WebUIPlugin serves them (GET /api/muse/state, POST /api/muse/line).
MuseRemote::StatePublisher museStatePublisher;
MuseRemote::LineMailbox museLineMailbox;
