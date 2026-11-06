// DSOUND.dll (ordinal imports). Audio is not implemented yet: no devices are reported and creation fails
// cleanly with DSERR_NODRIVER so the game continues without sound.
#include "rt.h"

#define D "DSOUND.dll"
using namespace rt;

static const uint32_t DSERR_NODRIVER = 0x88780078;

IMP_ID(D, "#1", DirectSoundCreate, 12) { if (ARG(1)) guest::store<uint32_t>(ARG(1), 0); SET(DSERR_NODRIVER); }
IMP_ID(D, "#3", DirectSoundEnumerateW, 8) { SET(0); }
IMP_ID(D, "#6", DirectSoundCaptureCreate, 12) { if (ARG(1)) guest::store<uint32_t>(ARG(1), 0); SET(DSERR_NODRIVER); }
IMP_ID(D, "#8", DirectSoundCaptureEnumerateW, 8) { SET(0); }
IMP_ID(D, "#9", GetDeviceID, 8)
{
	if (ARG(0) && ARG(1)) memcpy(guest::host(ARG(1)), guest::host(ARG(0)), 16);
	SET(0);
}
