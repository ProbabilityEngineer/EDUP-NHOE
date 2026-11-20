#pragma once

#include <cstdint>

namespace edtrace {
enum class ShipWeaponFamily { Beam, Multicannon, Pulse, Burst, Plasma, Repeater,
                              Missile, Torpedo, Flak, Fragment, Mine, MiningLaser, MiningCharge };
enum class ShipWeaponSize { Fighter, Small, Medium, Large, Huge };
struct ShipWeaponCues {
  std::uint32_t start = 0, stop = 0, shot = 0;
};
struct ShipWeaponProfile {
  ShipWeaponFamily family;
  ShipWeaponSize size;
  ShipWeaponCues cues[2];
};

struct ShipMotionLink {
  std::uint32_t firing, native_firing, motion;
};
// These firing variants play audio alone. Their alternate firing events
// already play the linked Motion cue, which contains only the authored effect.
inline constexpr ShipMotionLink kShipMotionLinks[] = {
    {252839122u, 252839123u, 1472761002u},    // Small fixed railgun
    {2249258127u, 2249258126u, 4102759559u},  // Medium fixed cannon
};

// LOD0 and LOD1 share each weapon's playback state.
inline constexpr ShipWeaponProfile kShipWeaponProfiles[] = {
    {ShipWeaponFamily::Torpedo, ShipWeaponSize::Large, {{491878982u, 0, 0}, {491878983u, 0, 0}}},
    {ShipWeaponFamily::Torpedo, ShipWeaponSize::Medium, {{1820060652u, 0, 0}, {1820060653u, 0, 0}}},
    {ShipWeaponFamily::Torpedo, ShipWeaponSize::Small, {{1239798714u, 0, 0}, {1239798715u, 0, 0}}},
    {ShipWeaponFamily::Missile, ShipWeaponSize::Large, {{2424068306u, 0, 0}, {2424068307u, 0, 0}}},
    {ShipWeaponFamily::Missile, ShipWeaponSize::Medium, {{788965072u, 0, 0}, {788965073u, 0, 0}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Large, {{3880196938u, 557938199u, 583355665u}, {3880196939u, 557938198u, 583355664u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Medium, {{1156031320u, 1226199895u, 55151373u}, {1156031321u, 1226199894u, 55151372u}}},
    {ShipWeaponFamily::Missile, ShipWeaponSize::Medium, {{1850222666u, 0, 0}, {1850222667u, 0, 0}}},
    {ShipWeaponFamily::Missile, ShipWeaponSize::Small, {{1615891120u, 0, 0}, {1615891121u, 0, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Fighter, {{143807216u, 3044296501u, 0}, {143807217u, 3044296500u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Huge, {{1584318588u, 3005386531u, 0}, {1584318589u, 3005386530u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Large, {{735294934u, 2125764459u, 0}, {735294935u, 2125764458u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Medium, {{4166670108u, 3191029699u, 0}, {4166670109u, 3191029698u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Small, {{2231715850u, 248667411u, 0}, {2231715851u, 248667410u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Small, {{2574303653u, 3850589006u, 0}, {2574303652u, 3850589007u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Fighter, {{178650940u, 3771426335u, 0}, {178650941u, 3771426334u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Huge, {{680023000u, 3857135361u, 0}, {680023001u, 3857135360u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Large, {{471305530u, 2406726609u, 0}, {471305531u, 2406726608u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Medium, {{4054225128u, 3037970381u, 0}, {4054225129u, 3037970380u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Small, {{2236770382u, 1110335685u, 0}, {2236770383u, 1110335684u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Large, {{4123255462u, 3080820517u, 0}, {4123255463u, 3080820516u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Medium, {{1358133324u, 2874951049u, 0}, {1358133325u, 2874951048u, 0}}},
    {ShipWeaponFamily::Beam, ShipWeaponSize::Small, {{576310938u, 1836434409u, 0}, {576310939u, 1836434408u, 0}}},
    {ShipWeaponFamily::Missile, ShipWeaponSize::Medium, {{698683114u, 0, 0}, {698683115u, 0, 0}}},
    {ShipWeaponFamily::Missile, ShipWeaponSize::Large, {{1341507730u, 0, 0}, {1341507731u, 0, 0}}},
    {ShipWeaponFamily::Missile, ShipWeaponSize::Medium, {{2864086672u, 0, 0}, {2864086673u, 0, 0}}},
    {ShipWeaponFamily::Missile, ShipWeaponSize::Medium, {{3770753721u, 0, 0}, {3770753720u, 0, 0}}},
    {ShipWeaponFamily::Missile, ShipWeaponSize::Small, {{2457615942u, 0, 0}, {2457615943u, 0, 0}}},
    {ShipWeaponFamily::Flak, ShipWeaponSize::Small, {{1977991022u, 0, 0}, {1977991023u, 0, 0}}},
    {ShipWeaponFamily::Flak, ShipWeaponSize::Medium, {{2274183642u, 0, 0}, {2274183643u, 0, 0}}},
    {ShipWeaponFamily::Plasma, ShipWeaponSize::Large, {{4229065718u, 0, 0}, {4229065719u, 0, 0}}},
    {ShipWeaponFamily::Plasma, ShipWeaponSize::Medium, {{3821497660u, 0, 0}, {3821497661u, 0, 0}}},
    {ShipWeaponFamily::Plasma, ShipWeaponSize::Small, {{2200547498u, 0, 0}, {2200547499u, 0, 0}}},
    {ShipWeaponFamily::Plasma, ShipWeaponSize::Fighter, {{4274045950u, 0, 0}, {4274045951u, 0, 0}}},
    {ShipWeaponFamily::Mine, ShipWeaponSize::Medium, {{2175702724u, 0, 0}, {2175702725u, 0, 0}}},
    {ShipWeaponFamily::Mine, ShipWeaponSize::Small, {{1720784898u, 0, 0}, {1720784899u, 0, 0}}},
    {ShipWeaponFamily::MiningCharge, ShipWeaponSize::Medium, {{339459373u, 0, 0}, {339459372u, 0, 0}}},
    {ShipWeaponFamily::MiningCharge, ShipWeaponSize::Small, {{2192180773u, 0, 0}, {2192180772u, 0, 0}}},
    {ShipWeaponFamily::MiningCharge, ShipWeaponSize::Medium, {{3726683348u, 0, 0}, {3726683349u, 0, 0}}},
    {ShipWeaponFamily::MiningCharge, ShipWeaponSize::Medium, {{3181949611u, 0, 0}, {3181949610u, 0, 0}}},
    {ShipWeaponFamily::MiningCharge, ShipWeaponSize::Small, {{544906891u, 0, 0}, {544906890u, 0, 0}}},
    {ShipWeaponFamily::MiningLaser, ShipWeaponSize::Medium, {{1265307891u, 38725400u, 0}, {1265307890u, 38725401u, 0}}},
    {ShipWeaponFamily::MiningLaser, ShipWeaponSize::Small, {{1707191203u, 3376436510u, 0}, {1707191202u, 3376436511u, 0}}},
    {ShipWeaponFamily::MiningLaser, ShipWeaponSize::Small, {{3780606550u, 863630477u, 0}, {3780606551u, 863630476u, 0}}},
    {ShipWeaponFamily::MiningCharge, ShipWeaponSize::Large, {{352412047u, 0, 0}, {352412046u, 0, 0}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Fighter, {{3633993616u, 104684145u, 1564565535u}, {3633993617u, 104684144u, 1564565534u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Huge, {{2502441116u, 2725761119u, 4178765057u}, {2502441117u, 2725761118u, 4178765056u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Large, {{2887973622u, 1845123391u, 3174475217u}, {2887973623u, 1845123390u, 3174475216u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Medium, {{232294972u, 61318287u, 3203014093u}, {232294973u, 61318286u, 3203014092u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Small, {{859455402u, 3091066935u, 1878084293u}, {859455403u, 3091066934u, 1878084292u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Small, {{3689013136u, 1059135839u, 2424297281u}, {3689013137u, 1059135838u, 2424297280u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Huge, {{3056208120u, 1210587797u, 1853915439u}, {3056208121u, 1210587796u, 1853915438u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Large, {{2929496922u, 2533983261u, 42446447u}, {2929496923u, 2533983260u, 42446446u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Medium, {{4275707784u, 1632716273u, 2821994463u}, {4275707785u, 1632716272u, 2821994462u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Small, {{1616298862u, 909378177u, 1489337831u}, {1616298863u, 909378176u, 1489337830u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Large, {{2810396486u, 1843371465u, 2616444403u}, {2810396487u, 1843371464u, 2616444402u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Medium, {{4174711532u, 4105507509u, 764072059u}, {4174711533u, 4105507508u, 764072058u}}},
    {ShipWeaponFamily::Multicannon, ShipWeaponSize::Small, {{3558316218u, 1479662109u, 2975453755u}, {3558316219u, 1479662108u, 2975453754u}}},
    {ShipWeaponFamily::Plasma, ShipWeaponSize::Huge, {{367820386u, 0, 0}, {367820387u, 0, 0}}},
    {ShipWeaponFamily::Plasma, ShipWeaponSize::Large, {{1025038092u, 0, 0}, {1025038093u, 0, 0}}},
    {ShipWeaponFamily::Plasma, ShipWeaponSize::Large, {{717087431u, 0, 0}, {717087430u, 0, 0}}},
    {ShipWeaponFamily::Plasma, ShipWeaponSize::Medium, {{1710255542u, 0, 0}, {1710255543u, 0, 0}}},
    {ShipWeaponFamily::Plasma, ShipWeaponSize::Medium, {{1544703058u, 0, 0}, {1544703059u, 0, 0}}},
    {ShipWeaponFamily::Repeater, ShipWeaponSize::Fighter, {{3798205069u, 0, 0}, {3798205068u, 0, 0}}},
    {ShipWeaponFamily::Repeater, ShipWeaponSize::Large, {{3188670861u, 0, 0}, {3188670860u, 0, 0}}},
    {ShipWeaponFamily::Repeater, ShipWeaponSize::Medium, {{2788806913u, 0, 0}, {2788806912u, 0, 0}}},
    {ShipWeaponFamily::Repeater, ShipWeaponSize::Small, {{3733008945u, 0, 0}, {3733008944u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Fighter, {{4204590363u, 0, 0}, {4204590362u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Huge, {{4202189421u, 0, 0}, {4202189420u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Large, {{3605154597u, 0, 0}, {3605154596u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Medium, {{3728187785u, 0, 0}, {3728187784u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Medium, {{666146134u, 0, 0}, {666146135u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Small, {{2360768489u, 0, 0}, {2360768488u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Fighter, {{3954470661u, 0, 0}, {3954470660u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Huge, {{782895859u, 0, 0}, {782895858u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Large, {{467698587u, 0, 0}, {467698586u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Medium, {{4054162963u, 0, 0}, {4054162962u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Small, {{1173023555u, 0, 0}, {1173023554u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Large, {{1348176199u, 0, 0}, {1348176198u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Medium, {{1872039847u, 0, 0}, {1872039846u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Small, {{2792849567u, 0, 0}, {2792849566u, 0, 0}}},
    {ShipWeaponFamily::Burst, ShipWeaponSize::Huge, {{1144479919u, 0, 0}, {1144479918u, 0, 0}}},
    {ShipWeaponFamily::Burst, ShipWeaponSize::Large, {{4050224367u, 0, 0}, {4050224366u, 0, 0}}},
    {ShipWeaponFamily::Burst, ShipWeaponSize::Medium, {{901309279u, 0, 0}, {901309278u, 0, 0}}},
    {ShipWeaponFamily::Burst, ShipWeaponSize::Small, {{1202148455u, 0, 0}, {1202148454u, 0, 0}}},
    {ShipWeaponFamily::Burst, ShipWeaponSize::Small, {{1434266238u, 0, 0}, {1434266239u, 0, 0}}},
    {ShipWeaponFamily::Burst, ShipWeaponSize::Huge, {{2679587493u, 0, 0}, {2679587492u, 0, 0}}},
    {ShipWeaponFamily::Burst, ShipWeaponSize::Large, {{1491874509u, 0, 0}, {1491874508u, 0, 0}}},
    {ShipWeaponFamily::Burst, ShipWeaponSize::Medium, {{1548317505u, 0, 0}, {1548317504u, 0, 0}}},
    {ShipWeaponFamily::Burst, ShipWeaponSize::Small, {{2990777457u, 0, 0}, {2990777456u, 0, 0}}},
    {ShipWeaponFamily::Burst, ShipWeaponSize::Large, {{4273195737u, 0, 0}, {4273195736u, 0, 0}}},
    {ShipWeaponFamily::Burst, ShipWeaponSize::Medium, {{1871252837u, 0, 0}, {1871252836u, 0, 0}}},
    {ShipWeaponFamily::Burst, ShipWeaponSize::Small, {{2456401197u, 0, 0}, {2456401196u, 0, 0}}},
    {ShipWeaponFamily::Pulse, ShipWeaponSize::Small, {{3183150238u, 0, 0}, {3183150239u, 0, 0}}},
    {ShipWeaponFamily::Fragment, ShipWeaponSize::Large, {{908382806u, 0, 0}, {908382807u, 0, 0}}},
    {ShipWeaponFamily::Fragment, ShipWeaponSize::Large, {{574000504u, 0, 0}, {574000505u, 0, 0}}},
    {ShipWeaponFamily::Fragment, ShipWeaponSize::Medium, {{3054122140u, 0, 0}, {3054122141u, 0, 0}}},
    {ShipWeaponFamily::Fragment, ShipWeaponSize::Small, {{2404803722u, 0, 0}, {2404803723u, 0, 0}}},
    {ShipWeaponFamily::Fragment, ShipWeaponSize::Large, {{3653724858u, 0, 0}, {3653724859u, 0, 0}}},
    {ShipWeaponFamily::Fragment, ShipWeaponSize::Medium, {{226509160u, 0, 0}, {226509161u, 0, 0}}},
    {ShipWeaponFamily::Fragment, ShipWeaponSize::Small, {{1124222414u, 0, 0}, {1124222415u, 0, 0}}},
    {ShipWeaponFamily::Fragment, ShipWeaponSize::Large, {{3010707494u, 0, 0}, {3010707495u, 0, 0}}},
    {ShipWeaponFamily::Fragment, ShipWeaponSize::Medium, {{1825384652u, 0, 0}, {1825384653u, 0, 0}}},
    {ShipWeaponFamily::Fragment, ShipWeaponSize::Small, {{3758730266u, 0, 0}, {3758730267u, 0, 0}}},
};
}  // namespace edtrace
