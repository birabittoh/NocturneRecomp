#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

// kernel/xam.cpp (or wherever your other XAM stubs live)

REXCVAR_DEFINE_STRING(user_name, "Player", "Game",
                      "Display name reported to the game as the signed-in "
                      "Xbox LIVE profile's username.");

enum XUSER_SIGNIN_STATE {
    eXUserSigninState_NotSignedIn      = 0,
    eXUserSigninState_SignedInLocally  = 1,
    eXUserSigninState_SignedInToLive   = 2,
};

uint32_t XamUserGetSigninState(uint32_t dwUserIndex)
{
    REXLOG_TRACE("XamUserGetSigninState called, userIndex={}", dwUserIndex);

    if (dwUserIndex == 0)
        return eXUserSigninState_SignedInToLive;

    return eXUserSigninState_NotSignedIn;
}


// XUSER_SIGNIN_INFO as the game reads it (40 bytes, big-endian guest layout).
// Fields are rex::be<> so writes land in the byte order the recompiled game
// loads them in. The game caches per-user Live state from this struct every
// frame (sub_82209B88); without it filled, UserSigninState stays 0 =
// NotSignedIn and every Xbox LIVE menu is gated behind the sign-in wall.
struct XUSER_SIGNIN_INFO {
    rex::be<uint64_t> xuid;               // 0x00
    rex::be<uint32_t> dwInfoFlags;        // 0x08 (bit1 = sponsored/guest account)
    rex::be<uint32_t> UserSigninState;    // 0x0C
    rex::be<uint32_t> dwGuestNumber;      // 0x10
    rex::be<uint32_t> dwSponsorUserIndex; // 0x14
    char              szUserName[16];     // 0x18
};
static_assert(sizeof(XUSER_SIGNIN_INFO) == 40, "XUSER_SIGNIN_INFO layout mismatch");

uint32_t XamUserGetSigninInfo(uint32_t dwUserIndex, uint32_t dwFlags, XUSER_SIGNIN_INFO* pInfo)
{
    REXLOG_TRACE("XamUserGetSigninInfo called, userIndex={}, flags={}", dwUserIndex, dwFlags);

    if (!pInfo)
        return 0x80070057; // E_INVALIDARG

    memset(pInfo, 0, sizeof(*pInfo));

    if (dwUserIndex != 0)
        return 0x00000525; // ERROR_NO_SUCH_USER

    // Signed-in-to-Live profile. dwInfoFlags bit1 = 0 so the game treats this as
    // a real (non-sponsored) account and gates privileges through
    // XamUserCheckPrivilege, which we already grant.
    pInfo->xuid            = 0xE000BABECAFE0001ull;
    pInfo->dwInfoFlags     = 0;
    pInfo->UserSigninState = eXUserSigninState_SignedInToLive;
    pInfo->dwGuestNumber   = 0;
    pInfo->dwSponsorUserIndex = 0;
    std::string userName = REXCVAR_GET(user_name);
    strncpy(pInfo->szUserName, userName.c_str(), sizeof(pInfo->szUserName) - 1);

    return 0;
}


uint32_t XUserCheckPrivilege(uint32_t dwUserIndex, uint32_t dwPrivilege, uint32_t* pfResult)
{
    REXLOG_TRACE("XUserCheckPrivilege called, userIndex={}, privilege={}", dwUserIndex, dwPrivilege);
    *pfResult = 1;
    uint32_t ret = 0;
    REXLOG_TRACE("XUserCheckPrivilege returning, pfResult={}, ret={}", *pfResult, ret);
    return ret;
}

REX_HOOK(__imp__XamUserCheckPrivilege, XUserCheckPrivilege);
REX_HOOK(__imp__XamUserGetSigninState, XamUserGetSigninState);
REX_HOOK(__imp__XamUserGetSigninInfo, XamUserGetSigninInfo);
