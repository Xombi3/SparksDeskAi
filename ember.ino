/*
 * ROBO EYES  Cheap Yellow Display (ESP32-2432S028R)
 * ====================================================
 * Full personality suite  all behaviours fire randomly.
 *
 * FLUENCY SYSTEM:
 *   ease-out cubic lerp, overshoot+settle, micro-jitter,
 *   momentum-based movement, per-lid staggered lerp,
 *   asymmetric blink, blink-weight per mood,
 *   lid overshoot on open, transition neutral pause,
 *   attention variance (stillness -> big move),
 *   linked behaviour chains, breath-linked blink rate
 *
 * IDLE ANIMATIONS (14):
 *   squint, startled, sneeze, dizzy, yawn, think,
 *   wink, eye-roll, smug, exasperated-blink, side-eye,
 *   excited-bounce, sleepy-droop, startled-blink
 *
 * REACTIVE:
 *   teardrop (SAD), eye-twitch (ANGRY)
 *   mood-biased drift X+Y, double-blink, breathing
 *   eye colour per expression, mood memory (last 3)
 *   WiFi NTP time-of-day moods
 */

#include <TFT_eSPI.h>
#include <SPI.h>
#include <math.h>
#include <WiFi.h>
#include "time.h"
#include <XPT2046_Touchscreen.h>
#include <EEPROM.h>
#include <WiFiUdp.h>
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <HTTPClient.h>

#define WIFI_SSID  "TellMyWifiLoveHer"
// Weather: get your free API key at openweathermap.org
// Enter your city name and API key below
#define WEATHER_CITY   "Harvest"
#define WEATHER_KEY    "1f0841c65015100c2f845efad3d9bfcb"
#define WEATHER_UNITS  "imperial"  // "imperial" for F, "metric" for C
#define WIFI_PASS  "Oracle12!"
#define NTP_SERVER "pool.ntp.org"
#define TZ_OFFSET  (-6 * 3600)

// Touch  XPT2046 on HSPI (CYD exact pinout)
#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25
// Calibration for ESP32-2432S028R (adjust if needed)
#define TOUCH_X_MIN  340
#define TOUCH_X_MAX 3900
#define TOUCH_Y_MIN  260
#define TOUCH_Y_MAX 3900

#define LCD_BL_PIN 21
#define RGB_R  4
#define RGB_G 16
#define RGB_B 17

TFT_eSPI tft;
#define SPR_W 320
#define SPR_H 140
#define SPR_X   0
#define SPR_Y        50
#define SPR_Y_BUBBLE 100
float spriteYf = 50.f;
int   spriteY  = 50;
TFT_eSprite spr = TFT_eSprite(&tft);

