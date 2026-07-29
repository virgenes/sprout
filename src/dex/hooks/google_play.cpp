/* com.popcap.SexyAppFramework.GooglePlay.{GooglePlayConnect, GooglePlayAchievements,
 * GooglePlayLeaderboard} and com.popcap.SexyAppFramework.cloud.Cloud
 *
 * The online services. With google_play=1 the engine is told it IS connected,
 * which enables the in-game shop, achievements, and leaderboards -- all
 * answered locally with success / free prices / "done". With google_play=0
 * (the default) the engine treats "not signed in" as an ordinary state, which
 * is what a real device is in most of the time, and degrades gracefully.
 *
 * Every name below was taken from the decompiled classes.dex and cross-checked
 * against the string table of libPVZ2.so itself, because the previous version
 * of this file was written from memory of what the API "should" look like and
 * every single entry was dead: it registered `com/popcap/SexyAppFramework/Cloud`
 * and `.../GooglePlayConnect`, while the engine names
 * `com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect` -- these classes
 * live in subpackages -- and methods like Play_UnlockAchievement and Cloud_Sync
 * do not exist in any build. A hook keyed on a name nothing ever sends is
 * invisible: it does not warn, it just never runs. Verify a class path against
 * `strings libPVZ2.so` before adding one here.
 *
 * That correction changes no behaviour, and saying so is the point of this
 * file: these methods are void or boolean, so the unhooked default (nothing /
 * false) was already the answer they now give explicitly. They are written down
 * so the silence is a decision, and so a later change to the unhooked default
 * cannot quietly tell the engine it is online and leave it waiting on a
 * callback that will never arrive.
 *
 * GooglePlay/* is new in 4.5.2; 1.6 names none of these classes.
 */

#include <sprout/config.h>
#include <sprout/dex/dex.h>

namespace sprout {
namespace dex {
namespace {

constexpr const char *kConnect = "com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect";
constexpr const char *kAchievements = "com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements";
constexpr const char *kLeaderboard = "com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard";
constexpr const char *kClass = "com/popcap/SexyAppFramework/AndroidGameApp";

/* Cloud is reached differently: the engine never passes its class path to
 * FindClass, so it must be holding an object handed to it from Java and calling
 * GetObjectClass on it. Our GetObjectClass answers "java/lang/Object" for an
 * object it cannot identify, which is where these land -- the method name alone
 * is unambiguous enough to dispatch on. If the census ever shows them arriving
 * under a different class, move them; do not guess. */
constexpr const char *kCloud = "java/lang/Object";

/* When emulate_iap is enabled, report as connected. */
void connected(DexCall &d) { d.ret_bool(pvz2_config()->emulate_iap != 0); }

/* Fire-and-forget operations: accepted, nothing happens. */
void ignored(DexCall &d) { d.ret(0); }

/* The two Cloud getters that return a String. Java would answer null when
 * nothing is stored, and null is exactly the engine cannot take: it feeds
 * these into std::string(const char *), which aborts the guest with
 * "basic_string::_S_construct null not valid". An empty string carries the
 * same meaning -- no saved cloud identity -- without the abort. */
void empty_string(DexCall &d) { d.ret_string(""); }

}  // namespace

void register_google_play(HookTable &t) {
    t.add(kConnect, "Play_IsConnected", connected);
    t.add(kConnect, "Play_IsConnectedToGame", connected);
    t.add(kConnect, "Play_Connect", ignored);
    t.add(kConnect, "Play_Connect_Silent", ignored);
    t.add(kConnect, "Play_Disconnect", ignored);

    t.add(kAchievements, "Play_QueueAchievement_Percentage", ignored);
    t.add(kAchievements, "Play_ResetAchievements", ignored);
    t.add(kAchievements, "Play_ShowAchievementView", ignored);
    t.add(kAchievements, "Play_UnlockAchievement", ignored);

    t.add(kLeaderboard, "Play_SubmitScoreToLeaderboard", ignored);
    t.add(kLeaderboard, "Play_ShowLeaderboardView", ignored);
    t.add(kLeaderboard, "Play_ShowLeaderboardViewAll", ignored);

    t.add(kClass, "Native_getGooglePlayAPIKey", empty_string);

    t.add(kCloud, "Cloud_Connect", ignored);
    t.add(kCloud, "Cloud_attemptSilentSync", ignored);
    t.add(kCloud, "Cloud_SetPcpId", ignored);
    t.add(kCloud, "Cloud_SetAge", ignored);
    t.add(kCloud, "Cloud_GetPcpId", empty_string);
    t.add(kCloud, "Cloud_GetAge", empty_string);
}

}  // namespace dex
}  // namespace sprout