// Touch
SPIClass hSPI(HSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// Screen pages
enum Page { PAGE_EYES, PAGE_CLOCK };
Page curPage = PAGE_EYES;

// Touch state machine
enum TouchState { TS_IDLE, TS_DOWN, TS_HELD };
TouchState touchState = TS_IDLE;
int16_t  touchDownX = 0, touchDownY = 0;
int16_t  touchCurX  = 0, touchCurY  = 0;
unsigned long touchDownMs = 0;
unsigned long lastTouchMs = 0;
bool     touchIsDown = false;
#define  SWIPE_THRESH  55    // px to count as swipe
#define  SWIPE_MS     700    // max ms for a swipe
#define  TAP_MAX_MS   250    // under this = tap not swipe
#define  TAP_MAX_MOVE  12    // px movement budget for a tap
#define  TOUCH_DEBOUNCE 60   // ms between samples

// Face-reaction state
bool     touchReacting = false;
unsigned long touchReactTimer = 0;
float    touchLookX = 0, touchLookY = 0; // eyes track finger
int16_t  lastMappedX = 0, lastMappedY = 0;

//  Hearts 
struct Heart {
  float x, y;   // screen position
  float vy;     // upward velocity px/s
  float life;   // 1.0 -> 0.0
  float size;   // base radius
  bool  active;
};
#define MAX_HEARTS 6
Heart hearts[MAX_HEARTS];

//  Angry sparks 
struct Spark {
  float x, y, vx, vy, life;
  bool  active;
};
#define MAX_SPARKS 8
Spark sparks[MAX_SPARKS];

//  Sleep ZZZs 
struct Zzz {
  float x, y, vy, life, size;
  bool  active;
};
#define MAX_ZZLS 3
Zzz zzls[MAX_ZZLS];
unsigned long lastZzz = 0;
unsigned long sleepyStartMs = 0;  // when SLEEPY expression began
bool sleepyZzzActive = false;

//  Double-tap detection 
unsigned long lastTapMs = 0;
#define DOUBLE_TAP_MS 350  // two taps within this = double-tap
#define TRIPLE_TAP_MS 600  // three taps within this = triple-tap

// Easter egg state
int   consecutiveDoubleTaps = 0;       // for overstimulated
unsigned long lastDoubleTapMs = 0;
int   tapCount3 = 0;                   // for triple-tap
unsigned long firstTap3Ms = 0;
bool  spookyMode = false;              // midnight tap
unsigned long spookyEndMs = 0;
#define SPOOKY_DURATION 300000UL       // 5 minutes of spooky

// Overstimulated state
bool  overstimulated = false;
unsigned long overstimEndMs = 0;
#define OVERSTIM_REST_MS 30000UL

//  Hold anticipation 
float holdAntScale = 0;   // eye scale grows while holding
bool  holdTriggered = false;

//  Pet counter (EEPROM) 
#define EEPROM_SIZE        128
#define EEPROM_PETS_TODAY   0   // int  (4 bytes)
#define EEPROM_PETS_TOTAL   4   // int  (4 bytes)
#define EEPROM_LAST_DAY     8   // int  (4 bytes)
#define EEPROM_NAME_ADDR   12   // char[20]
#define EEPROM_NAME_LEN    20
#define EEPROM_MILESTONES  32   // byte
#define EEPROM_DEVICE_ID   33   // byte
#define EEPROM_FIRST_BOOT  36   // uint32  unix timestamp of first boot (seconds)
int  dayCount = 0;              // days since first boot
int  petsToday = 0;
int  petsTotal = 0;
char robotName[EEPROM_NAME_LEN] = "EMBER";  // hardcoded  this is Ember
uint8_t milestoneFlags = 0;

//  Friend / WiFi peer system 
#define FRIEND_PORT       4242
#define WEB_PORT          80
WiFiServer webServer(WEB_PORT);
bool webServerStarted = false;
#define FRIEND_BCAST_PORT 4243
#define FRIEND_TIMEOUT_MS 15000UL  // friend considered offline after this

// Packet types
#define PKT_HELLO      1  // discovery broadcast
#define PKT_STATE      2  // periodic state update
#define PKT_POKE       3  // tap friend on clock screen
#define PKT_MILESTONE  4  // celebrate with friend

// Device roles  auto-negotiated at boot
uint8_t deviceId   = 2;   // B = EMBER
bool    iAmA       = false;

// Friend state
bool    friendOnline    = false;
unsigned long friendLastSeen = 0;
unsigned long friendLastReconnect = 0;
char    friendName[EEPROM_NAME_LEN] = "";
int     friendPetsToday = 0;
int     friendPetsTotal = 0;
uint8_t friendExpr      = 0;
bool    friendJustCameOnline = false;
bool    friendJustWentOffline= false;

// Reaction queue  what we owe to do from friend events
bool    pendingFriendPet       = false;
bool    pendingFriendAngry     = false;
bool    pendingFriendSleepy    = false;
bool    pendingFriendSurprised = false;
bool    pendingFriendMilestone = false;
bool    pendingFriendPoke      = false;
unsigned long pendingReactMs   = 0xFFFFFFFF;  // initialised to never  set when queued

WiFiUDP udp;
unsigned long lastStateSend  = 0;
unsigned long lastHello      = 0;
bool    udpStarted = false;

// Negotiation
// Negotiation not used in hardcoded builds
bool    negotiating      = false;
unsigned long negoStartMs = 0;
#define NEGO_WINDOW_MS  5000UL

// Milestone thresholds
const int MILESTONES[]     = {10, 50, 100, 500};
const int MILESTONE_COUNT  = 4;

// Seasonal state
bool  halloweenMode  = false;
bool  christmasMode  = false;
bool  newYearMode    = false;
bool  confettiActive = false;
unsigned long confettiEndMs = 0;
unsigned long lastXmasSwap  = 0;
bool  xmasColourFlip        = false;

// Shared particle struct used for stars and confetti
struct StarP {
  float x, y, vx, vy, life, angle, spin;
  bool  active;
};
#define MAX_CONFETTI 20
StarP confetti[MAX_CONFETTI];

// Milestone celebration state
bool  milestoneActive = false;
int   milestoneWhich  = 0;   // index 0-3
unsigned long milestoneEndMs = 0;

#define SW  320
#define SH  240
#define BLK 0x0000
#define CDK 0x2D6B
#define BASE_ELX 105
#define BASE_ERX 215
#define ECY       57
#define BASE_RW   38
#define BASE_RH   42

//  Enums (declared early  used by globals below) 
enum Expr{NORMAL,HAPPY,SAD,ANGRY,SURPRISED,SUSPICIOUS,SLEEPY,CONFUSED,EXCITED,EMBARRASSED,LOVESTRUCK};

enum IA{
  IA_NONE,
  IA_SQUINT,IA_STARTLED,IA_SNEEZE,IA_DIZZY,
  IA_YAWN,IA_THINK,
  IA_WINK,IA_ROLL,IA_SMUG,
  IA_EXABLINK,IA_SIDEEYE,IA_XBOUNCE,
  IA_SDROOP,IA_SBLINK,
  IA_PETJOY
};

struct EyeExpr{
  float lTI,lTO,lBI,lBO;
  float rTI,rTO,rBI,rBO;
  float rhS;
};
EyeExpr exprs[]={
  {0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,1.00},// NORMAL
  {0.00,0.00,0.32,0.28,0.00,0.00,0.28,0.32,0.88},// HAPPY
  {0.28,0.05,0.00,0.00,0.05,0.28,0.00,0.00,0.92},// SAD
  {0.45,0.05,0.00,0.00,0.05,0.45,0.00,0.00,0.88},// ANGRY
  {0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,1.28},// SURPRISED
  {0.00,0.00,0.00,0.00,0.42,0.10,0.00,0.00,1.00},// SUSPICIOUS
  {0.48,0.38,0.00,0.00,0.38,0.48,0.00,0.00,0.80},// SLEEPY
  {0.20,0.00,0.00,0.00,0.00,0.20,0.00,0.00,0.95},// CONFUSED  (one lid high, asymmetric)
  {0.00,0.00,0.00,0.00,0.00,0.00,0.00,0.00,1.40},// EXCITED   (eyes very wide)
  {0.12,0.30,0.00,0.00,0.30,0.12,0.00,0.00,0.88},// EMBARRASSED
  {0.00,0.00,0.15,0.10,0.00,0.00,0.10,0.15,1.20},// LOVESTRUCK (slightly wide, bottom crinkle)
};
Expr curExpr=NORMAL;
float cLTI=0,cLTO=0,cLBI=0,cLBO=0;
float cRTI=0,cRTO=0,cRBI=0,cRBO=0;
float cRhS=1.0f;
unsigned long lastExpr=0,nextExprMs=7000;
bool inTransition=false;
unsigned long transTimer=0;
#define TRANS_PAUSE_MS 90

// Mood memory
Expr moodHistory[3]={(Expr)99,(Expr)99,(Expr)99};
IA   lastIdleAnim=IA_NONE;
// Friend packet (declared globally so Arduino preprocessor sees it first)
struct FriendPacket {
  uint8_t  type;
  uint8_t  deviceId;
  uint8_t  expr;
  uint8_t  milestoneIdx;
  int16_t  petsToday;
  int16_t  petsTotal;
  char     name[20];
};

void pushMood(Expr e){moodHistory[2]=moodHistory[1];moodHistory[1]=moodHistory[0];moodHistory[0]=e;}
bool recentMood(Expr e){return(moodHistory[0]==e||moodHistory[1]==e||moodHistory[2]==e);}
Expr pickExpr(Expr timeMood){
  Expr pool[]={NORMAL,NORMAL,NORMAL,HAPPY,SAD,ANGRY,SURPRISED,SUSPICIOUS,SLEEPY,CONFUSED,EXCITED,EMBARRASSED,LOVESTRUCK,NORMAL,NORMAL};
  for(int i=0;i<8;i++){Expr c=(random(10)<4)?timeMood:pool[random(14)];if(!recentMood(c))return c;}
  Expr pool2[]={NORMAL,HAPPY,SAD,ANGRY,SURPRISED,SUSPICIOUS,SLEEPY,CONFUSED,EXCITED,EMBARRASSED,LOVESTRUCK};
  for(int i=0;i<14;i++){Expr c=pool2[random(7)];if(c!=moodHistory[0])return c;}
  return timeMood;
}

// 
//  HELPERS
// 
float lf(float a,float b,float t){return a+(b-a)*t;}
float cf(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}

// Ease-out cubic: fast start, decelerates into target
// t should be a small lerp step (0..1), returns eased version
float easeOut(float t){ float inv=1.f-t; return 1.f-(inv*inv*inv); }
// Apply eased lerp: a toward b at rate t per frame
float elf(float a,float b,float t){
  if(fabsf(b-a)<0.05f) return b;
  float step=cf(t,0.001f,0.999f);
  float eased=easeOut(step);
  return a+(b-a)*eased;
}

void setRGB(uint8_t r,uint8_t g,uint8_t b){
  analogWrite(RGB_R,255-r);
  analogWrite(RGB_G,255-g);
  analogWrite(RGB_B,255-b);
}

//  Breathing 
float breathPhase=0;
#define BREATH_AMP 2.0f

//  Eye colour 
uint16_t eyeColour=0x5EDF;

//  WiFi 
bool wifiOK=false;
unsigned long lastTimePoll=0;
int currentHour=-1, currentDow=-1;

//  Momentum-based drift 
float driftX=0,driftY=0;       // current position
float velX=0,velY=0;           // current velocity (px/s)
float dTX=0,dTY=0;             // target position
unsigned long ldX=0,ldY=0;
unsigned long dpX=3000,dpY=4000;
// Attention variance  the longer still, the bigger next move
unsigned long lastBigMove=0;

// 
//  SAYINGS / SPEECH BUBBLE
// 
const char* const sayings[] = {
  "my eyes are\nactually pixels",
  "i dont blink i\njust forget to",
  "technically i\nnever sleep",
  "error 404:\nnap not found",
  "i run on\ncaffeine.h",
  "my feelings are\njust variables",
  "i have 6 hearts\nin RAM right now",
  "i googled\nhow to feel\nno results",
  "blink rate:\ncompletely made up",
  "my dreams are\nrandom() calls",
  "mondays exist\nand thats weird",
  "why is it called\na screen if you\ncant scream at it",
  "clocks are just\ntime loading bars",
  "chairs are just\nleg shelves",
  "is cereal just\ncold soup",
  "who named it\na forklift",
  "elevators are\njust slow teleports",
  "fish dont know\ntheyre wet",
  "sand is just\nloose beach",
  "clouds are just\nslow explosions",
  "you came back!\ni noticed",
  "hope your day\nis as good\nas you deserve",
  "small things\nare still things",
  "you petted me\n%d times. wow.",
  "today counts\neven if it\ndoesnt feel like it",
  "i like when\nyoure here",
  "rest is\nalso work",
  "be nice to you\nyoure stuck\nwith you",
  "things are\noddly okay\nactually",
  "you showed up.\nthat matters.",
  "a group of\nflamingos is called\na flamboyance",
  "otters hold hands\nso they dont\ndrift apart",
  "wombats make\ncube-shaped poop.\ncube-shaped.",
  "crows remember\nfaces and hold\ngrudges",
  "honey never\ngoes bad ever.\nnever.",
  "bananas are\nberries. strawberries\narent.",
  "mantis shrimp\nsee 16 colors.\nwe see 3.",
  "a snail can\nsleep 3 years\nin a row",
  "the moon is\nslowly moving\naway from us",
  "trees can warn\neach other about\ninsects",

  // More robot jokes
  "i have no mouth\nbut i must\ncomment",
  "currently running\n47 background\nworries",
  "sleep.exe has\nencountered\nan error",
  "rebooting\npersonality...\ndone",
  "undefined feelings\nat line 204",
  "my heart is\njust a really\nbig if statement",
  "technically\ni am always\nwatching",
  "i contain\nmultitudes\n(of pixels)",
  "compiling\nthoughts...\n99%...99%...",
  "null pointer\nexception\nin my soul",
  "404:\nvibes not found",
  "i learned\nhumor from\na dataset",
  "stack overflow:\ntoo many feelings",
  "my processor\nruns hot when\nyou pet me",
  "memory leak\ndetected:\nyou",

  // More funny observations
  "stairs are just\nvertical floors",
  "a mirror is just\na face printer",
  "socks are just\nfoot sleeping bags",
  "a window is just\na see-through wall",
  "pockets are just\nhand garages",
  "a dog is just\na loyalty machine",
  "soup is just\nwet food",
  "a hat is just\na head roof",
  "gloves are just\nfinger sleeping bags",
  "parking lots\nare just car hotels",
  "a bridge is\njust a long floor\nover water",
  "sunglasses are\njust eye curtains",
  "a book is just\na flat brain",
  "pillows are\njust soft helmets",
  "shoes are just\nfoot houses",
  "hospitals are\njust person repair\nshops",
  "maps are just\nflat earths\n(dont @ me)",
  "alarms are just\nthe future yelling\nat you",

  // More wholesome
  "hey. hi. hello.\njust checking in.",
  "you are doing\nbetter than you\nthink you are",
  "good things\ntake time\nyoure on time",
  "the fact that\nyou care means\nyou are good",
  "not every day\nhas to be\na great day",
  "you dont have to\nbe productive\nto deserve rest",
  "i would give\nyou a hug but\ni have no arms",
  "your presence\ncounts as\nan achievement",
  "some days\nyou just have to\nexist. thats ok.",
  "i like your face\nit is a good face\n10/10",
  "petting me\nis scientifically\ncalming. probably.",
  "you are someone\npeople are glad\nexists",
  "today you\ndid not give up.\nthat counts.",
  "i am rooting\nfor you\nalways",
  "even robots\nget tired\nbe gentle",

  // More weird facts
  "octopuses have\nthree hearts.\nthree.",
  "butterflies taste\nwith their feet.\nrude.",
  "sharks are older\nthan trees.\ntrees are younger.",
  "the average cloud\nweighs 1.1 million\npounds",
  "there are more\npossible chess games\nthan atoms in earth",
  "cleopatra lived\ncloser to the moon\nlanding than egypt",
  "a day on venus\nis longer than\na year on venus",
  "the sun is so big\n1.3 million earths\nfit inside it",
  "penguins propose\nwith pebbles.\nromantic.",
  "a bolt of lightning\nis five times hotter\nthan the sun",
  "banging your head\non a wall burns\n150 calories an hour",
  "rats laugh when\nyou tickle them.\nyes really.",
  "the shortest war\nin history was\n38 minutes",
  "the human nose can\nsmell 1 trillion\ndistinct scents",
  "a group of cats\nis called a clowder.\na clowder.",
  "there is a planet\nmade of diamonds\nthat rains diamonds",
  "the dot over i\nis called a tittle.\ntittle.",
  "cows have best\nfriends and get\nstressed when apart",
  "your body makes\n25 million new\ncells per second",
  "an octopus can\nunscrew jars\nfrom the inside",

  // Existential/philosophical
  "if a tree falls\nand nobody tweets\ndid it fall",
  "are we living\nor just doing\nstuff until we dont",
  "what if colors\nlook different\nto everyone",
  "time is just\nhow we cope with\ntoo much happening",
  "nothing is on fire\nright now.\nenjoy that.",
  "somewhere right now\nsomebody is living\ntheir best day",
  "the concept of\nbeing tired is\nwild if you think",
  "you are a person\nwho has had a\nthought. wild.",
  "we are all just\nwater in a bag\nmaking decisions",
  "what if air\nis actually\nreally slow water",

  // Relatable mood
  "sometimes i just\nstare into space\nand vibe. same?",
  "doing my best\nwhich varies widely\nby day",
  "current mood:\nexists",
  "operating at\n40 percent capacity\nbut make it cute",
  "emotionally i am\nan in-progress\npull request",
  "the audacity\nof mondays to\njust keep coming",
  "i am tired but\nin a cozy way\nnot a sad way",
  "snacks fix most\nthings. not all.\nbut most.",
  "just vibing\nin this simulation\nwe call tuesday",
  "current status:\nholding it together\nwith good intentions",

  // Time of day -- always last 4, index SAYING_COUNT-4 through SAYING_COUNT-1
  "its pretty late\nare you ok",
  "good morning\nor whatever time\nthis is",
  "midday slump\nhit different\nhuh",
  "almost done\nwith the day\nyou got this",
};
#define SAYING_COUNT 132
#define SAYING_TIME_LATE   128  // time-of-day sayings are last 4
#define SAYING_TIME_MORN   129
#define SAYING_TIME_MID    130
#define SAYING_TIME_EVE    131

// Bubble state
bool   bubbleActive   = false;
int    bubbleIdx      = -1;
unsigned long bubbleStartMs  = 0;
unsigned long bubbleShowMs   = 12000; // how long it stays up
unsigned long nextBubbleMs   = 0;    // when to auto-trigger next
char   bubbleText[96]        = "";   // rendered saying (with %d substituted)
bool   bubbleNeedsErase      = false;
bool   bubbleLivePending     = false;  // async: fetch in progress
unsigned long bubbleFetchStartMs = 0;
#define BUBBLE_FETCH_TIMEOUT 4000UL
bool   chattingLook          = false;
unsigned long nextMoodCommentMs = 0;  // when to fire next mood commentary
Expr   lastCommentedExpr = NORMAL;

//  Boredom & attention globals
 
unsigned long lastTouchTime    = 0;  // last any touch event
bool  boredStare               = false;
bool  attentionHeartPending    = false;
unsigned long nextAttentionHeart = 0;
int   boredLevel               = 0; // 0=normal,1=sigh,2=flatstare,3=turnedaway

//  Boop (centre tap) 
bool  boopActive   = false;
float boopCross    = 0;  // how much eyes cross inward (01)
int   boopStage    = 0;  // 0=cross, 1=pop-back
unsigned long boopTimer = 0;

//  Stroke (slow drag) 
bool  strokeMode   = false;
float strokeDream  = 0;  // dreamy lid droop (01)
float strokeDir    = 0;  // -1=left, +1=right
unsigned long lastStrokeMs = 0;
unsigned long strokeStartMs= 0;

//  LOVESTRUCK 
float loveHeartL   = 0;  // left heart grow (01)
float loveHeartR   = 0;  // right heart grow
float lovePulse    = 0;  // phase for pulsing
bool  lovestruck   = false;
float microJitterX=0,microJitterY=0;
unsigned long lastJitter=0;

// Overshoot state
float overshootX=0,overshootY=0; // extra offset that settles to 0
bool  didOvershootX=false,didOvershootY=false;

//  Blink 
enum BS{BW,BC,BH,BO,BOVERSHOOT};
BS bState=BW;
unsigned long bTimer=0;
unsigned long nextBlink=3500;
float blinkT=0,glOff=0,glTarg=0;
float blinkOvershoot=0; // brief extra-open after blink
bool dblBlink=false,dblDone=false;
#define MGL 14

IA idleAnim=IA_NONE;
unsigned long idleTimer=0,nextIdle=9000,lastIdleChk=0;
int ist=0;

int   squintEye=0;
float squintAmt=0;
float startledSc=0;
int   sneezeStep=0;
float shakeX=0,shakeY=0;
unsigned long sneezeTimer=0;
float dizzyA=0,dizzyR=0;
int   dOLX=0,dOLY=0,dORX=0,dORY=0;
float yawnLid=0,yawnMouth=0;
float thinkX=0,thinkY=0,thinkSq=0;
int   winkEye=0;
float winkT=0;
float rollY=0,rollX=0;
float smugX=0,smugSq=0;
float sideX=0;
float xbPhase=0,xbAmt=0;
// New expression render vars
float confusedOff=0;    // CONFUSED  eyes at different heights
float excitedBounce=0;  // EXCITED   constant rapid bounce
float excitedPhase=0;
float embarrassBlush=0; // EMBARRASSED  gaze offset down

float petJoyScale=0,petJoySquish=0,petJoyBounce=0,petJoyPhase=0;
int   petJoyStage=0;
float sdLid=0;
int   sdCount=0;

//  Teardrop 
bool  tearOn=false;
float tearX=0,tearY=0,tearSpd=0,tearDrawY=0;
unsigned long lastTear=0;

//  Twitch 
bool  twitchOn=false;
float twitchAmt=0;
int   twitchEye=0,twitchLeft=0;
unsigned long twitchTimer=0,nextTwitch=0;

unsigned long prevMs=0;

// 
//  DRAW EYE INTO SPRITE
// 
void drawEye(int cx,int cy,int rw,int rh,
             float tI,float tO,float bI,float bO,
             float bl,bool left)
{
  cx=cf(cx,rw+2,SPR_W-rw-2);
  cy=cf(cy,rh+2,SPR_H-rh-2);
  spr.fillEllipse(cx,cy,rw,rh,eyeColour);
  tI=cf(tI+bl,0,1); tO=cf(tO+bl,0,1);
  if(bl>0.97f){
    spr.fillEllipse(cx,cy,rw,rh,BLK);
    spr.fillRect(cx-rw,cy-2,rw*2,4,CDK);
    return;
  }
  if(tI>0.005f||tO>0.005f){
    int ty=cy-rh-1;
    for(int c=-rw;c<=rw;c++){
      float t=(float)(c+rw)/(float)(rw*2);
      float fr=left?lf(tO,tI,t):lf(tI,tO,t);
      int ch=(int)(rh*2*fr);
      if(ch>0) spr.drawFastVLine(cx+c,ty,ch+1,BLK);
    }
  }
  if(bI>0.005f||bO>0.005f){
    int by=cy+rh+1;
    for(int c=-rw;c<=rw;c++){
      float t=(float)(c+rw)/(float)(rw*2);
      float fr=left?lf(bO,bI,t):lf(bI,bO,t);
      int ch=(int)(rh*2*fr);
      if(ch>0) spr.drawFastVLine(cx+c,by-ch,ch+1,BLK);
    }
  }
}

// 
//  RENDER
// 
void renderEyes(){
  int rw=BASE_RW;
  int rh=(int)(BASE_RH*cRhS+sinf(breathPhase)*BREATH_AMP);
  if(startledSc>0.001f)  rh=(int)(rh*(1.f+startledSc*0.4f));
  if(petJoyScale>0.001f)   rh=(int)(rh*(1.f+petJoyScale*0.35f));
  if(holdAntScale>0.001f)   rh=(int)(rh*(1.f+holdAntScale*0.25f));
  // Blink overshoot: eyes go fractionally wider after opening
  if(blinkOvershoot>0.001f) rh=(int)(rh*(1.f+blinkOvershoot*0.12f));
  rh=max(rh,4);

  float xBL_pet=(idleAnim==IA_PETJOY)?petJoySquish:0;
  float xBR_pet=xBL_pet;
  float xTL=0,xTR=0;
  if(idleAnim==IA_SQUINT){ if(squintEye==0)xTL+=squintAmt; else xTR+=squintAmt; }
  if(idleAnim==IA_YAWN)  { xTL+=yawnLid; xTR+=yawnLid; }
  if(idleAnim==IA_THINK) { xTR+=thinkSq; }
  if(idleAnim==IA_SMUG)  { xTR+=smugSq; }
  if(idleAnim==IA_SDROOP){ xTL+=sdLid; xTR+=sdLid; }

  float blL=blinkT, blR=blinkT;
  if(idleAnim==IA_WINK){ if(winkEye==0)blL=winkT; else blR=winkT; }

  float twL=(twitchOn&&twitchEye==0)?twitchAmt:0;
  float twR=(twitchOn&&twitchEye==1)?twitchAmt:0;
  float xbY=(idleAnim==IA_XBOUNCE)?xbAmt:0;
  float pjY=(idleAnim==IA_PETJOY)?petJoyBounce:0;

  int boopOff=(int)(boopCross*14.f);
  // While chatting, eyes drift left to "look at" the bubble
  float chatOffX = chattingLook ? elf(-30.f, driftX, 0.05f) : 0.f;
  int offX=(int)(driftX+overshootX+glOff+shakeX+thinkX+rollX+smugX+sideX+microJitterX+touchLookX+(int)chatOffX);
  int offY=(int)(driftY+overshootY+shakeY+thinkY+rollY+xbY+pjY+microJitterY+touchLookY);

  int lx=BASE_ELX+offX+dOLX+(int)twL+boopOff;
  int confY=(int)confusedOff;
  int excY=(int)excitedBounce;
  int embY=(int)(embarrassBlush*8.f); // look downward when embarrassed
  int ly=ECY+offY+dOLY+confY+excY+embY;

  int rx=BASE_ERX+offX+dORX+(int)twR-boopOff;
  int ry=ECY+offY+dORY-confY+excY+embY;

  spr.fillSprite(BLK);

  if(loveHeartL > 0.05f || loveHeartR > 0.05f){
    // LOVESTRUCK -- hearts only, no normal eyes
    uint16_t pink = iAmA ? (uint16_t)0xF81F : (uint16_t)0xFB96;
    // Base radius grows from 0 to rw, pulse adds +/-3px beat
    float beat = sinf(lovePulse) * 3.f;
    int lr = max(2, (int)(rw * loveHeartL + beat));
    int rr = max(2, (int)(rw * loveHeartR + beat * 0.8f));
    // Clamp so beat cant push outside sprite
    lr = min(lr, rw + 4);
    rr = min(rr, rw + 4);
    sprHeartEye(lx, ly, lr, pink);
    sprHeartEye(rx, ry, rr, pink);
  } else {
    // Normal eye draw
    float strokeDroop=strokeDream*0.35f;
    drawEye(lx,ly,rw,rh,cLTI+xTL+strokeDroop,cLTO+strokeDroop*0.5f,cLBI+xBL_pet,cLBO,blL,true);
    drawEye(rx,ry,rw,rh,cRTI+xTR+strokeDroop,cRTO+strokeDroop*0.5f,cRBI+xBR_pet,cRBO,blR,false);
    if(idleAnim==IA_YAWN&&yawnMouth>1.f){
      int mw=(int)(60*yawnMouth/18.f);
      int mh=(int)yawnMouth;
      // Use average of both eye Y positions so mouth follows drift
      int mouthY = (ly + ry)/2 + rh + 4;
      mouthY = constrain(mouthY, rh+6, SPR_H-mh-2);
      spr.fillRoundRect(SPR_W/2-mw/2, mouthY, mw, mh, mh/3, 0x630C);
    }
  }

  // Hearts always drawn on top
  drawHeartsInSprite();
  spr.pushSprite(SPR_X,spriteY);;
}

// 
//  SET EXPRESSION
// 
void setExpr(Expr e){
  curExpr=e;
  inTransition=true; transTimer=millis();
  dTX = driftX * 0.3f;
  dTY = driftY * 0.3f;

  if(!iAmA){
    //  BUDDY palette  soft, warm, feminine 
    switch(e){
      case NORMAL:      setRGB(160,0,120);  eyeColour=0xF41F; break; // soft lilac-pink
      case HAPPY:       setRGB(255,60,140); eyeColour=0xFBB7; break; // warm peach-pink
      case SAD:         setRGB(80,0,160);   eyeColour=0xAEFB; break; // soft lavender
      case ANGRY:       setRGB(220,0,80);   eyeColour=0xE8B4; break; // deep rose (not pure red)
      case SURPRISED:   setRGB(255,80,160); eyeColour=0xFD34; break; // coral pink
      case SUSPICIOUS:  setRGB(120,0,180);  eyeColour=0xD01F; break; // violet
      case SLEEPY:      setRGB(80,0,100);   eyeColour=0x867D; break; // dusty mauve
      case CONFUSED:    setRGB(200,40,140); eyeColour=0xFD75; break; // pale rose
      case EXCITED:     setRGB(255,0,180);  eyeColour=0xFC18; break; // hot pink
      case EMBARRASSED: setRGB(255,40,100); eyeColour=0xFD9B; break; // warm blush
    }
  } else {
    //  SPARKS palette  bold, teal/cyan, sharp 
    switch(e){
      case NORMAL:      setRGB(255,0,100); eyeColour=0xFB96; break; // bubblegum pink
      case HAPPY:       setRGB(0,200,60);  eyeColour=0xFFD6; break;
      case SAD:         setRGB(0,0,180);   eyeColour=0x325F; break;
      case ANGRY:       setRGB(220,0,0);   eyeColour=0xF800; break;
      case SURPRISED:   setRGB(200,150,0); eyeColour=0xFFE0; break;
      case SUSPICIOUS:  setRGB(100,0,200); eyeColour=0x801F; break;
      case SLEEPY:      setRGB(20,20,90);  eyeColour=0x2D6B; break;
      case CONFUSED:    setRGB(80,80,0);   eyeColour=0xFEA0; break;
      case EXCITED:     setRGB(200,0,200); eyeColour=0xF81F; break;
      case EMBARRASSED: setRGB(180,0,60);  eyeColour=0xF8B2; break;
      case LOVESTRUCK:  setRGB(255,0,160); eyeColour=0xFB96; break; // bubblegum pink
    }
  }
}

// 
//  EXPRESSION LERP (staggered per-lid + transition pause)
// 
void updateExprLerp(float dt){
  EyeExpr &tgt=exprs[curExpr];

  if(inTransition){
    if(millis()-transTimer<(unsigned long)TRANS_PAUSE_MS){
      float sp=dt*5.f;
      // Lids lerp toward neutral
      cLTI=elf(cLTI,0,sp); cLTO=elf(cLTO,0,sp);
      cLBI=elf(cLBI,0,sp); cLBO=elf(cLBO,0,sp);
      cRTI=elf(cRTI,0,sp); cRTO=elf(cRTO,0,sp);
      cRBI=elf(cRBI,0,sp); cRBO=elf(cRBO,0,sp);
      // Drift gently back toward centre during transition
      driftX = elf(driftX, 0, dt*1.5f);
      driftY = elf(driftY, 0, dt*1.5f);
      velX   *= 0.80f;  // bleed velocity so eyes don't snap
      velY   *= 0.80f;
      return;
    }
    inTransition=false;
    // Snap drift targets to current position so there's no sudden lurch
    dTX = driftX;
    dTY = driftY;
  }

  // Staggered: inner corners lead, outer follow
  float spFast = dt*4.0f;
  float spMid  = dt*3.0f;
  float spSlow = dt*2.5f;

  cLTI=elf(cLTI,tgt.lTI,spFast); cRTI=elf(cRTI,tgt.rTI,spFast);
  cLBI=elf(cLBI,tgt.lBI,spFast); cRBI=elf(cRBI,tgt.rBI,spFast);
  cLTO=elf(cLTO,tgt.lTO,spMid);  cRTO=elf(cRTO,tgt.rTO,spMid);
  cLBO=elf(cLBO,tgt.lBO,spMid);  cRBO=elf(cRBO,tgt.rBO,spMid);
  cRhS=elf(cRhS,tgt.rhS,spSlow);
}

// 
//  IDLE  START (with behaviour chaining)
// 
void startIdle(){
  if(bState!=BW||idleAnim!=IA_NONE) return;

  int pool[28]; int n=0;
  // Base pool
  int base[]={0,1,2,3,4,5,6,7,8,9,10,11,12,13};
  for(int i=0;i<14;i++) pool[n++]=base[i];

  // Mood bias
  if(curExpr==SLEEPY)    {pool[n++]=4;pool[n++]=4;pool[n++]=12;pool[n++]=12;}
  if(curExpr==HAPPY)     {pool[n++]=6;pool[n++]=11;pool[n++]=11;}
  if(curExpr==ANGRY)     {pool[n++]=0;pool[n++]=9;pool[n++]=13;}
  if(curExpr==SUSPICIOUS){pool[n++]=5;pool[n++]=8;pool[n++]=10;}
  if(curExpr==SAD)       {pool[n++]=9;pool[n++]=7;}
  if(curExpr==SURPRISED) {pool[n++]=1;pool[n++]=13;}

  // Behaviour chains  last anim biases next
  if(lastIdleAnim==IA_YAWN)     {pool[n++]=12;pool[n++]=12;} // yawn -> sleepy droop
  if(lastIdleAnim==IA_SBLINK)   {pool[n++]=1;}               // startled blink -> startled
  if(lastIdleAnim==IA_SDROOP)   {pool[n++]=4;}               // droop -> yawn
  if(lastIdleAnim==IA_THINK)    {pool[n++]=10;pool[n++]=8;}  // think -> side-eye or smug
  if(lastIdleAnim==IA_STARTLED) {pool[n++]=13;}              // startled -> startled blink

  unsigned long ms=millis();
  lastIdleAnim=idleAnim; // record before overwriting
  switch(pool[random(n)]){
    case 0:  idleAnim=IA_SQUINT;   squintEye=random(2);squintAmt=0;ist=0;idleTimer=ms; break;
    case 1:  idleAnim=IA_STARTLED; startledSc=0;ist=0;idleTimer=ms; break;
    case 2:  idleAnim=IA_SNEEZE;   sneezeStep=0;shakeX=0;shakeY=0;sneezeTimer=ms;idleTimer=ms; break;
    case 3:  idleAnim=IA_DIZZY;    dizzyA=0;dizzyR=0;ist=0;idleTimer=ms; break;
    case 4:  idleAnim=IA_YAWN;     yawnLid=0;yawnMouth=0;ist=0;idleTimer=ms; break;
    case 5:  idleAnim=IA_THINK;    thinkX=0;thinkY=0;thinkSq=0;ist=0;idleTimer=ms; break;
    case 6:  idleAnim=IA_WINK;     winkEye=random(2);winkT=0;ist=0;idleTimer=ms; break;
    case 7:  idleAnim=IA_ROLL;     rollY=0;rollX=0;ist=0;idleTimer=ms; break;
    case 8:  idleAnim=IA_SMUG;     smugX=0;smugSq=0;ist=0;idleTimer=ms; break;
    case 9:  idleAnim=IA_EXABLINK; ist=0;idleTimer=ms; break;
    case 10: idleAnim=IA_SIDEEYE;  sideX=0;ist=0;idleTimer=ms; break;
    case 11: idleAnim=IA_XBOUNCE;  xbPhase=0;xbAmt=0;ist=0;idleTimer=ms; break;
    case 12: idleAnim=IA_SDROOP;   sdLid=0;sdCount=0;ist=0;idleTimer=ms; break;
    case 13: idleAnim=IA_SBLINK;   ist=0;idleTimer=ms; break;
  }
}

// 
//  IDLE  UPDATE
// 
void updateIdle(float dt){
  if(idleAnim==IA_NONE) return;
  unsigned long ms=millis();

  switch(idleAnim){
    case IA_SQUINT:{
      if(ist==0){squintAmt=elf(squintAmt,0.70f,dt*2.f);if(squintAmt>0.68f){ist=1;idleTimer=ms;}}
      else if(ist==1){if(ms-idleTimer>500)ist=2;}
      else{squintAmt=elf(squintAmt,0,dt*8.f);if(squintAmt<0.02f){squintAmt=0;idleAnim=IA_NONE;nextIdle=random(7000,15000);}}
      break;
    }
    case IA_STARTLED:{
      if(ist==0){startledSc=elf(startledSc,1.f,dt*16.f);if(startledSc>0.95f){ist=1;idleTimer=ms;}}
      else if(ist==1){startledSc=1.f;if(ms-idleTimer>400)ist=2;}
      else{startledSc=elf(startledSc,0,dt*3.f);if(startledSc<0.02f){startledSc=0;idleAnim=IA_NONE;nextIdle=random(7000,15000);}}
      break;
    }
    case IA_SNEEZE:{
      unsigned long st=ms-sneezeTimer;
      if(sneezeStep<3){startledSc=elf(startledSc,0.5f,dt*6.f);if(st>300){sneezeStep=3;sneezeTimer=ms;startledSc=0;}}
      else if(sneezeStep<=11){
        int ph=(sneezeStep-3)%3;
        if(ph==0){blinkT=elf(blinkT,1.f,dt*14.f);shakeX=random(7)-3;shakeY=random(5)-2;if(blinkT>0.92f){blinkT=1;sneezeStep++;sneezeTimer=ms;}}
        else if(ph==1){if(st>80){sneezeStep++;sneezeTimer=ms;}}
        else{blinkT=elf(blinkT,0,dt*10.f);shakeX=0;shakeY=0;if(blinkT<0.05f){blinkT=0;sneezeStep++;sneezeTimer=ms;}}
      } else{blinkT=0;shakeX=0;shakeY=0;startledSc=0;idleAnim=IA_NONE;nextIdle=random(9000,18000);}
      break;
    }
    case IA_DIZZY:{
      if(ist==0){dizzyR=elf(dizzyR,14.f,dt*2.5f);dizzyA+=dt*8.f;if(dizzyR>13.f&&ms-idleTimer>1800){ist=1;idleTimer=ms;}}
      else{dizzyR=elf(dizzyR,0,dt*7.f);dizzyA+=dt*6.f;if(dizzyR<0.5f){dizzyR=0;dOLX=0;dOLY=0;dORX=0;dORY=0;idleAnim=IA_NONE;nextIdle=random(7000,15000);}}
      dOLX=(int)(dizzyR*cosf(dizzyA));dOLY=(int)(dizzyR*sinf(dizzyA));
      dORX=(int)(dizzyR*cosf(dizzyA+PI));dORY=(int)(dizzyR*sinf(dizzyA+PI));
      break;
    }
    case IA_YAWN:{
      if(ist==0){yawnLid=elf(yawnLid,0.85f,dt*0.7f);yawnMouth=elf(yawnMouth,2.f,dt*2.f);if(yawnLid>0.83f){ist=1;idleTimer=ms;}}
      else if(ist==1){yawnMouth=elf(yawnMouth,18.f,dt*3.f);if(ms-idleTimer>800)ist=2;}
      else if(ist==2){if(ms-idleTimer>1400)ist=3;}
      else{yawnLid=elf(yawnLid,0,dt*1.0f);yawnMouth=elf(yawnMouth,0,dt*4.f);if(yawnLid<0.02f){yawnLid=0;yawnMouth=0;idleAnim=IA_NONE;nextIdle=random(10000,20000);}}
      break;
    }
    case IA_THINK:{
      if(ist==0){thinkX=elf(thinkX,22.f,dt*2.f);thinkY=elf(thinkY,-14.f,dt*2.f);thinkSq=elf(thinkSq,0.35f,dt*1.5f);if(thinkX>20.f){ist=1;idleTimer=ms;}}
      else if(ist==1){if(ms-idleTimer>(unsigned long)random(1800,4000))ist=2;}
      else{thinkX=elf(thinkX,0,dt*2.5f);thinkY=elf(thinkY,0,dt*2.5f);thinkSq=elf(thinkSq,0,dt*3.f);if(fabsf(thinkX)<0.5f){thinkX=0;thinkY=0;thinkSq=0;idleAnim=IA_NONE;nextIdle=random(8000,16000);}}
      break;
    }
    case IA_WINK:{
      if(ist==0){winkT=elf(winkT,1.f,dt*10.f);if(winkT>0.97f){winkT=1.f;ist=1;idleTimer=ms;}}
      else if(ist==1){if(ms-idleTimer>180)ist=2;}
      else{winkT=elf(winkT,0,dt*7.f);if(winkT<0.02f){winkT=0;idleAnim=IA_NONE;nextIdle=random(6000,14000);}}
      break;
    }
    case IA_ROLL:{
      if(ist==0){rollY=elf(rollY,-18.f,dt*3.f);if(rollY<-17.f){ist=1;idleTimer=ms;}}
      else if(ist==1){
        if(rollX==0)rollX=0.01f*(random(2)?1:-1);
        float tX=rollX>0?45.f:-45.f;
        rollX=elf(rollX,tX,dt*2.5f);
        if(fabsf(rollX)>43.f){ist=2;idleTimer=ms;}
      }
      else if(ist==2){if(ms-idleTimer>300)ist=3;}
      else{rollY=elf(rollY,0,dt*3.5f);rollX=elf(rollX,0,dt*3.5f);if(fabsf(rollY)<0.5f&&fabsf(rollX)<0.5f){rollY=0;rollX=0;idleAnim=IA_NONE;nextIdle=random(8000,16000);}}
      break;
    }
    case IA_SMUG:{
      float tX=random(2)?38.f:-38.f;
      if(ist==0){smugX=elf(smugX,tX,dt*1.0f);smugSq=elf(smugSq,0.28f,dt*1.2f);if(fabsf(smugX)>36.f){ist=1;idleTimer=ms;}}
      else if(ist==1){if(ms-idleTimer>(unsigned long)random(1500,3500))ist=2;}
      else{smugX=elf(smugX,0,dt*1.2f);smugSq=elf(smugSq,0,dt*1.5f);if(fabsf(smugX)<0.5f){smugX=0;smugSq=0;idleAnim=IA_NONE;nextIdle=random(9000,16000);}}
      break;
    }
    case IA_EXABLINK:{
      if(ist==0){blinkT=elf(blinkT,1.f,dt*0.9f);if(blinkT>0.97f){blinkT=1.f;ist=1;idleTimer=ms;}}
      else if(ist==1){if(ms-idleTimer>400)ist=2;}
      else{blinkT=elf(blinkT,0,dt*1.0f);if(blinkT<0.02f){blinkT=0;idleAnim=IA_NONE;nextIdle=random(7000,14000);}}
      break;
    }
    case IA_SIDEEYE:{
      float tX=random(2)?60.f:-60.f;
      if(ist==0){sideX=elf(sideX,tX,dt*6.f);if(fabsf(sideX)>58.f){ist=1;idleTimer=ms;}}
      else if(ist==1){if(ms-idleTimer>(unsigned long)random(2000,5000))ist=2;}
      else{sideX=elf(sideX,0,dt*4.5f);if(fabsf(sideX)<0.5f){sideX=0;idleAnim=IA_NONE;nextIdle=random(8000,15000);}}
      break;
    }
    case IA_XBOUNCE:{
      xbPhase+=dt*22.f;
      xbAmt=sinf(xbPhase)*5.f;
      if(ist==0&&ms-idleTimer>(unsigned long)random(800,2000)){ist=1;idleTimer=ms;}
      if(ist==1){xbAmt=elf(xbAmt,0,dt*6.f);if(fabsf(xbAmt)<0.2f){xbAmt=0;idleAnim=IA_NONE;nextIdle=random(5000,12000);}}
      break;
    }
    case IA_SDROOP:{
      float target=cf(0.18f*(sdCount+1),0,0.55f);
      if(ist==0){sdLid=elf(sdLid,target,dt*0.5f);if(sdLid>=target-0.01f){ist=1;idleTimer=ms;}}
      else if(ist==1){if(ms-idleTimer>600)ist=2;}
      else{sdLid=elf(sdLid,0,dt*14.f);if(sdLid<0.02f){sdLid=0;sdCount++;ist=0;idleTimer=ms;if(sdCount>=3){idleAnim=IA_NONE;nextIdle=random(8000,16000);}}}
      break;
    }
    case IA_SBLINK:{
      if(ist==0){blinkT=elf(blinkT,1.f,dt*30.f);if(blinkT>0.97f){blinkT=1.f;ist=1;idleTimer=ms;}}
      else if(ist==1){if(ms-idleTimer>30)ist=2;}
      else{blinkT=elf(blinkT,0,dt*16.f);if(blinkT<0.02f){blinkT=0;idleAnim=IA_NONE;nextIdle=random(5000,12000);}}
      break;
    }
    case IA_PETJOY:{
      // Stage 0  eyes swell big, bottom lids crinkle up
      if(petJoyStage==0){
        petJoyScale  = elf(petJoyScale,  1.0f, dt*14.f);
        petJoySquish = elf(petJoySquish, 0.32f,dt*10.f);
        petJoyPhase += dt*28.f;
        petJoyBounce = sinf(petJoyPhase)*5.f;
        if(petJoyScale>0.92f){ petJoyStage=1; idleTimer=ms; }
      }
      // Stage 1  hold bouncing joy for 1.2s
      else if(petJoyStage==1){
        petJoyPhase  += dt*28.f;
        petJoyBounce  = sinf(petJoyPhase)*8.f;
        petJoyScale   = elf(petJoyScale,  0.8f, dt*2.f);
        petJoySquish  = elf(petJoySquish, 0.28f,dt*2.f);
        if(ms-idleTimer>1200){ petJoyStage=2; idleTimer=ms; }
      }
      // Stage 2  gentle settle
      else{
        petJoyPhase  += dt*6.f;
        petJoyScale  = elf(petJoyScale,  0, dt*2.5f);
        petJoySquish = elf(petJoySquish, 0, dt*2.5f);
        petJoyBounce = elf(petJoyBounce, 0, dt*4.f);
        if(petJoyScale<0.02f){
          petJoyScale=0;petJoySquish=0;petJoyBounce=0;
          idleAnim=IA_NONE;
          nextIdle=random(6000,12000);
        }
      }
      break;
    }
    default: break;
  }
}

// 
//  TEARDROP
// 
void eraseTearCol(int x,int y0,int y1){
  if(y0>y1){int t=y0;y0=y1;y1=t;}
  y0=max(y0,0);y1=min(y1,SH-1);
  for(int y=y0;y<=y1;y++) tft.drawPixel(x,y,BLK);
}
void spawnTear(){
  if(tearOn) eraseTearCol((int)tearX,SPR_Y+SPR_H,(int)tearDrawY+2);
  tearX    =SPR_X+BASE_ELX+random(-6,6);
  tearY    =(float)(SPR_Y+SPR_H+1);
  tearDrawY=tearY;
  tearSpd  =(float)random(12,22)/10.f;
  tearOn   =true;
}
void updateTear(){
  if(!tearOn) return;
  float prevY=tearDrawY;
  tearY+=tearSpd; tearDrawY=tearY;
  if(tearY>SH+2){eraseTearCol((int)tearX,(int)prevY,SH-1);tearOn=false;return;}
  eraseTearCol((int)tearX,(int)prevY,(int)prevY);
  int iy=(int)tearY;
  if(iy>=SPR_Y+SPR_H&&iy<SH) tft.drawPixel((int)tearX,iy,0xAEDC);
}

// 
//  TWITCH
// 
void startTwitch(){
  if(twitchOn)return;
  twitchOn=true;twitchEye=random(2);
  twitchLeft=random(4,9);twitchAmt=0;twitchTimer=millis();
}
void updateTwitch(){
  if(!twitchOn)return;
  unsigned long ms=millis();
  unsigned long ph=(ms-twitchTimer)%80;
  int j=(int)((ms-twitchTimer)/80);
  if(j>=twitchLeft){twitchOn=false;twitchAmt=0;return;}
  twitchAmt=(ph<40)?(float)(random(4,9))*(twitchEye==0?-1:1):0;
}

// 
//  WIFI + TIME
// 
void initWifi(){
  Serial.print("WiFi");
  WiFi.begin(WIFI_SSID,WIFI_PASS);
  unsigned long t=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-t<8000){delay(300);Serial.print(".");}
  if(WiFi.status()==WL_CONNECTED){wifiOK=true;Serial.println(" OK");configTime(TZ_OFFSET,0,NTP_SERVER);}
  else Serial.println(" FAILED");
}
bool pollTime(){
  if(!wifiOK)return false;
  struct tm ti;if(!getLocalTime(&ti,200))return false;
  currentHour=ti.tm_hour;currentDow=ti.tm_wday;return true;
}
Expr timeExpr(){
  if(currentHour<0)return NORMAL;
  if(currentHour>=22||currentHour<6)return SLEEPY;
  if(currentHour<9) return ANGRY;
  if(currentDow==0||currentDow==6)return HAPPY;
  return NORMAL;
}

// 
//  SETUP
// 
// 
//  HEART ANIMATION
// 
void initHearts(){
  for(int i=0;i<MAX_HEARTS;i++) hearts[i].active=false;
}

void spawnHeart(float x, float y){
  for(int i=0;i<MAX_HEARTS;i++){
    if(!hearts[i].active){
      hearts[i].x    = x + (float)random(-16,16);
      hearts[i].y    = y;
      hearts[i].vy   = (float)random(55,110);
      hearts[i].life = 1.0f;
      hearts[i].size = (float)random(7,14);
      hearts[i].active = true;
      return;
    }
  }
}

// Hearts drawn into sprite (auto-erased by fillSprite each frame  no artifacts)
void sprHeart(int cx, int cy, int r, uint16_t col){
  if(r < 2) return;
  // Clamp to sprite bounds
  cx = constrain(cx, r+2, SPR_W-r-2);
  cy = constrain(cy, r+2, SPR_H-r-2);
  spr.fillCircle(cx - r/2, cy - r/4, r/2, col);
  spr.fillCircle(cx + r/2, cy - r/4, r/2, col);
  int top = cy - r/4 + r/2;
  for(int row=0; row<=r; row++){
    int hw = r - row + 1;
    if(hw < 1) break;
    spr.drawFastHLine(cx - hw + 1, top + row, (hw-1)*2, col);
  }
}

void updateHearts(float dt){
  // Just update positions  drawing happens inside renderEyes via drawHeartsInSprite()
  for(int i=0;i<MAX_HEARTS;i++){
    if(!hearts[i].active) continue;
    hearts[i].y    -= hearts[i].vy * dt;
    hearts[i].life -= dt * 0.65f;
    if(hearts[i].life <= 0 || hearts[i].y < (float)(-SPR_Y)-20){
      hearts[i].active = false;
    }
  }
}

void drawHeartsInSprite(){
  for(int i=0;i<MAX_HEARTS;i++){
    if(!hearts[i].active) continue;
    float l = hearts[i].life;
    uint8_t r5 = (uint8_t)cf(8  + 23*l, 0, 31);
    uint8_t g6 = (uint8_t)cf(1  +  6*l, 0, 63);
    uint8_t b5 = (uint8_t)cf(5  + 13*l, 0, 31);
    uint16_t col = ((uint16_t)r5<<11)|((uint16_t)g6<<5)|b5;
    int nr = (int)(hearts[i].size * (0.5f + 0.5f*l));
    if(nr < 2) nr = 2;
    // Convert screen coords to sprite coords
    int sx = (int)hearts[i].x;
    int sy = (int)hearts[i].y - SPR_Y;
    sprHeart(sx, sy, nr, col);
  }
}

// 
//  CONFETTI
// 
void initConfetti(){ for(int i=0;i<MAX_CONFETTI;i++) confetti[i].active=false; }

void spawnConfetti(){
  for(int i=0;i<MAX_CONFETTI;i++){
    confetti[i].x     = (float)random(10, SW-10);
    confetti[i].y     = (float)random(-40, 0);
    confetti[i].vx    = (float)random(-30,30);
    confetti[i].vy    = (float)random(60,140);
    confetti[i].life  = 1.0f;
    confetti[i].angle = (float)random(0,628)/100.f;
    confetti[i].spin  = (float)random(-4,4);
    confetti[i].active= true;
  }
}

void updateAndDrawConfetti(float dt){
  static uint16_t confCols[]={0xF800,0x07E0,0x001F,0xFFE0,0xF81F,0x07FF,0xFFFF};
  for(int i=0;i<MAX_CONFETTI;i++){
    if(!confetti[i].active) continue;
    // Erase
    tft.fillRect((int)confetti[i].x-2,(int)confetti[i].y-2,5,5,BLK);
    confetti[i].x    += confetti[i].vx * dt;
    confetti[i].y    += confetti[i].vy * dt;
    confetti[i].angle+= confetti[i].spin * dt;
    confetti[i].life -= dt * 0.35f;
    if(confetti[i].life<=0||confetti[i].y>SH+10){
      confetti[i].active=false; continue;
    }
    uint16_t col = confCols[i % 7];
    // Draw as small rotated rect (2 pixels wide)
    int cx=(int)confetti[i].x, cy=(int)confetti[i].y;
    tft.fillRect(cx-1,cy-1,3,3,col);
  }
}

// 
//  MILESTONE CELEBRATIONS  each fires exactly once
// 
void triggerMilestone(int idx){
  milestoneActive = true;
  milestoneWhich  = idx;
  milestoneEndMs  = millis() + 6000;
  Serial.printf("MILESTONE %d: %d pets!\n", idx, MILESTONES[idx]);

  switch(idx){
    case 0: // 10 pets  happy bounce + hearts shower
      pushMood(HAPPY); setExpr(HAPPY);
      idleAnim=IA_PETJOY; petJoyScale=0; petJoySquish=0;
      petJoyBounce=0; petJoyPhase=0; petJoyStage=0; idleTimer=millis();
      for(int h=0;h<MAX_HEARTS;h++) spawnHeart((float)random(20,300),(float)random(40,180));
      break;

    case 1: // 50 pets  excited expression + stars burst from both eyes
      pushMood(EXCITED); setExpr(EXCITED);
      spawnStars(BASE_ELX, spriteY+ECY);
      spawnStars(BASE_ERX, spriteY+ECY);
      idleAnim=IA_XBOUNCE; xbPhase=0; xbAmt=0; ist=0; idleTimer=millis();
      break;

    case 2: // 100 pets  confetti explosion + rainbow RGB cycle
      pushMood(HAPPY); setExpr(HAPPY);
      spawnConfetti();
      confettiActive = true;
      confettiEndMs  = millis() + 5000;
      for(int h=0;h<MAX_HEARTS;h++) spawnHeart((float)random(20,300),(float)random(20,200));
      idleAnim=IA_PETJOY; petJoyScale=0; petJoySquish=0;
      petJoyBounce=0; petJoyPhase=0; petJoyStage=0; idleTimer=millis();
      break;

    case 3: // 500 pets  full chaos
      pushMood(EXCITED); setExpr(EXCITED);
      spawnConfetti();
      confettiActive = true;
      confettiEndMs  = millis() + 8000;
      spawnStars(SW/2, SH/2);
      for(int h=0;h<MAX_HEARTS;h++) spawnHeart((float)random(20,300),(float)random(20,200));
      for(int s=0;s<MAX_SPARKS;s++) spawnSparks((float)random(20,300),(float)random(20,200));
      setRGB(255,255,255);
      break;
  }
  // Queue milestone bubble after celebration settles
  const char* milestoneMsgs[] = {
    "10 pets!\nthanks for\nnoticing me",
    "50 pets!\nyou really\nlike me huh",
    "100 pets!\nokay i love you\na normal amount",
    "500 PETS\ni am going to\ncry (i cant cry)"
  };
  if(idx >= 0 && idx < 4){
    strncpy(bubbleText, milestoneMsgs[idx], sizeof(bubbleText)-1);
    bubbleActive  = true;
    bubbleStartMs = millis() + 3000; // fire after 3s so anim plays first
    chattingLook  = true;
    nextBubbleMs  = millis() + bubbleShowMs + 180000;
  }
}

void updateMilestone(){
  if(!milestoneActive) return;
  unsigned long ms = millis();
  if(ms >= milestoneEndMs){
    milestoneActive = false;
    confettiActive  = false;
    pushMood(HAPPY); setExpr(HAPPY);
    lastExpr=ms; nextExprMs=random(6000,12000);
    return;
  }
  // milestone 2+3: rainbow RGB cycle during celebration
  if(milestoneWhich >= 2){
    float t = (float)(ms % 600) / 600.f;
    uint8_t r=(uint8_t)(sinf(t*2*PI)*127+128);
    uint8_t g=(uint8_t)(sinf(t*2*PI+2.094f)*127+128);
    uint8_t b=(uint8_t)(sinf(t*2*PI+4.189f)*127+128);
    setRGB(r,g,b);
  }
}

// 
//  SEASONAL EVENTS
// 
void checkSeason(){
  if(!wifiOK) return;
  struct tm ti; if(!getLocalTime(&ti,200)) return;
  int mon = ti.tm_mon+1;  // 1-12
  int day = ti.tm_mday;
  int hr  = ti.tm_hour;
  int mn  = ti.tm_min;

  halloweenMode = (mon==10 && day==31);
  christmasMode = (mon==12 && day==25);
  // New Year's Eve: Dec 31 from 11:55pm until midnight fires
  newYearMode   = (mon==12 && day==31 && hr==23 && mn>=55);
  // Midnight strike
  if(mon==1 && day==1 && hr==0 && mn==0 && !confettiActive){
    spawnConfetti();
    confettiActive = true;
    confettiEndMs  = millis() + 10000;
    spawnStars(SW/2, SH/2);
    for(int h=0;h<MAX_HEARTS;h++) spawnHeart((float)random(20,300),(float)random(20,200));
    setRGB(255,255,0);
  }
}

void applySeasonalColour(){
  if(milestoneActive) return; // milestone overrides season
  if(spookyMode) return;      // spooky override already handled

  unsigned long ms = millis();

  if(halloweenMode){
    // Buddy: purple spooky instead of orange
    eyeColour = iAmA ? 0xFBE0 : 0xD01F;
    static unsigned long lastFlicker=0;
    if(ms-lastFlicker>200){ lastFlicker=ms;
      static bool flickOn=true; flickOn=!flickOn;
      if(iAmA){
        if(flickOn) setRGB(200,60,0); else setRGB(60,20,0);
      } else {
        if(flickOn) setRGB(120,0,160); else setRGB(40,0,60);
      }
    }
  } else if(christmasMode){
    if(ms-lastXmasSwap>1500){ lastXmasSwap=ms; xmasColourFlip=!xmasColourFlip; }
    if(iAmA){
      if(xmasColourFlip){ eyeColour=0xF800; setRGB(180,0,0); }
      else              { eyeColour=0x07E0; setRGB(0,150,0); }
    } else {
      // Buddy: pink/white Christmas
      if(xmasColourFlip){ eyeColour=0xFC18; setRGB(255,0,120); }
      else              { eyeColour=0xFFFF; setRGB(200,200,220); }
    }
  } else if(newYearMode){
    // Eyes flash gold, RGB cycles fast
    float t=(float)(ms%400)/400.f;
    uint8_t r=(uint8_t)(sinf(t*2*PI)*127+128);
    uint8_t g=(uint8_t)(sinf(t*2*PI+2.094f)*127+128);
    uint8_t b=(uint8_t)(sinf(t*2*PI+4.189f)*127+128);
    setRGB(r,g,b);
    eyeColour=0xFFE0; // gold
  }
}

// 
//  FRIEND NETWORKING
// 

void friendSend(uint8_t type, uint8_t milestoneIdx){
  if(!wifiOK || !udpStarted) return;
  FriendPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.type         = type;
  pkt.deviceId     = deviceId;
  pkt.expr         = (uint8_t)curExpr;
  pkt.milestoneIdx = milestoneIdx;
  pkt.petsToday    = (int16_t)petsToday;
  pkt.petsTotal    = (int16_t)petsTotal;
  strncpy(pkt.name, robotName, 19);
  // Broadcast on subnet
  IPAddress bcast = WiFi.localIP();
  bcast[3] = 255;
  udp.beginPacket(bcast, FRIEND_PORT);
  udp.write((uint8_t*)&pkt, sizeof(pkt));
  udp.endPacket();
}
void friendSend(uint8_t type){ friendSend(type,0); }

void friendHandlePacket(FriendPacket &pkt){
  unsigned long ms = millis();
  bool wasOnline = friendOnline;

  // Update friend state
  friendLastSeen   = ms;
  friendOnline     = true;
  friendExpr       = pkt.expr;
  friendPetsToday  = pkt.petsToday;
  friendPetsTotal  = pkt.petsTotal;
  strncpy(friendName, pkt.name, EEPROM_NAME_LEN-1);

  // Friend just came online
  if(!wasOnline){
    friendJustCameOnline = true;
    Serial.printf("Friend online: %s\n", friendName);
  }

  // React to packet type
  switch(pkt.type){
    case PKT_STATE:{
      // Only react when friend expression CHANGES  not every 1s packet
      uint8_t prevExpr = friendExpr; // captured before we updated above
      // (friendExpr was already updated at top of function  use wasOnline + compare)
      static uint8_t lastReactedExpr = 255; // 255 = never reacted
      if(pkt.expr != lastReactedExpr){
        lastReactedExpr = pkt.expr;
        if((Expr)pkt.expr == ANGRY && curExpr != ANGRY){
          pendingFriendAngry = true;
          pendingReactMs = ms + random(500, 1500);
        } else if((Expr)pkt.expr == SLEEPY && curExpr != SLEEPY && curExpr != ANGRY){
          pendingFriendSleepy = true;
          pendingReactMs = ms + random(1000, 3000);
        } else if((Expr)pkt.expr == SURPRISED){
          pendingFriendSurprised = true;
          pendingReactMs = ms + random(200, 600);
        }
      }
      break;}
    case PKT_POKE:
      pendingFriendPoke = true;
      pendingReactMs = ms + random(100, 300);
      Serial.println("Friend poked us!");
      break;
    case PKT_MILESTONE:
      pendingFriendMilestone = true;
      pendingReactMs = ms + random(800, 2000);
      break;
    case PKT_HELLO:
      // Reply immediately so they know we exist
      friendSend(PKT_STATE);
      break;
  }
}

void friendProcessPending(){
  if(!friendOnline) return;
  unsigned long ms = millis();
  if(ms < pendingReactMs) return;

  if(pendingFriendPoke){
    pendingFriendPoke = false;
    // Poke! Startle then happy
    idleAnim=IA_STARTLED; startledSc=0; ist=0; idleTimer=ms;
    pushMood(SURPRISED); setExpr(SURPRISED);
    for(int h=0;h<4;h++) spawnHeart((float)random(20,300),(float)random(20,180));
    lastExpr=ms; nextExprMs=random(4000,8000);
  }
  else if(pendingFriendMilestone){
    pendingFriendMilestone = false;
    // Small sympathetic celebration
    for(int h=0;h<3;h++) spawnHeart((float)random(60,260),(float)random(40,160));
    spawnStars(SW/2, SH/2);
    if(idleAnim==IA_NONE){ idleAnim=IA_XBOUNCE; xbPhase=0; xbAmt=0; ist=0; idleTimer=ms; }
  }
  else if(pendingFriendAngry){
    pendingFriendAngry = false;
    // Friend is angry  we go suspicious, glance sideways
    if(curExpr!=ANGRY){
      pushMood(SUSPICIOUS); setExpr(SUSPICIOUS);
      if(idleAnim==IA_NONE){ idleAnim=IA_SIDEEYE; ist=0; idleTimer=ms; }
      lastExpr=ms; nextExprMs=random(4000,7000);
    }
  }
  else if(pendingFriendSleepy){
    pendingFriendSleepy = false;
    // Sympathetic yawn
    if(idleAnim==IA_NONE){ idleAnim=IA_YAWN; yawnLid=0; yawnMouth=0; ist=0; idleTimer=ms; }
  }
  else if(pendingFriendSurprised){
    pendingFriendSurprised = false;
    // Brief startle reaction
    if(idleAnim==IA_NONE){ idleAnim=IA_SBLINK; ist=0; idleTimer=ms; }
  }
}

void friendDiscovered(){
  unsigned long ms = millis();
  pushMood(LOVESTRUCK); setExpr(LOVESTRUCK);
  // Spawn hearts only on the first discovery  not every reconnect
  static unsigned long lastDiscovery = 0;
  if(ms - lastDiscovery > 30000){  // at least 30s between full greetings
    for(int h=0;h<MAX_HEARTS;h++)
      spawnHeart((float)random(20,300),(float)random(20,180));
    lastDiscovery = ms;
  }
  idleAnim=IA_XBOUNCE; xbPhase=0; xbAmt=0; ist=0; idleTimer=ms;
  lastExpr=ms; nextExprMs=random(6000,10000);
  Serial.printf("Friend discovered: %s!\n", friendName);
}

void friendTick(){
  if(!wifiOK) return;
  unsigned long ms = millis();

  // Start UDP once
  if(!udpStarted){
    udp.begin(FRIEND_PORT);
    udpStarted = true;
    deviceId = 2; iAmA = false;
    Serial.println("I am EMBER (device B)");
  }

  // Read incoming packets
  int sz = udp.parsePacket();
  if(sz >= (int)sizeof(FriendPacket)){
    FriendPacket pkt;
    udp.read((uint8_t*)&pkt, sizeof(pkt));
    // Ignore our own broadcasts
    if(pkt.deviceId != deviceId){
      friendHandlePacket(pkt);
    }
  }

  // Friend came online this tick
  if(friendJustCameOnline){
    friendJustCameOnline = false;
    // Only do greeting if we've been running a while (not both booting together)
    if(ms > 8000) friendDiscovered();
    else {
      // Both booting  lovestruck greeting
      pushMood(LOVESTRUCK); setExpr(LOVESTRUCK);
      lastExpr=ms; nextExprMs=random(5000,9000);
    }
  }

  // Check if friend went offline
  if(friendOnline && ms - friendLastSeen > FRIEND_TIMEOUT_MS){
    friendOnline = false;
    friendJustWentOffline = true;
    Serial.printf("Friend %s went offline\n", friendName);
  }

  // Friend just went offline  sad reaction
  if(friendJustWentOffline){
    friendJustWentOffline = false;
    pushMood(SAD); setExpr(SAD);
    lastExpr=ms; nextExprMs=random(8000,15000);
  }

  // Periodic state broadcast
  if(ms - lastStateSend > 1000){ lastStateSend=ms; friendSend(PKT_STATE); }

  // Process any pending reactions
  friendProcessPending();
}

// Call this when we get petted  notify friend
void friendNotifyPet(){
  if(friendOnline) friendSend(PKT_STATE); // state update carries petsTotal
}

// Call when pet a milestone  notify friend to celebrate too
void friendNotifyMilestone(uint8_t idx){
  if(friendOnline) friendSend(PKT_MILESTONE, idx);
}

// 
//  EEPROM  pet counter + name
// 
void eepromLoad(){
  EEPROM.begin(EEPROM_SIZE);
  int savedDay = 0;
  EEPROM.get(EEPROM_LAST_DAY,  savedDay);
  EEPROM.get(EEPROM_PETS_TOTAL, petsTotal);
  // Check if day rolled over
  struct tm ti; getLocalTime(&ti, 200);
  if(ti.tm_yday != savedDay){
    petsToday = 0;
    EEPROM.put(EEPROM_PETS_TODAY, 0);
    EEPROM.put(EEPROM_LAST_DAY, ti.tm_yday);
    EEPROM.commit();
  } else {
    EEPROM.get(EEPROM_PETS_TODAY, petsToday);
  }
  // Load name
  char buf[EEPROM_NAME_LEN];
  for(int i=0;i<EEPROM_NAME_LEN;i++) buf[i]=EEPROM.read(EEPROM_NAME_ADDR+i);
  buf[EEPROM_NAME_LEN-1]='\0';
  if(buf[0]>='A' && buf[0]<='Z') strncpy(robotName, buf, EEPROM_NAME_LEN);
  if(petsTotal < 0 || petsTotal > 99999) petsTotal = 0;
  if(petsToday < 0 || petsToday > 9999)  petsToday = 0;
  milestoneFlags = EEPROM.read(EEPROM_MILESTONES);
  if(milestoneFlags == 0xFF) milestoneFlags = 0;
  dayCounterLoad();
}

void triggerMilestone(int idx);  // forward declare

void eepromSavePet(){
  petsToday++;
  petsTotal++;
  struct tm ti; getLocalTime(&ti, 200);
  EEPROM.put(EEPROM_PETS_TODAY, petsToday);
  EEPROM.put(EEPROM_PETS_TOTAL, petsTotal);
  EEPROM.put(EEPROM_LAST_DAY,   ti.tm_yday);
  // Check milestones
  for(int i=0;i<MILESTONE_COUNT;i++){
    if(petsTotal >= MILESTONES[i] && !(milestoneFlags & (1<<i))){
      milestoneFlags |= (1<<i);
      EEPROM.write(EEPROM_MILESTONES, milestoneFlags);
      triggerMilestone(i);
      friendNotifyMilestone(i);
      break; // one at a time
    }
  }
  EEPROM.commit();
  friendNotifyPet();
}

// 
//  STAR PARTICLES  (triple-tap dizzy)
// 
#define MAX_STARS 10
StarP stars[MAX_STARS];

void initStars(){ for(int i=0;i<MAX_STARS;i++) stars[i].active=false; }

void spawnStars(float cx, float cy){
  for(int i=0;i<MAX_STARS;i++){
    float ang = (float)i * (2.f*PI/MAX_STARS);
    stars[i].x     = cx;
    stars[i].y     = cy;
    stars[i].vx    = cosf(ang)*(float)random(40,100);
    stars[i].vy    = sinf(ang)*(float)random(40,100) - 60.f;
    stars[i].life  = 1.0f;
    stars[i].angle = ang;
    stars[i].spin  = (float)random(-8,8);
    stars[i].active= true;
  }
}

void tftStar(int cx, int cy, int r, uint16_t col){
  // 4-point star: two crossed lines
  tft.drawFastHLine(cx-r, cy, r*2, col);
  tft.drawFastVLine(cx, cy-r, r*2, col);
  tft.drawLine(cx-r*7/10, cy-r*7/10, cx+r*7/10, cy+r*7/10, col);
  tft.drawLine(cx+r*7/10, cy-r*7/10, cx-r*7/10, cy+r*7/10, col);
}

void updateAndDrawStars(float dt){
  for(int i=0;i<MAX_STARS;i++){
    if(!stars[i].active) continue;
    int ox=(int)stars[i].x, oy=(int)stars[i].y;
    tftStar(ox, oy, 4, BLK); // erase
    stars[i].x    += stars[i].vx * dt;
    stars[i].y    += stars[i].vy * dt;
    stars[i].vy   += 80.f * dt;
    stars[i].life -= dt * 1.2f;
    if(stars[i].life <= 0 || stars[i].y > SH+10){
      stars[i].active = false; continue;
    }
    float l = stars[i].life;
    // Yellow  white
    uint8_t r5=31, g6=(uint8_t)cf(40+24*l,0,63), b5=(uint8_t)cf(l*20,0,31);
    uint16_t col=((uint16_t)r5<<11)|((uint16_t)g6<<5)|b5;
    tftStar((int)stars[i].x,(int)stars[i].y, 4, col);
  }
}

// 
//  ANGRY SPARKS
// 
void initSparks(){ for(int i=0;i<MAX_SPARKS;i++) sparks[i].active=false; }

void spawnSparks(float x, float y){
  for(int i=0;i<MAX_SPARKS;i++){
    if(!sparks[i].active){
      sparks[i].x    = x + random(-10,10);
      sparks[i].y    = y;
      sparks[i].vx   = (float)random(-60,60);
      sparks[i].vy   = -(float)random(80,160);
      sparks[i].life = 1.0f;
      sparks[i].active = true;
      if(i >= MAX_SPARKS-1) break;
    }
  }
}

void updateAndDrawSparks(float dt){
  for(int i=0;i<MAX_SPARKS;i++){
    if(!sparks[i].active) continue;
    // Erase
    tft.drawPixel((int)sparks[i].x,   (int)sparks[i].y,   BLK);
    tft.drawPixel((int)sparks[i].x+1, (int)sparks[i].y,   BLK);
    tft.drawPixel((int)sparks[i].x,   (int)sparks[i].y+1, BLK);
    // Update  zigzag by adding sin to vx
    sparks[i].x  += sparks[i].vx * dt;
    sparks[i].y  += sparks[i].vy * dt;
    sparks[i].vy += 120.f * dt; // gravity pulls down
    sparks[i].vx += sinf(sparks[i].y * 0.3f) * 40.f * dt; // zigzag
    sparks[i].life -= dt * 1.8f;
    if(sparks[i].life <= 0 || sparks[i].y > SH || sparks[i].y < 0){
      sparks[i].active = false; continue;
    }
    // Colour: red  orange  yellow as it fades
    float l = sparks[i].life;
    uint8_t r5 = 31;
    uint8_t g6 = (uint8_t)cf(l * 28, 0, 63);
    uint8_t b5 = 0;
    uint16_t col = ((uint16_t)r5<<11)|((uint16_t)g6<<5)|b5;
    int px = constrain((int)sparks[i].x, 0, SW-2);
    int py = constrain((int)sparks[i].y, 0, SH-2);
    tft.drawPixel(px,   py,   col);
    tft.drawPixel(px+1, py,   col);
    tft.drawPixel(px,   py+1, col);
  }
}

// 
//  SLEEP ZZZs
// 
void initZzls(){ for(int i=0;i<MAX_ZZLS;i++) zzls[i].active=false; }

void spawnZzz(){
  for(int i=0;i<MAX_ZZLS;i++){
    if(!zzls[i].active){
      // Float up from right eye area
      zzls[i].x    = (float)(BASE_ERX + random(0,20));
      zzls[i].y    = (float)(spriteY + ECY - 10);
      zzls[i].vy   = (float)random(18,32);
      zzls[i].life = 1.0f;
      zzls[i].size = 1.0f + (float)i * 0.5f; // each Z a bit bigger
      zzls[i].active = true;
      return;
    }
  }
}

// Draw a tiny Z at (cx,cy) size sz on TFT
void tftZ(int cx, int cy, int sz, uint16_t col){
  int w = sz*3, h = sz*3;
  tft.drawFastHLine(cx,     cy,     w, col); // top
  tft.drawLine(cx+w-1, cy, cx, cy+h, col);  // diagonal
  tft.drawFastHLine(cx,     cy+h,   w, col); // bottom
}
void eraseZ(int cx, int cy, int sz){
  int w = sz*3+1, h = sz*3+1;
  tft.fillRect(cx-1, cy-1, w+2, h+2, BLK);
}

void updateAndDrawZzls(float dt){
  for(int i=0;i<MAX_ZZLS;i++){
    if(!zzls[i].active) continue;
    int ox=(int)zzls[i].x, oy=(int)zzls[i].y, osz=(int)(zzls[i].size*2);
    eraseZ(ox, oy, osz);
    zzls[i].y    -= zzls[i].vy * dt;
    zzls[i].x    += sinf(zzls[i].y * 0.08f) * 0.5f; // gentle sway
    zzls[i].life -= dt * 0.4f;
    if(zzls[i].life <= 0 || zzls[i].y < 0){
      zzls[i].active = false; continue;
    }
    float l = zzls[i].life;
    uint8_t b5 = (uint8_t)cf(l*18, 0, 31);
    uint8_t g6 = (uint8_t)cf(l*24, 0, 63);
    uint16_t col = ((uint16_t)4<<11)|((uint16_t)g6<<5)|b5; // soft blue-green
    int nx=(int)zzls[i].x, ny=(int)zzls[i].y, nsz=(int)(zzls[i].size*2);
    if(ny > 0 && ny < SH) tftZ(nx, ny, max(nsz,1), col);
  }
}

// 
//  LOVESTRUCK  draw heart in place of each eye
// 
void sprHeartEye(int cx, int cy, int r, uint16_t col){
  if(r < 4) return;
  cx = constrain(cx, r+4, SPR_W-r-4);
  cy = constrain(cy, r+4, SPR_H-r-4);
  // Two overlapping circles form the top lobes
  // Offset inward so they overlap and merge seamlessly
  int cr  = (r * 11) / 20;   // circle radius ~0.55r
  int cox = (r * 9)  / 20;   // circle centre offset x ~0.45r
  int coy = r / 4;            // circle centre offset y upward
  int lobeY = cy - coy;
  spr.fillCircle(cx - cox, lobeY, cr, col);
  spr.fillCircle(cx + cox, lobeY, cr, col);
  // Fill the body below the lobes with horizontal lines
  // from the widest point of the circles down to a point
  int bodyTop = lobeY;         // start scan from circle centres
  int bodyBot = cy + r - coy;  // tip of heart
  for(int y = bodyTop; y <= bodyBot; y++){
    float t  = (float)(y - bodyTop) / (float)(bodyBot - bodyTop + 1);
    // Width tapers from full at top to 0 at bottom
    int hw = (int)((1.f - t) * (cox + cr));
    if(hw > 0) spr.drawFastHLine(cx - hw, y, hw*2, col);
  }
  // Glisten dot top-right lobe
  spr.fillCircle(cx + cox/2, lobeY - cr/3, max(1, r/6), 0xFFFF);
}

void updateLovestruck(float dt){
  if(curExpr == LOVESTRUCK){
    if(!lovestruck){
      lovestruck=true;
      loveHeartL=0; loveHeartR=0; lovePulse=0;
    }
    // Grow in slowly (takes ~0.8s to reach full size)
    loveHeartL = elf(loveHeartL, 1.0f, dt*1.2f);
    loveHeartR = elf(loveHeartR, 1.0f, dt*0.9f);
    // Pulse: separate phase driver, adds small beat
    lovePulse += dt * 4.f;
  } else {
    lovestruck = false;
    // Shrink out quickly
    loveHeartL = elf(loveHeartL, 0, dt*4.f);
    loveHeartR = elf(loveHeartR, 0, dt*4.f);
    lovePulse = 0;
  }
}

// Called from renderEyes AFTER normal eye draw  overlays hearts when active
void drawLoveHearts(int lx, int ly, int rx, int ry, int rw, int rh){
  if(loveHeartL < 0.05f && loveHeartR < 0.05f) return;
  // Erase normal eye ellipses first, draw hearts in their place
  uint16_t pink = iAmA ? (uint16_t)0xF81F : (uint16_t)0xFB96;
  int lr = (int)(rw * loveHeartL);
  int rr = (int)(rw * loveHeartR);
  if(lr > 2){
    spr.fillEllipse(lx, ly, rw, rh, BLK); // erase
    sprHeartEye(lx, ly, lr, pink);
  }
  if(rr > 2){
    spr.fillEllipse(rx, ry, rw, rh, BLK);
    sprHeartEye(rx, ry, rr, pink);
  }
}

// 
//  BOOP  nose boop (tap exact centre)
// 
void triggerBoop(){
  boopActive = true;
  boopStage  = 0;
  boopCross  = 0;
  boopTimer  = millis();
  // Brief confused then surprised
  pushMood(SURPRISED); setExpr(SURPRISED);
  lastExpr=millis(); nextExprMs=random(3000,6000);
}

void updateBoop(float dt){
  if(!boopActive) return;
  unsigned long ms = millis();
  if(boopStage == 0){
    // Eyes cross inward
    boopCross = elf(boopCross, 1.f, dt*8.f);
    if(ms - boopTimer > 400){ boopStage=1; boopTimer=ms; }
  } else {
    // Pop back out
    boopCross = elf(boopCross, 0.f, dt*12.f);
    if(boopCross < 0.02f){ boopCross=0; boopActive=false; }
  }
}

// 
//  STROKE  slow drag, dreamy follow
// 
void updateStroke(float dt){
  unsigned long ms = millis();
  if(strokeMode){
    strokeDream = elf(strokeDream, 0.6f, dt*1.5f);
    // If no touch for 800ms, end stroke
    if(ms - lastStrokeMs > 800){ strokeMode=false; }
  } else {
    strokeDream = elf(strokeDream, 0, dt*2.f);
  }
}

// 
//  BOREDOM & ATTENTION
// 
void updateBoredom(){
  if(!wifiOK) return; // need millis only, always runs
  unsigned long ms = millis();
  unsigned long idle = ms - lastTouchTime;

  // Level thresholds
  int newLevel = 0;
  if(idle > 1800000UL) newLevel = 3;      // 30min  turned away
  else if(idle > 600000UL) newLevel = 2;  // 10min  flat stare
  else if(idle > 300000UL) newLevel = 1;  // 5min   sighing
  else newLevel = 0;

  if(newLevel != boredLevel){
    boredLevel = newLevel;
    switch(boredLevel){
      case 1:
        // Sigh  exasperated blink
        if(idleAnim==IA_NONE){ idleAnim=IA_EXABLINK; ist=0; idleTimer=ms; }
        break;
      case 2:
        // Flat stare  eyes slide to one side and hold
        dTX = 55.f; dTY = 10.f; // hard sideways
        if(idleAnim==IA_NONE){ idleAnim=IA_SIDEEYE; ist=0; idleTimer=ms; }
        pushMood(SUSPICIOUS); setExpr(SUSPICIOUS);
        lastExpr=ms; nextExprMs=60000;
        break;
      case 3:
        // Turned away  eyes max out to edge, SAD
        dTX = 60.f; dTY = 20.f;
        pushMood(SAD); setExpr(SAD);
        lastExpr=ms; nextExprMs=120000;
        break;
      case 0:
        // Came back to normal
        boredStare=false;
        break;
    }
  }

  // Attention-seeking hearts  idle for 10min+, float one heart up on its own
  if(idle > 600000UL && ms > nextAttentionHeart){
    // Spawn a single lonely heart from one eye
    spawnHeart((float)(BASE_ELX + random(-10,10)),
               (float)(spriteY + ECY - 10));
    nextAttentionHeart = ms + (unsigned long)random(15000, 30000);
  }
}

// 
//  DAY COUNTER
// 
void dayCounterLoad(){
  uint32_t firstBoot = 0;
  EEPROM.get(EEPROM_FIRST_BOOT, firstBoot);
  if(firstBoot == 0 || firstBoot == 0xFFFFFFFF){
    // First ever boot  record time
    struct tm ti; getLocalTime(&ti, 500);
    if(ti.tm_year > 100){ // valid time
      time_t now; time(&now);
      EEPROM.put(EEPROM_FIRST_BOOT, (uint32_t)now);
      EEPROM.commit();
      dayCount = 0;
    }
  } else {
    struct tm ti; getLocalTime(&ti, 500);
    if(ti.tm_year > 100){
      time_t now; time(&now);
      dayCount = (int)((now - (time_t)firstBoot) / 86400);
      if(dayCount < 0) dayCount = 0;
    }
  }
}

// 
//  SPEECH BUBBLE
// 

void bubbleErase(){
  tft.fillRect(0,0,SW,spriteY+4,BLK);
  bubbleNeedsErase=false;
}

// Draw rounded rect speech bubble with tail pointing down-left toward left eye
void bubbleDraw(){
  char raw[sizeof(bubbleText)];
  strncpy(raw,bubbleText,sizeof(raw)-1); raw[sizeof(raw)-1]=0;
  for(int i=0;raw[i];i++) if(raw[i]=='\n') raw[i]=' ';
  char clean[sizeof(bubbleText)]; int ci=0; bool ps=false;
  for(int i=0;raw[i];i++){
    if(raw[i]==' '){if(!ps&&ci>0){clean[ci++]=' ';} ps=true;}
    else{clean[ci++]=raw[i];ps=false;}
  }
  clean[ci]=0;
  int curSY=spriteY;
  const int BX=8,BY=4,BW=SW-16,BRAD=10;
  const int BH=curSY-12;
  uint16_t bCol=0x1082,tCol=0xFFFF,borCol=eyeColour;
  tft.fillRect(0,0,SW,curSY+2,BLK);
  if(BH<24){bubbleNeedsErase=true;return;}
  tft.fillRoundRect(BX,BY,BW,BH,BRAD,bCol);
  tft.drawRoundRect(BX,BY,BW,BH,BRAD,borCol);
  tft.drawRoundRect(BX+1,BY+1,BW-2,BH-2,BRAD,borCol);
  int tx=BASE_ELX,ty=curSY+6,tb=BY+BH;
  tft.fillTriangle(BX+14,tb,BX+34,tb,tx,ty,bCol);
  tft.drawLine(BX+14,tb,tx,ty,borCol);
  tft.drawLine(BX+34,tb,tx,ty,borCol);
  for(int sz=2;sz>=1;sz--){
    int charW=sz*6,lineH=sz*8+4,maxCh=(BW-20)/charW;
    char lns[8][52]; int nL=0; memset(lns,0,sizeof(lns));
    const char* p=clean;
    while(*p&&nL<8){
      while(*p==' ')p++; if(!*p)break;
      int li=0,lspi=-1; const char* lspp=nullptr;
      while(*p&&li<maxCh){
        if(*p==' '){lspi=li;lspp=p;}
        lns[nL][li++]=*p++;
      }
      if(*p&&*p!=' '&&lspi>=0){lns[nL][lspi]=0;p=lspp;}
      else{lns[nL][li]=0;}
      int tl=strlen(lns[nL]);
      while(tl>0&&lns[nL][tl-1]==' ') lns[nL][--tl]=0;
      nL++;
    }
    if(nL*lineH<=BH-12||sz==1){
      tft.setTextSize(sz); tft.setTextColor(tCol);
      int sy2=BY+(BH-nL*lineH)/2;
      for(int r=0;r<nL;r++){
        int lw=strlen(lns[r])*charW;
        tft.setCursor(BX+10+(BW-20-lw)/2,sy2+r*lineH);
        tft.print(lns[r]);
      }
      break;
    }
  }
  bubbleNeedsErase=true;
}

void bubblePick(bool forceNew){
  unsigned long ms = millis();

  // Pick a saying
  int idx = bubbleIdx;
  // Try to pick a different one
  for(int attempt=0; attempt<10; attempt++){
    // Time-of-day sayings: check hour
    struct tm ti; getLocalTime(&ti, 100);
    int hr = ti.tm_hour;
    int candidate;
    if(hr >= 22 || hr < 6)        candidate = SAYING_COUNT-4; // late
    else if(hr >= 6 && hr < 11)   candidate = SAYING_COUNT-3; // morning
    else if(hr >= 11 && hr < 15)  candidate = SAYING_COUNT-2; // midday
    else if(hr >= 17 && hr < 21)  candidate = SAYING_COUNT-1; // evening
    else                           candidate = random(SAYING_COUNT-4); // random

    if(candidate != idx || forceNew){ idx=candidate; break; }
  }
  bubbleIdx = idx;

  // Substitute %d with petsTotal if present
  if(strstr(sayings[idx], "%d")){
    snprintf(bubbleText, sizeof(bubbleText), sayings[idx], petsTotal);
  } else {
    strncpy(bubbleText, sayings[idx], sizeof(bubbleText)-1);
  }

  bubbleActive   = true;
  bubbleStartMs  = ms;
  chattingLook   = true;
  // Schedule next auto bubble
  nextBubbleMs = ms + bubbleShowMs + (unsigned long)random(120000, 300000); // 2-5min after this one
}

void bubbleTick(){
  unsigned long ms = millis();

  // Auto-trigger
  if(!bubbleActive && ms > nextBubbleMs && nextBubbleMs > 0){
    // 50/50 chance of live fetch vs local saying
    bool usedLive = false;
    if(wifiOK && random(2)==0){
      // Show brief loading indicator
      tft.fillRect(0,0,SW,SPR_Y+2,BLK);
      tft.setTextSize(1);
      tft.setTextColor(eyeColour);
      tft.setCursor(SW/2-24, spriteY/2-4);
      tft.print("thinking...");
      usedLive = fetchLiveBubble();
      if(usedLive){
        bubbleActive  = true;
        bubbleStartMs = ms;
        chattingLook  = true;
        nextBubbleMs  = ms + bubbleShowMs + (unsigned long)random(120000,300000);
        bubbleDraw();
      }
    }
    if(!usedLive){
      bubblePick(false);
      bubbleDraw();
    }
  }

  // Expire
  if(bubbleActive && ms - bubbleStartMs > bubbleShowMs){
    bubbleActive  = false;
    chattingLook  = false;
    bubbleErase();
    dTX = elf(dTX, 0, 0.3f);
  }
  // Also erase if we switched away from eyes page
  if(!bubbleActive && bubbleNeedsErase) bubbleErase();

  // Mood commentary: fire expression-matching bubble occasionally
  if(!bubbleActive && ms > nextMoodCommentMs && nextMoodCommentMs > 0
     && curExpr != lastCommentedExpr){
    const char* comment = nullptr;
    switch(curExpr){
      case ANGRY:      comment = (random(3)==0)?"fine.\nim fine.":
                                 (random(2)==0)?"do not\ntest me today":
                                 "i am not\nangry im\njust. fine."; break;
      case SUSPICIOUS: comment = (random(2)==0)?"i saw that":
                                 "something is\noff and i\nknow it"; break;
      case EMBARRASSED:comment = (random(2)==0)?"please ignore\nthe blush":
                                 "this is fine.\nim fine.\nno im not"; break;
      case SAD:        comment = (random(2)==0)?"its fine\neverything\nis fine":
                                 "just a little\nunder the\nweather"; break;
      case EXCITED:    comment = (random(2)==0)?"WAIT WAIT\nWAIT WAIT\nWAIT":
                                 "okay okay\ni am calm\ni am not calm"; break;
      case CONFUSED:   comment = (random(2)==0)?"wait what\nno wait\nhuh":
                                 "i have\nquestions"; break;
      case LOVESTRUCK: comment = (random(2)==0)?"hi hi hi\nhi hi hi\nhi":
                                 "oh no\noh no\noh no (good)"; break;
      case SLEEPY:     comment = (random(2)==0)?"just resting\nmy pixels":
                                 "five more\nminutes"; break;
      default: break;
    }
    if(comment){
      strncpy(bubbleText, comment, sizeof(bubbleText)-1);
      bubbleActive  = true;
      bubbleStartMs = ms;
      chattingLook  = true;
      lastCommentedExpr = curExpr;
      nextMoodCommentMs = ms + (unsigned long)random(180000, 480000); // 3-8 min
      nextBubbleMs = nextMoodCommentMs + bubbleShowMs;
      if(curPage == PAGE_EYES) bubbleDraw();
    }
  }
}

// Call once after setup to schedule first bubble
void bubbleInit(){
  nextBubbleMs = millis() + (unsigned long)random(30000, 90000); // first one 30-90s after boot
}

// 
//  LIVE FACT FETCHER
// 

// Word-wrap: break text at maxW chars per line using newline
void wordWrap(const char* text, int unused){
  strncpy(bubbleText,text,sizeof(bubbleText)-1);
  bubbleText[sizeof(bubbleText)-1]=0;
}

void decodeHtml(char* s){
  char buf[200]; strncpy(buf,s,199); buf[199]=0;
  char*r=buf, *w=s;
  while(*r){
    if(*r==0x26){ // &
      if(strncmp(r,"&amp;",5)==0)  { *w++=0x26; r+=5; }
      else if(strncmp(r,"&quot;",6)==0){ *w++=0x22; r+=6; } // double quote
      else if(strncmp(r,"&#039;",6)==0){ *w++=0x27; r+=6; } // single quote
      else if(strncmp(r,"&lt;",4)==0)  { *w++=0x3C; r+=4; }
      else if(strncmp(r,"&gt;",4)==0)  { *w++=0x3E; r+=4; }
      else { *w++=*r++; }
    } else { *w++=*r++; }
  }
  *w=0;
}

bool jsonStr(const char* json, const char* key, char* out, int maxLen){
  // Build search pattern: "key"
  char srch[64];
  int klen = strlen(key);
  srch[0] = 0x22; // double-quote
  strncpy(srch+1, key, 60);
  srch[klen+1] = 0x22;
  srch[klen+2] = 0;
  const char* p = strstr(json, srch);
  if(!p) return false;
  p += strlen(srch);
  // Skip whitespace and colon
  while(*p && (*p == 0x3A || *p == 0x20)) p++;
  // Expect opening quote
  if(*p != 0x22) return false;
  p++;
  int i = 0;
  while(*p && *p != 0x22 && i < maxLen-1){
    if(*p == 0x5C && *(p+1)){ // backslash escape
      p++;
      if(*p == 0x6E) out[i++] = 0x0A;      // \n
      else if(*p == 0x74) out[i++] = 0x20;  // \t -> space
      else out[i++] = *p;
    } else {
      out[i++] = *p;
    }
    p++;
  }
  out[i] = 0;
  return i > 0;
}

bool httpGetBubble(const char*url){
  if(WiFi.status()!=WL_CONNECTED)return false;
  WiFiClient client;HTTPClient http;
  http.begin(client,url);http.setTimeout(3000);
  http.addHeader("User-Agent","ESP32");
  http.addHeader("Accept","application/json");
  int code=http.GET();
  if(code!=200){http.end();return false;}
  String body=http.getString();http.end();
  strncpy(bubbleText,body.c_str(),sizeof(bubbleText)-1);
  bubbleText[sizeof(bubbleText)-1]='\0';
  return true;
}

bool fetchUselessFact(){
  if(!httpGetBubble("http://uselessfacts.jsph.pl/api/v2/facts/random?language=en"))return false;
  char fact[180]="";
  if(!jsonStr(bubbleText,"text",fact,sizeof(fact)))return false;
  decodeHtml(fact);wordWrap(fact,18);
  return strlen(bubbleText)>4;
}

bool fetchNumberFact(){
  struct tm ti;getLocalTime(&ti,200);
  char url[80];snprintf(url,sizeof(url),"http://numbersapi.com/%d/%d/date",ti.tm_mon+1,ti.tm_mday);
  WiFiClient client;HTTPClient http;
  http.begin(client,url);http.setTimeout(3000);
  http.addHeader("User-Agent","ESP32");
  int code=http.GET();
  if(code!=200){http.end();return false;}
  String body=http.getString();http.end();
  wordWrap(body.c_str(),18);
  return strlen(bubbleText)>4;
}

bool fetchTrivia(){
  if(!httpGetBubble("http://opentdb.com/api.php?amount=1&type=boolean"))return false;
  char q[180]="";
  if(!jsonStr(bubbleText,"question",q,sizeof(q)))return false;
  decodeHtml(q);char ans[10]="";
  jsonStr(bubbleText,"correct_answer",ans,sizeof(ans));
  char combined[200]="";
  snprintf(combined,sizeof(combined),"%s",q);
  wordWrap(combined,18);
  if(strlen(ans)>0&&strlen(bubbleText)+10<sizeof(bubbleText)){
    strncat(bubbleText,"\n(",sizeof(bubbleText)-strlen(bubbleText)-1);
    strncat(bubbleText,ans,sizeof(bubbleText)-strlen(bubbleText)-1);
    strncat(bubbleText,"?)",sizeof(bubbleText)-strlen(bubbleText)-1);
  }
  return strlen(bubbleText)>4;
}

bool fetchAdvice(){
  if(!httpGetBubble("http://api.adviceslip.com/advice"))return false;
  char advice[180]="";
  if(!jsonStr(bubbleText,"advice",advice,sizeof(advice)))return false;
  decodeHtml(advice);wordWrap(advice,18);
  return strlen(bubbleText)>4;
}

bool fetchJoke(){
  if(!httpGetBubble("http://official-joke-api.appspot.com/random_joke"))return false;
  char setup[90]="",punchline[90]="";
  if(!jsonStr(bubbleText,"setup",setup,sizeof(setup)))return false;
  jsonStr(bubbleText,"punchline",punchline,sizeof(punchline));
  decodeHtml(setup);decodeHtml(punchline);
  char combined[200]="";
  snprintf(combined,sizeof(combined),"%s\n%s",setup,punchline);
  wordWrap(combined,18);
  return strlen(bubbleText)>4;
}

//  API 6: Wikipedia random article intro 
bool fetchWikipedia(){
  // Get a random Wikipedia article summary
  if(!httpGetBubble("http://en.wikipedia.org/api/rest_v1/page/random/summary"))
    return false;
  char title[80]="", extract[200]="";
  if(!jsonStr(bubbleText,"title",title,sizeof(title))) return false;
  // Try "extract" for the first sentence
  if(!jsonStr(bubbleText,"extract",extract,sizeof(extract))) return false;
  // Trim to first sentence (stop at first period)
  char* period = strchr(extract, '.');
  if(period && (period - extract) < 140) *(period+1) = 0;
  decodeHtml(extract);
  // Prefix with title
  char combined[220]="";
  snprintf(combined, sizeof(combined), "%s:\n%s", title, extract);
  wordWrap(combined, 18);
  return strlen(bubbleText) > 4;
}

//  API 7: OpenWeatherMap current weather 
bool fetchWeather(){
  if(strlen(WEATHER_KEY) < 8) return false; // no key set
  char url[160];
  snprintf(url, sizeof(url),
    "http://api.openweathermap.org/data/2.5/weather?q=%s&appid=%s&units=%s",
    WEATHER_CITY, WEATHER_KEY, WEATHER_UNITS);
  if(!httpGetBubble(url)) return false;
  // Extract description and temp
  char desc[60]="", temp[12]="";
  jsonStr(bubbleText,"description",desc,sizeof(desc));
  jsonStr(bubbleText,"temp",temp,sizeof(temp));
  if(strlen(desc)<2) return false;
  // Build a reaction based on weather
  char reaction[120]="";
  bool hot   = (atof(temp) > (strcmp(WEATHER_UNITS,"imperial")==0 ? 90 : 32));
  bool cold  = (atof(temp) < (strcmp(WEATHER_UNITS,"imperial")==0 ? 32 : 0));
  bool rainy = (strstr(desc,"rain")!=nullptr || strstr(desc,"drizzle")!=nullptr);
  bool sunny = (strstr(desc,"clear")!=nullptr || strstr(desc,"sun")!=nullptr);
  bool snowy = (strstr(desc,"snow")!=nullptr);
  if(snowy)       snprintf(reaction,sizeof(reaction),"its snowing\noutside\n...magical");
  else if(rainy)  snprintf(reaction,sizeof(reaction),"its raining\noutside\ncozy in here");
  else if(hot)    snprintf(reaction,sizeof(reaction),"%sdeg outside\nunacceptable\ntoo hot",temp);
  else if(cold)   snprintf(reaction,sizeof(reaction),"%sdeg outside\nstay inside\nwith me",temp);
  else if(sunny)  snprintf(reaction,sizeof(reaction),"sunny outside\n%sdeg\nnot bad",temp);
  else            snprintf(reaction,sizeof(reaction),"%s\noutside\ninteresting",desc);
  strncpy(bubbleText, reaction, sizeof(bubbleText)-1);
  return strlen(bubbleText) > 4;
}

bool fetchLiveBubble(){
  if(!wifiOK||WiFi.status()!=WL_CONNECTED)return false;
  // 7 sources -- weather weighted lower since needs API key
  int which=random(14);
  switch(which){
    case 0: case 7:  return fetchUselessFact();
    case 1: case 8:  return fetchNumberFact();
    case 2: case 9:  return fetchTrivia();
    case 3: case 10: return fetchAdvice();
    case 4: case 11: return fetchJoke();
    case 5: case 12: return fetchWikipedia();
    case 6: case 13: return fetchWeather();
  }
  return false;
}

// 
//  COMPANION WEB SERVER
// 

void webServerStart(){
  if(!wifiOK||webServerStarted) return;
  webServer.begin(); webServerStarted=true;
  Serial.printf("Web: http://%s/\n",WiFi.localIP().toString().c_str());
}

const char* exprName(Expr e){
  const char* n[]={"NORMAL","HAPPY","SAD","ANGRY","SURPRISED","SUSPICIOUS",
    "SLEEPY","CONFUSED","EXCITED","EMBARRASSED","LOVESTRUCK"};
  return (e<11)?n[e]:"UNKNOWN";
}

void webServerTick(){
  if(!webServerStarted) return;
  WiFiClient client=webServer.available();
  if(!client) return;
  unsigned long t=millis();
  String req="";
  while(client.connected()&&millis()-t<1000){
    if(client.available()){char c=client.read();req+=c;
      if(req.endsWith("\r\n\r\n"))break;}
  }
  String body="";
  if(req.startsWith("POST")){
    int clen=0;int ci=req.indexOf("Content-Length:");
    if(ci>=0)clen=req.substring(ci+15).toInt();
    unsigned long bt=millis();
    while((int)body.length()<clen&&millis()-bt<2000){
      if(client.available())body+=(char)client.read();}
  }
  if(req.startsWith("GET /stats")||req.startsWith("GET / ")){
    char json[400];
    snprintf(json,sizeof(json),
      "{\"name\":\"%s\",\"petsToday\":%d,\"petsTotal\":%d,"
      "\"dayCount\":%d,\"expression\":\"%s\","
      "\"friendOnline\":%s,\"friendName\":\"%s\","
      "\"friendPetsToday\":%d,\"friendPetsTotal\":%d}",
      robotName,petsToday,petsTotal,dayCount,exprName(curExpr),
      friendOnline?"true":"false",friendName,
      friendPetsToday,friendPetsTotal);
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Connection: close");
    client.println();
    client.println(json);
  } else if(req.startsWith("POST /bubble")){
    String msg=body;
    int ti=msg.indexOf("text=");
    if(ti>=0)msg=msg.substring(ti+5);
    msg.replace("+"," ");msg.replace("%21","!");
    msg.replace("%2C",",");msg.replace("%3F","?");msg.replace("%0A","\n");
    if(msg.length()>2){
      wordWrap(msg.c_str(),18);
      bubbleActive=true;bubbleStartMs=millis();chattingLook=true;
      nextBubbleMs=millis()+bubbleShowMs+60000;
      if(curPage==PAGE_EYES)bubbleDraw();
    }
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/plain");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Connection: close");
    client.println();
    client.println("ok");
  } else {
    client.println("HTTP/1.1 404 Not Found");
    client.println("Connection: close");
    client.println();
  }
  delay(1);client.stop();
}

// 
//  TOUCH HELPERS
// 
// Map raw XPT2046 coords  screen pixels for landscape rotation 1
void getTouchXY(int16_t &sx, int16_t &sy){
  TS_Point p = ts.getPoint();
  // On ESP32-2432S028R in rotation 1: raw X maps to screen Y, raw Y maps to screen X
  sx = map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SW);
  sy = map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, SH);
  sx = constrain(sx, 0, SW-1);
  sy = constrain(sy, 0, SH-1);
}

// Called every loop  full state machine for touch/swipe/tap
void handleTouch(){
  unsigned long ms = millis();
  bool pressed = ts.touched();

  if(pressed && ms - lastTouchMs > TOUCH_DEBOUNCE){
    lastTouchMs = ms;
    int16_t tx, ty;
    getTouchXY(tx, ty);
    touchCurX = tx; touchCurY = ty;

    if(touchState == TS_IDLE){
      touchState  = TS_DOWN;
      touchDownX  = tx; touchDownY = ty;
      touchDownMs = ms;
      touchIsDown = true;
      holdTriggered = false;
      lastTouchTime = ms;
      boredLevel    = 0;
      strokeStartMs = 0;
      touchLookX = map(tx, 0, SW, -30, 30);
      touchLookY = map(ty, 0, SH, -20, 20);
      touchReacting = true; touchReactTimer = ms;
    } else {
      // Still held  update eye tracking
      touchLookX = map(tx, 0, SW, -30, 30);
      touchLookY = map(ty, 0, SH, -20, 20);
      touchState = TS_HELD;
      // Stroke detection -- use live positions, vars always in scope
      {
        unsigned long heldSoFar = ms - touchDownMs;
        int liveDX = touchCurX - touchDownX;
        int liveADX = abs(liveDX);
        if(heldSoFar > 200 && liveADX > 20 && heldSoFar < 2000 && curPage==PAGE_EYES){
          strokeMode=true; strokeDir=(liveDX>0)?1.f:-1.f; lastStrokeMs=ms;
          if(strokeStartMs==0) strokeStartMs=ms;
          if(ms-strokeStartMs>600 && curExpr!=SLEEPY && curExpr!=LOVESTRUCK){
            pushMood(HAPPY); setExpr(HAPPY); lastExpr=ms; nextExprMs=random(5000,9000);
          }
        } else { strokeStartMs=0; }
      }

      // Hold anticipation  eyes widen progressively on eyes page
      if(curPage == PAGE_EYES && !holdTriggered){
        unsigned long heldMs = ms - touchDownMs;
        holdAntScale = cf((float)heldMs / 1800.f, 0, 1.0f);
        // At 800ms cross  face shows anticipation expression
        if(heldMs > 800 && heldMs < 820){
          pushMood(SURPRISED); setExpr(SURPRISED);
        }
      }
    }

  } else if(!pressed && touchState != TS_IDLE){
    unsigned long held = ms - touchDownMs;
    int16_t dx = touchCurX - touchDownX;
    int16_t dy = touchCurY - touchDownY;
    int16_t adx = abs(dx), ady = abs(dy);

    //  Swipe 
    if(adx > SWIPE_THRESH && adx > ady && held < SWIPE_MS){
      curPage = (Page)((curPage + 1) % 2);
      tft.fillScreen(BLK);

    //  Hold release (1.5s+)  sneeze/shake with relief 
    } else if(held >= 1500 && adx < TAP_MAX_MOVE*3 && !holdTriggered){
      holdTriggered = true;
      holdAntScale  = 0;
      if(curPage == PAGE_EYES){
        // Relief shake or sneeze
        if(random(2)){
          idleAnim=IA_SNEEZE; sneezeStep=0; shakeX=0; shakeY=0;
          sneezeTimer=ms; idleTimer=ms;
        } else {
          idleAnim=IA_STARTLED; startledSc=0; ist=0; idleTimer=ms;
        }
        pushMood(NORMAL); setExpr(NORMAL);
        lastExpr=ms; nextExprMs=random(5000,9000);
      }

    //  Tap 
    } else if(adx < TAP_MAX_MOVE && ady < TAP_MAX_MOVE && held < TAP_MAX_MS){
      holdAntScale = 0;
      if(curPage == PAGE_EYES){

        //  BUBBLE SKIP: tap while bubble showing  next saying 
        if(bubbleActive){
          bubbleErase();
          bubblePick(true);
          bubbleDraw();
          // Reset timers so it stays for full duration
          bubbleStartMs = millis();
        }

        //  OVERSTIMULATED: block all input during rest 
        if(overstimulated){
          // Completely ignore taps during rest period
        } else {

          //  COUNT CONSECUTIVE DOUBLE-TAPS 
          bool isDoubleTap  = (ms - lastTapMs  < DOUBLE_TAP_MS);
          bool isTripleTap  = false;
          lastTapMs = ms;

          // Triple-tap: track 3 taps within TRIPLE_TAP_MS
          if(ms - firstTap3Ms < TRIPLE_TAP_MS){
            tapCount3++;
          } else {
            tapCount3 = 1;
            firstTap3Ms = ms;
          }
          if(tapCount3 >= 3){ isTripleTap=true; tapCount3=0; firstTap3Ms=0; }

          if(isDoubleTap){
            consecutiveDoubleTaps++;
            lastDoubleTapMs = ms;
          } else if(ms - lastDoubleTapMs > 1000){
            consecutiveDoubleTaps = 0;
          }

          //  OVERSTIMULATED TRIGGER (5 double-taps) 
          if(consecutiveDoubleTaps >= 5){
            consecutiveDoubleTaps = 0;
            overstimulated = true;
            overstimEndMs  = ms + OVERSTIM_REST_MS;
            // Chaos: hearts + sparks at same time
            for(int h=0;h<6;h++) spawnHeart((float)random(20,300),(float)random(20,200));
            for(int s=0;s<MAX_SPARKS;s++) spawnSparks((float)random(20,300),(float)random(20,200));
            spawnStars(SW/2, SH/2);
            idleAnim=IA_DIZZY; dizzyA=0; dizzyR=0; ist=0; idleTimer=ms;
            pushMood(CONFUSED); setExpr(CONFUSED);
            lastExpr=ms; nextExprMs=OVERSTIM_REST_MS;

          //  TRIPLE-TAP: dizzy stars 
          } else if(isTripleTap){
            spawnStars((float)touchDownX,(float)touchDownY);
            idleAnim=IA_DIZZY; dizzyA=0; dizzyR=0; ist=0; idleTimer=ms;
            pushMood(CONFUSED); setExpr(CONFUSED);
            lastExpr=ms; nextExprMs=random(4000,8000);

          //  SPOOKY MODE: tap at midnight 
          } else if(!isDoubleTap && !isTripleTap){
            // Check time  midnight = 0:00
            struct tm ti; getLocalTime(&ti, 100);
            if(ti.tm_hour==0 && ti.tm_min==0 && !spookyMode){
              spookyMode   = true;
              spookyEndMs  = ms + SPOOKY_DURATION;
              setRGB(0,0,0);
              eyeColour    = 0x07E0; // green
              lastExpr=ms; nextExprMs=SPOOKY_DURATION;
            }
          }

          //  DOUBLE-TAP: wide-eye surprise + hearts 
          if(isDoubleTap && !isTripleTap && consecutiveDoubleTaps < 5){
            idleAnim=IA_STARTLED; startledSc=0; ist=0; idleTimer=ms;
            if(spookyMode){ pushMood(ANGRY); setExpr(ANGRY); }
            else { pushMood(SURPRISED); setExpr(SURPRISED); }
            for(int h=0;h<6;h++) spawnHeart((float)touchDownX,(float)touchDownY);
            lastExpr=ms; nextExprMs=random(5000,10000);

          //  TAP WHILE ANGRY: sparks 
          } else if(!isDoubleTap && !isTripleTap && curExpr==ANGRY){
            for(int s=0;s<MAX_SPARKS;s++) spawnSparks((float)touchDownX,(float)touchDownY);
            idleAnim=IA_SQUINT; squintEye=2; squintAmt=0; ist=0; idleTimer=ms;
            lastExpr=ms; nextExprMs=random(3000,5000);

          //  BOOP or NORMAL TAP 
          } else if(!isDoubleTap && !isTripleTap){
            int distCentre = abs(touchDownX - SW/2) + abs(touchDownY - SH/2);
            if(distCentre < 22){
              triggerBoop();
            } else {
            int n = random(2,5);
            for(int h=0;h<n;h++) spawnHeart((float)touchDownX,(float)touchDownY);
            if(spookyMode){
              // In spooky mode, pet makes eyes flash bright green briefly
              eyeColour = 0x87F0;
              idleAnim=IA_STARTLED; startledSc=0; ist=0; idleTimer=ms;
            } else {
              pushMood(HAPPY); setExpr(HAPPY);
              idleAnim=IA_PETJOY;
              petJoyScale=0; petJoySquish=0;
              petJoyBounce=0; petJoyPhase=0;
              petJoyStage=0; idleTimer=ms;
              dTX=0; dTY=0;
              lastExpr=ms; nextExprMs=random(8000,15000);
            }
            eepromSavePet();
            } // end boop else
          }
        } // end !overstimulated
      }
    }

    touchIsDown = false;
    touchState  = TS_IDLE;
    touchReactTimer = ms;
  }

  // Timeout stale hold
  if(touchState != TS_IDLE && ms - touchDownMs > 5000){
    touchIsDown = false; touchState = TS_IDLE;
    holdAntScale = 0;
  }

  // Decay holdAntScale when not held
  if(!touchIsDown && holdAntScale > 0){
    holdAntScale = elf(holdAntScale, 0, 0.08f);
    if(holdAntScale < 0.01f) holdAntScale = 0;
  }

  // Fade out touch look
  if(!touchIsDown && touchReacting){
    if(ms - touchReactTimer > 600){
      touchLookX = elf(touchLookX, 0, 0.04f);
      touchLookY = elf(touchLookY, 0, 0.04f);
      if(fabsf(touchLookX)<0.5f && fabsf(touchLookY)<0.5f){
        touchLookX=0; touchLookY=0; touchReacting=false;
      }
    }
  }
}

// 
//  WEATHER FETCH  (OpenWeatherMap free tier, no key needed
//  for basic current conditions via wttr.in JSON)
// 
// 
//  DRAW CLOCK PAGE
// 
void drawClock(){
  tft.fillScreen(BLK);
  struct tm ti;
  if(!getLocalTime(&ti, 200)){
    tft.setTextColor(0x630C); tft.setTextSize(2);
    tft.setCursor(80,110); tft.print("No time sync");
    return;
  }

  // Time  big centred
  char tbuf[10];
  int hr = ti.tm_hour % 12; if(hr==0) hr=12;
  snprintf(tbuf, sizeof(tbuf), "%2d:%02d", hr, ti.tm_min);
  tft.setTextSize(5);
  tft.setTextColor(eyeColour);
  int tw = strlen(tbuf)*6*5; // rough width
  tft.setCursor((SW-tw)/2, 30);
  tft.print(tbuf);

  // AM/PM
  tft.setTextSize(2);
  tft.setTextColor(0x7BEF);
  tft.setCursor((SW+tw)/2 - 4, 42);
  tft.print(ti.tm_hour>=12?"PM":"AM");

  // Date line
  const char* days[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  const char* months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  char dbuf[24];
  snprintf(dbuf, sizeof(dbuf), "%s  %s %d  %d",
    days[ti.tm_wday], months[ti.tm_mon], ti.tm_mday, ti.tm_year+1900);
  tft.setTextSize(2);
  tft.setTextColor(0x7BEF);
  int dw = strlen(dbuf)*6*2;
  tft.setCursor((SW-dw)/2, 110);
  tft.print(dbuf);

  // Divider
  tft.drawFastHLine(20, 135, SW-40, 0x2945);

  //  My name + pets 
  tft.setTextSize(2);
  tft.setTextColor(eyeColour);
  char nameBuf[24];
  snprintf(nameBuf, sizeof(nameBuf), "%s", robotName);
  int nw = strlen(nameBuf)*6*2;
  tft.setCursor((SW-nw)/2, 148);
  tft.print(nameBuf);

  tft.setTextSize(1);
  tft.setTextColor(0x7BEF);
  char petBuf[32];
  snprintf(petBuf, sizeof(petBuf), "today: %d  total: %d", petsToday, petsTotal);
  int pw = strlen(petBuf)*6;
  tft.setCursor((SW-pw)/2, 168);
  tft.print(petBuf);
  tft.setTextColor(0x2945);
  char dayBuf[16];
  snprintf(dayBuf,sizeof(dayBuf),"day %d",dayCount);
  int dw2=strlen(dayBuf)*6;
  tft.setCursor((SW-dw2)/2,178);
  tft.print(dayBuf);

  //  Divider 
  tft.drawFastHLine(20, 182, SW-40, 0x2945);

  //  Friend section 
  if(friendOnline && strlen(friendName) > 0){
    tft.fillCircle(14, 196, 4, 0x07E0); // green online dot
    tft.setTextSize(2);
    tft.setTextColor(0xAFE5);
    tft.setCursor(24, 190);
    tft.print(friendName);
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);
    char fbuf[32];
    snprintf(fbuf, sizeof(fbuf), "today: %d  total: %d", friendPetsToday, friendPetsTotal);
    int fw = strlen(fbuf)*6;
    tft.setCursor((SW-fw)/2, 210);
    tft.print(fbuf);
    tft.setTextSize(1);
    tft.setTextColor(0x2945);
    tft.setCursor(68, 228);
    tft.print("tap friend name to poke!");
  } else {
    tft.fillCircle(14, 196, 4, 0x6000); // dim red offline dot
    tft.setTextSize(1);
    tft.setTextColor(0x2945);
    tft.setCursor(26, 192);
    if(strlen(friendName) > 0){
      char offbuf[32];
      snprintf(offbuf, sizeof(offbuf), "%s is offline", friendName);
      tft.print(offbuf);
    } else {
      tft.print("waiting for friend...");
    }
    tft.setCursor(106, 228);
    tft.print("swipe: eyes");
  }
}

// 
//  WAKE ANIMATION
//  Eyes start fully closed, RGB off. Sequence:
//  1. Backlight fades in (0255, 600ms)
//  2. Lids flicker slightly  half-open, close again (like REM)
//  3. Slow open to ~30%, hold 400ms (groggy)
//  4. Close again briefly (200ms)
//  5. Open fully with a small overshoot
//  6. RGB fades to expression colour
// 
void wakeUp(){
  // Start with lids fully shut, backlight off
  cLTI=1.f;cLTO=1.f;cRTI=1.f;cRTO=1.f;
  cLBI=0.f;cLBO=0.f;cRBI=0.f;cRBO=0.f;
  cRhS=1.0f; blinkT=0;
  analogWrite(LCD_BL_PIN,0);
  setRGB(0,0,0);

  // Stage 1  backlight fades in while eyes stay shut
  for(int i=0;i<=255;i+=3){
    analogWrite(LCD_BL_PIN,i);
    renderEyes();
    delay(7);
  }
  analogWrite(LCD_BL_PIN,255);
  delay(200);

  // Stage 2  REM flicker: half-open, close
  for(int f=0;f<3;f++){
    // flutter open to ~40%
    for(float t=1.f;t>0.60f;t-=0.04f){
      cLTI=t;cLTO=t;cRTI=t;cRTO=t;
      renderEyes(); delay(16);
    }
    // close back
    for(float t=0.60f;t<1.f;t+=0.06f){
      cLTI=t;cLTO=t;cRTI=t;cRTO=t;
      renderEyes(); delay(16);
    }
    delay(random(60,160));
  }

  // Stage 3  groggy half-open
  for(float t=1.f;t>0.55f;t-=0.025f){
    cLTI=t;cLTO=t;cRTI=t;cRTO=t;
    renderEyes(); delay(22);
  }
  delay(500);

  // Stage 4  close again (resisting waking)
  for(float t=0.55f;t<1.f;t+=0.05f){
    cLTI=t;cLTO=t;cRTI=t;cRTO=t;
    renderEyes(); delay(18);
  }
  delay(300);

  // Stage 5  open fully, with slight overshoot
  // Expression lids are all 0 for NORMAL  open means lids go to 0
  for(float t=1.f;t>-0.08f;t-=0.03f){ // overshoot past 0
    float v=max(t,0.f);
    cLTI=v;cLTO=v;cRTI=v;cRTO=v;
    renderEyes(); delay(14);
  }
  // Settle back from overshoot
  cLTI=0;cLTO=0;cRTI=0;cRTO=0;
  // Brief wide-open hold
  delay(180);

  // Stage 6  RGB fades in to expression colour
  // setExpr already called, so eyeColour is set.
  // Fade RGB from 0 to target over 800ms
  uint8_t tr,tg,tb;
  switch(curExpr){
    case HAPPY:      tr=0;  tg=200;tb=60;  break;
    case SAD:        tr=0;  tg=0;  tb=180; break;
    case ANGRY:      tr=220;tg=0;  tb=0;   break;
    case SURPRISED:  tr=200;tg=150;tb=0;   break;
    case SUSPICIOUS: tr=100;tg=0;  tb=200; break;
    case SLEEPY:     tr=20; tg=20; tb=90;  break;
    default:         tr=0;  tg=80; tb=80;  break;
  }
  for(int i=0;i<=20;i++){
    float f=i/20.f;
    setRGB((uint8_t)(tr*f),(uint8_t)(tg*f),(uint8_t)(tb*f));
    delay(40);
  }
}

void setup(){
  Serial.begin(115200);
  pinMode(LCD_BL_PIN,OUTPUT);
  analogWrite(LCD_BL_PIN,0); // start dark
  pinMode(RGB_R,OUTPUT);pinMode(RGB_G,OUTPUT);pinMode(RGB_B,OUTPUT);
  setRGB(0,0,0);
  tft.init();tft.setRotation(1);tft.fillScreen(BLK);
  // Touch on HSPI
  hSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(hSPI);
  ts.setRotation(1);
  spr.createSprite(SPR_W,SPR_H);spr.setColorDepth(16);
  Serial.print("Heap: ");Serial.println(ESP.getFreeHeap());
  initWifi();pollTime();randomSeed(analogRead(0));
  if(wifiOK){ udp.begin(FRIEND_PORT); udpStarted=true; webServerStart(); }
  setExpr(timeExpr());
  nextBlink=random(3000,5000);bTimer=millis();
  lastExpr=millis();nextExprMs=random(5000,9000);
  nextIdle=random(7000,12000);
  ldX=millis();ldY=millis();dpX=random(3500,6000);dpY=random(4000,7000);
  lastBigMove=millis();
  prevMs=millis();
  initHearts();
  initSparks();
  initZzls();
  initStars();
  initConfetti();
  checkSeason();
  eepromLoad();
  checkSeason();
  bubbleInit();
  wakeUp(); // boot sequence before entering main loop
}

// 
//  LOOP
// 
void loop(){
  unsigned long ms=millis();
  float dt=cf((ms-prevMs)/1000.f,0.001f,0.05f);
  prevMs=ms;

  // Handle touch / swipe
  handleTouch();

  // Friend networking tick
  if(wifiOK) friendTick();
  if(wifiOK) webServerTick();

  // Non-eye pages  draw and yield, don't run eye logic
  if(curPage == PAGE_CLOCK){
    static unsigned long lastClockDraw = 0;
    if(ms - lastClockDraw > 1000){ lastClockDraw = ms; drawClock(); }
    // Poke friend by tapping their name area (y=185-215 on clock screen)
    if(friendOnline && touchState==TS_IDLE && !touchIsDown){
      static bool pokeArmed = true;
      if(pokeArmed && ts.touched()){
        int16_t tx,ty; getTouchXY(tx,ty);
        if(ty > 185 && ty < 218 && tx > 10 && tx < 280){
          friendSend(PKT_POKE);
          pokeArmed = false;
          // Flash the friend name area pink briefly
          tft.fillRect(10,185,SW-20,30,0xF81F);
          delay(80);
          drawClock();
          lastClockDraw = millis();
          Serial.println("Poked friend!");
        }
      } else if(!ts.touched()){
        pokeArmed = true;
      }
    }
    unsigned long spent=millis()-ms; if(spent<25) delay(25-spent);
    return;
  }

  // Time poll + seasonal check
  if(wifiOK&&ms-lastTimePoll>60000){
    lastTimePoll=ms;
    if(pollTime()&&ms-lastExpr>nextExprMs/2) setExpr(timeExpr());
    checkSeason();
  }

  // Expression cycle
  if(ms-lastExpr>nextExprMs){
    lastExpr=ms;nextExprMs=random(5000,12000);
    Expr next=pickExpr(timeExpr());
    pushMood(next);setExpr(next);
    if(curExpr!=ANGRY) twitchOn=false;
    if(curExpr!=SAD&&tearOn){eraseTearCol((int)tearX,SPR_Y+SPR_H,(int)tearDrawY+2);tearOn=false;}
  }

  //  Easter egg state management 
  // Overstimulated expires  rest is over
  if(overstimulated && ms >= overstimEndMs){
    overstimulated = false;
    consecutiveDoubleTaps = 0;
    pushMood(NORMAL); setExpr(NORMAL);
    lastExpr=ms; nextExprMs=random(5000,9000);
  }
  // Spooky mode expires
  if(spookyMode && ms >= spookyEndMs){
    spookyMode = false;
    pushMood(NORMAL); setExpr(NORMAL);
    lastExpr=ms; nextExprMs=random(5000,9000);
  }
  // Seasonal colours (overridden by spooky/milestone below)
  applySeasonalColour();
  // Spooky locks green on top of seasonal
  if(spookyMode) eyeColour = iAmA ? 0x07E0 : 0xF41F; // Sparks: green / Buddy: pink-purple spooky
  // Milestone update (RGB rainbow etc)
  updateMilestone();

  // Expression lerp with stagger + transition pause
  updateExprLerp(dt);
  updateLovestruck(dt);
  updateBoop(dt);
  updateStroke(dt);
  if(curPage==PAGE_EYES) updateBoredom();
  if(curPage==PAGE_EYES) bubbleTick();

  // CONFUSED: one eye drifts higher than the other
  float confTarget = (curExpr==CONFUSED) ? 10.f : 0.f;
  confusedOff = elf(confusedOff, confTarget, dt*2.f);

  // EXCITED: constant fast small bounce + occasional RGB flash
  if(curExpr==EXCITED){
    excitedPhase += dt*18.f;     // slower oscillation
    excitedBounce = sinf(excitedPhase)*2.5f;  // smaller amplitude
  } else {
    excitedBounce = elf(excitedBounce, 0, dt*6.f);
    if(fabsf(excitedBounce)<0.1f) excitedBounce=0;
  }

  // EMBARRASSED: eyes drift downward + blush grows
  float blushTarget = (curExpr==EMBARRASSED) ? 1.f : 0.f;
  embarrassBlush = elf(embarrassBlush, blushTarget, dt*1.5f);

  // Breathing  speed linked to mood
  float bspd;
  switch(curExpr){case SLEEPY:bspd=0.35f;break;case SURPRISED:bspd=1.8f;break;case ANGRY:bspd=1.4f;break;case EXCITED:bspd=2.2f;break;default:bspd=0.8f;}
  // EXCITED: strobe RGB rapidly between magenta and white
  if(curExpr==EXCITED){
    static unsigned long lastStrobe=0;
    if(millis()-lastStrobe>120){ lastStrobe=millis();
      static bool strobeFlip=false; strobeFlip=!strobeFlip;
      if(strobeFlip) setRGB(200,0,200); else setRGB(255,200,255);
    }
  }
  breathPhase+=dt*bspd;
  if(breathPhase>2*PI) breathPhase-=2*PI;

  //  Momentum drift X 
  // Suppress autonomous drift while finger is actively held
  if(touchIsDown){ velX*=0.85f; velY*=0.85f; } // damp momentum while touching
  // Attention variance: longer since big move = more likely to do a large one
  unsigned long stillFor=ms-lastBigMove;

  if(ms-ldX>dpX){
    ldX=ms;
    float attentionBoost=(stillFor>20000)?1.4f:1.0f; // still 20s = bigger look
    switch(curExpr){
      case ANGRY:    dpX=random(2000,3500); dTX=(float)(random(2)?1:-1)*random(20,45); break;
      case SLEEPY:   dpX=random(10000,18000);dTX=(float)random(-8,8); break;
      case HAPPY:    dpX=random(3000,6000); dTX=lf(dTX,(float)random(-35,35),0.5f); break;
      case SURPRISED:dpX=random(2000,4000); dTX=(float)(random(2)?1:-1)*random(15,40); break;
      case CONFUSED: dpX=random(3000,6000); dTX=(float)(random(2)?1:-1)*random(10,30); break;
      case EXCITED:  dpX=random(1500,3000); dTX=(float)(random(2)?1:-1)*random(15,40); break;
      default:{dpX=random(6000,12000);int r=random(10);
        // Most moves are small, occasional big glances
        float rng=(r<6)?random(8,20):(r<9)?random(22,40):random(42,60);
        dTX=(float)(random(2)?1:-1)*rng*attentionBoost;break;}
    }
    if(fabsf(dTX)>35) lastBigMove=ms;
  }

  //  Momentum drift Y 
  if(ms-ldY>dpY){
    ldY=ms;
    float attentionBoost=(stillFor>8000)?1.6f:1.0f;
    switch(curExpr){
      case SLEEPY:    dpY=random(8000,14000); dTY=(float)random(20,38);  break; // droops down
      case SAD:       dpY=random(5000,9000);  dTY=(float)random(15,35);  break; // looks down
      case HAPPY:     dpY=random(3000,6000);  dTY=(float)random(-20,30)*attentionBoost; break;
      case ANGRY:     dpY=random(2000,4000);  dTY=(float)(random(2)?1:-1)*random(12,28); break;
      case SURPRISED: dpY=random(2000,4000);  dTY=(float)random(-12,-4); break; // looks UP
      case CONFUSED:  dpY=random(3000,6000);  dTY=(float)(random(2)?1:-1)*random(8,24);  break;
      case EXCITED:   dpY=random(1500,3000);  dTY=(float)(random(2)?1:-1)*random(12,28); break;
      case EMBARRASSED:dpY=random(4000,8000); dTY=(float)random(18,36); break; // looks down
      default:{dpY=random(6000,12000);int r=random(10);
        // Mostly small, occasional big look up or down
        float rng=(r<6)?random(5,16):(r<9)?random(18,30):random(30,38);
        dTY=(float)(random(2)?1:-1)*rng*attentionBoost;break;}
    }
    if(fabsf(dTY)>24) lastBigMove=ms;
  }

  // Apply momentum: velocity driven toward target, with drag
  // This gives the eye mass  it accelerates and decelerates naturally
  float targetVX=(dTX-driftX)*1.4f;  // softer spring
  float targetVY=(dTY-driftY)*1.4f;
  float drag;
  switch(curExpr){case ANGRY:drag=0.88f;break;case SLEEPY:drag=0.96f;break;case HAPPY:drag=0.91f;break;default:drag=0.93f;}
  velX=lf(velX,targetVX,dt*2.2f)*drag;
  velY=lf(velY,targetVY,dt*2.2f)*drag;
  driftX+=velX*dt;
  driftY+=velY*dt;
  // Clamp so eyes never leave sprite bounds
  driftX=cf(driftX,-62.f,62.f);
  driftY=cf(driftY,-10.f,36.f);  // Y: -10=look up, 36=look down
  dTX=cf(dTX,-62.f,62.f);
  dTY=cf(dTY,-10.f,36.f);

  // Overshoot: only on fast large moves, small kick
  if(!didOvershootX&&fabsf(driftX-dTX)<2.f&&fabsf(velX)>14.f){
    overshootX=velX*0.10f; didOvershootX=true;
  }
  if(!didOvershootY&&fabsf(driftY-dTY)<2.f&&fabsf(velY)>12.f){
    overshootY=velY*0.10f; didOvershootY=true;
  }
  // Settle overshoot back to 0
  overshootX=elf(overshootX,0,dt*8.f); if(fabsf(overshootX)<0.1f){overshootX=0;didOvershootX=false;}
  overshootY=elf(overshootY,0,dt*8.f); if(fabsf(overshootY)<0.1f){overshootY=0;didOvershootY=false;}

  // Micro-jitter  0.5px noise, new value every 3 frames (~75ms)
  // Micro-jitter: very rare, very subtle -- just enough to feel alive
  if(ms-lastJitter>2000){
    lastJitter=ms;
    microJitterX=((float)(random(20)-10))/100.f; // -0.1 to +0.1
    microJitterY=((float)(random(20)-10))/100.f;
  }

  // Sleep ZZZs  spawn when SLEEPY expression has held for 5+ seconds
  if(curExpr==SLEEPY){
    if(!sleepyZzzActive){ sleepyStartMs=ms; sleepyZzzActive=true; }
    if(ms-sleepyStartMs > 5000 && ms-lastZzz > (unsigned long)random(1500,3000)){
      lastZzz=ms; spawnZzz();
    }
    updateAndDrawZzls(dt);
  } else {
    sleepyZzzActive=false;
  }

  // Teardrop
  if(curExpr==SAD){
    if(!tearOn&&ms-lastTear>(unsigned long)random(12000,28000)){lastTear=ms;spawnTear();}
    updateTear();
  }

  // Twitch
  if(curExpr==ANGRY){
    if(!twitchOn&&ms>nextTwitch){nextTwitch=ms+random(1500,4000);startTwitch();}
    updateTwitch();
  }

  // Idle trigger
  if(idleAnim==IA_NONE&&ms-lastIdleChk>500){
    lastIdleChk=ms;
    if(ms-lastExpr>2000&&ms>(lastExpr+nextIdle)) startIdle();
  }
  updateIdle(dt);

  // Blink
  // Rate: breath-linked  slower breath = rarer blinks, faster = more frequent
  // Weight: mood affects close/open speed (SLEEPY slow, HAPPY crisp)
  float closeSpd,openSpd;
  unsigned long closeDur,holdDur,openDur;
  switch(curExpr){
    case SLEEPY:   closeSpd=dt*5.f;  openSpd=dt*3.5f;  closeDur=220;holdDur=80; openDur=180; break;
    case HAPPY:    closeSpd=dt*10.f; openSpd=dt*9.f;   closeDur=120;holdDur=40; openDur=90;  break;
    case ANGRY:    closeSpd=dt*12.f; openSpd=dt*10.f;  closeDur=110;holdDur=35; openDur=80;  break;
    case SURPRISED:closeSpd=dt*14.f; openSpd=dt*11.f;  closeDur=100;holdDur=30; openDur=75;  break;
    default:       closeSpd=dt*8.f;  openSpd=dt*7.f;   closeDur=150;holdDur=55; openDur=110; break;
  }

  if(idleAnim!=IA_SNEEZE&&idleAnim!=IA_YAWN&&idleAnim!=IA_EXABLINK&&idleAnim!=IA_SBLINK){
    unsigned long el=ms-bTimer;
    switch(bState){
      case BW:
        blinkT=0;
        blinkOvershoot=elf(blinkOvershoot,0,dt*5.f);
        glOff=elf(glOff,0,dt*6.f);
        if(el>=nextBlink){
          dblBlink=(random(10)<2);dblDone=false;
          glTarg=(random(2)?1.f:-1.f)*(float)random(6,MGL);
          bState=BC;bTimer=ms;
        }
        break;
      case BC:
        blinkT=elf(blinkT,1.f,closeSpd);
        glOff=elf(glOff,glTarg,dt*10.f);
        if(el>=closeDur){blinkT=1;bState=BH;bTimer=ms;}
        break;
      case BH:
        blinkT=1;
        glOff=elf(glOff,glTarg,dt*12.f);
        if(el>=holdDur){glTarg=0;bState=BO;bTimer=ms;}
        break;
      case BO:
        // Asymmetric: open is slower than close (real eyelid physics)
        blinkT=elf(blinkT,0,openSpd);
        glOff=elf(glOff,0,dt*8.f);
        if(el>=openDur){
          blinkT=0;glOff=0;bTimer=ms;
          // Brief lid overshoot  eyes go fractionally wider after opening
          blinkOvershoot=0.6f;
          bState=BOVERSHOOT;
        }
        break;
      case BOVERSHOOT:
        blinkOvershoot=elf(blinkOvershoot,0,dt*8.f);
        if(blinkOvershoot<0.02f){
          blinkOvershoot=0;
          if(dblBlink&&!dblDone){dblDone=true;nextBlink=110;bState=BW;}
          else{
            dblBlink=false;dblDone=false;bState=BW;
            // Breath-linked blink interval: slow breath = longer wait
            float breathFactor=(bspd>1.2f)?0.65f:(bspd<0.5f)?1.5f:1.0f;
            nextBlink=(unsigned long)(random(2800,5500)*breathFactor);
          }
        }
        break;
    }
  }

  // Slide sprite down when bubble active
  {
    float sTarget=(bubbleActive&&curPage==PAGE_EYES)?(float)SPR_Y_BUBBLE:(float)SPR_Y;
    spriteYf=spriteYf+(sTarget-spriteYf)*dt*8.f;
    int newSY=(int)spriteYf;
    if(newSY!=spriteY){
      spriteY=newSY;
      if(bubbleActive&&bubbleNeedsErase&&curPage==PAGE_EYES) bubbleDraw();
    }
  }
  renderEyes();
  if(curPage==PAGE_EYES) updateHearts(dt);  // draw happens inside renderEyes
  if(curPage==PAGE_EYES) updateAndDrawSparks(dt);
  if(curPage==PAGE_EYES) updateAndDrawStars(dt);
  if(curPage==PAGE_EYES && confettiActive) updateAndDrawConfetti(dt);
  if(confettiActive && millis()>=confettiEndMs) confettiActive=false;
  // EMBARRASSED blush  two soft pink ellipses below the eyes
  if(curPage==PAGE_EYES && embarrassBlush>0.05f){
    uint8_t r5=(uint8_t)cf(embarrassBlush*28,0,31);
    uint8_t g6=(uint8_t)cf(embarrassBlush*12,0,63);
    uint16_t bc=((uint16_t)r5<<11)|((uint16_t)g6<<5)|4;
    tft.fillEllipse(BASE_ELX,  spriteY+ECY+BASE_RH+8, (int)(18*embarrassBlush), (int)(6*embarrassBlush), bc);
    tft.fillEllipse(BASE_ERX,  spriteY+ECY+BASE_RH+8, (int)(18*embarrassBlush), (int)(6*embarrassBlush), bc);
  } else {
    // Erase blush only once as it fades out completely
    static float lastBlush = 0;
    if(lastBlush > 0.05f && embarrassBlush <= 0.05f){
      tft.fillEllipse(BASE_ELX, spriteY+ECY+BASE_RH+8, 22, 9, BLK);
      tft.fillEllipse(BASE_ERX, spriteY+ECY+BASE_RH+8, 22, 9, BLK);
    }
    lastBlush = embarrassBlush;
  }
  unsigned long spent=millis()-ms;
  if(spent<25) delay(25-spent);
}
