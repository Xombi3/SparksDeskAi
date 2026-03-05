/*
 * ROBO EYES — Cheap Yellow Display (ESP32-2432S028R)
 * ====================================================
 * Full personality suite — all behaviours fire randomly.
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

#define WIFI_SSID  "YOUR_WIFI_SSID"
#define WIFI_PASS  "YOUR_WIFI_PASSWORD"
#define NTP_SERVER "pool.ntp.org"
#define TZ_OFFSET  (-6 * 3600)

// Touch — XPT2046 on HSPI (CYD exact pinout)
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
#define SPR_H 114
#define SPR_X   0
#define SPR_Y  63
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

// ── Hearts ───────────────────────────────────────────────────
struct Heart {
  float x, y;   // screen position
  float vy;     // upward velocity px/s
  float life;   // 1.0 -> 0.0
  float size;   // base radius
  bool  active;
};
#define MAX_HEARTS 6
Heart hearts[MAX_HEARTS];

// ── Angry sparks ─────────────────────────────────────────────
struct Spark {
  float x, y, vx, vy, life;
  bool  active;
};
#define MAX_SPARKS 8
Spark sparks[MAX_SPARKS];

// ── Sleep ZZZs ───────────────────────────────────────────────
struct Zzz {
  float x, y, vy, life, size;
  bool  active;
};
#define MAX_ZZLS 3
Zzz zzls[MAX_ZZLS];
unsigned long lastZzz = 0;
unsigned long sleepyStartMs = 0;  // when SLEEPY expression began
bool sleepyZzzActive = false;

// ── Double-tap detection ─────────────────────────────────────
unsigned long lastTapMs = 0;
#define DOUBLE_TAP_MS 350  // two taps within this = double-tap

// ── Hold anticipation ────────────────────────────────────────
float holdAntScale = 0;   // eye scale grows while holding
bool  holdTriggered = false;

// ── Pet counter (EEPROM) ─────────────────────────────────────
#define EEPROM_SIZE       64
#define EEPROM_PETS_TODAY  0   // int  (4 bytes)
#define EEPROM_PETS_TOTAL  4   // int  (4 bytes)
#define EEPROM_LAST_DAY    8   // int  (4 bytes) — tm_yday
#define EEPROM_NAME_ADDR  12   // char[20]
#define EEPROM_NAME_LEN   20
int  petsToday = 0;
int  petsTotal = 0;
char robotName[EEPROM_NAME_LEN] = "SPARKS";

#define SW  320
#define SH  240
#define BLK 0x0000
#define CDK 0x2D6B
#define BASE_ELX 105
#define BASE_ERX 215
#define ECY       57
#define BASE_RW   38
#define BASE_RH   42

// ── Enums (declared early — used by globals below) ───────────
enum Expr{NORMAL,HAPPY,SAD,ANGRY,SURPRISED,SUSPICIOUS,SLEEPY};

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
void pushMood(Expr e){moodHistory[2]=moodHistory[1];moodHistory[1]=moodHistory[0];moodHistory[0]=e;}
bool recentMood(Expr e){return(moodHistory[0]==e||moodHistory[1]==e||moodHistory[2]==e);}
Expr pickExpr(Expr timeMood){
  Expr pool[]={NORMAL,NORMAL,NORMAL,HAPPY,SAD,ANGRY,SURPRISED,SUSPICIOUS,SLEEPY,NORMAL};
  for(int i=0;i<8;i++){Expr c=(random(10)<4)?timeMood:pool[random(10)];if(!recentMood(c))return c;}
  Expr pool2[]={NORMAL,HAPPY,SAD,ANGRY,SURPRISED,SUSPICIOUS,SLEEPY};
  for(int i=0;i<14;i++){Expr c=pool2[random(7)];if(c!=moodHistory[0])return c;}
  return timeMood;
}

// ════════════════════════════════════════════════════════════
//  HELPERS
// ════════════════════════════════════════════════════════════
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

// ── Breathing ────────────────────────────────────────────────
float breathPhase=0;
#define BREATH_AMP 2.0f

// ── Eye colour ───────────────────────────────────────────────
uint16_t eyeColour=0x5EDF;

// ── WiFi ─────────────────────────────────────────────────────
bool wifiOK=false;
unsigned long lastTimePoll=0;
int currentHour=-1, currentDow=-1;

// ── Momentum-based drift ─────────────────────────────────────
float driftX=0,driftY=0;       // current position
float velX=0,velY=0;           // current velocity (px/s)
float dTX=0,dTY=0;             // target position
unsigned long ldX=0,ldY=0;
unsigned long dpX=3000,dpY=4000;
// Attention variance — the longer still, the bigger next move
unsigned long lastBigMove=0;
float microJitterX=0,microJitterY=0;
unsigned long lastJitter=0;

// Overshoot state
float overshootX=0,overshootY=0; // extra offset that settles to 0
bool  didOvershootX=false,didOvershootY=false;

// ── Blink ────────────────────────────────────────────────────
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
float petJoyScale=0,petJoySquish=0,petJoyBounce=0,petJoyPhase=0;
int   petJoyStage=0;
float sdLid=0;
int   sdCount=0;

// ── Teardrop ─────────────────────────────────────────────────
bool  tearOn=false;
float tearX=0,tearY=0,tearSpd=0,tearDrawY=0;
unsigned long lastTear=0;

// ── Twitch ───────────────────────────────────────────────────
bool  twitchOn=false;
float twitchAmt=0;
int   twitchEye=0,twitchLeft=0;
unsigned long twitchTimer=0,nextTwitch=0;

unsigned long prevMs=0;

// ════════════════════════════════════════════════════════════
//  DRAW EYE INTO SPRITE
// ════════════════════════════════════════════════════════════
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

// ════════════════════════════════════════════════════════════
//  RENDER
// ════════════════════════════════════════════════════════════
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

  int offX=(int)(driftX+overshootX+glOff+shakeX+thinkX+rollX+smugX+sideX+microJitterX+touchLookX);
  int offY=(int)(driftY+overshootY+shakeY+thinkY+rollY+xbY+pjY+microJitterY+touchLookY);

  int lx=BASE_ELX+offX+dOLX+(int)twL;
  int ly=ECY     +offY+dOLY;
  int rx=BASE_ERX+offX+dORX+(int)twR;
  int ry=ECY     +offY+dORY;

  spr.fillSprite(BLK);
  drawEye(lx,ly,rw,rh,cLTI+xTL,cLTO,cLBI+xBL_pet,cLBO,blL,true);
  drawEye(rx,ry,rw,rh,cRTI+xTR,cRTO,cRBI+xBR_pet,cRBO,blR,false);

  if(idleAnim==IA_YAWN&&yawnMouth>1.f){
    int mw=(int)(60*yawnMouth/18.f);
    int mh=(int)yawnMouth;
    spr.fillRoundRect(SPR_W/2-mw/2,ECY+rh+4,mw,mh,mh/3,0x630C);
  }
  spr.pushSprite(SPR_X,SPR_Y);
}

// ════════════════════════════════════════════════════════════
//  SET EXPRESSION
// ════════════════════════════════════════════════════════════
void setExpr(Expr e){
  curExpr=e;
  inTransition=true; transTimer=millis();
  switch(e){
    case NORMAL:     setRGB(0,80,80);   eyeColour=0x5EDF; break;
    case HAPPY:      setRGB(0,200,60);  eyeColour=0xFFD6; break;
    case SAD:        setRGB(0,0,180);   eyeColour=0x325F; break;
    case ANGRY:      setRGB(220,0,0);   eyeColour=0xF800; break;
    case SURPRISED:  setRGB(200,150,0); eyeColour=0xFFE0; break;
    case SUSPICIOUS: setRGB(100,0,200); eyeColour=0x801F; break;
    case SLEEPY:     setRGB(20,20,90);  eyeColour=0x2D6B; break;
  }
}

// ════════════════════════════════════════════════════════════
//  EXPRESSION LERP (staggered per-lid + transition pause)
// ════════════════════════════════════════════════════════════
void updateExprLerp(float dt){
  EyeExpr &tgt=exprs[curExpr];

  // During transition pause, lerp everything toward neutral first
  if(inTransition){
    if(millis()-transTimer<TRANS_PAUSE_MS){
      float sp=dt*6.f;
      cLTI=elf(cLTI,0,sp); cLTO=elf(cLTO,0,sp);
      cLBI=elf(cLBI,0,sp); cLBO=elf(cLBO,0,sp);
      cRTI=elf(cRTI,0,sp); cRTO=elf(cRTO,0,sp);
      cRBI=elf(cRBI,0,sp); cRBO=elf(cRBO,0,sp);
      return;
    }
    inTransition=false;
  }

  // Staggered: inner corners (TI) lead, outer (TO) follow slightly behind
  // This mimics how real eyelids move — centre first, corners catch up
  float spFast = dt*4.0f;   // inner corners
  float spMid  = dt*3.0f;   // outer corners
  float spSlow = dt*2.5f;   // scale (whole eye size)

  cLTI=elf(cLTI,tgt.lTI,spFast); cRTI=elf(cRTI,tgt.rTI,spFast);
  cLBI=elf(cLBI,tgt.lBI,spFast); cRBI=elf(cRBI,tgt.rBI,spFast);
  cLTO=elf(cLTO,tgt.lTO,spMid);  cRTO=elf(cRTO,tgt.rTO,spMid);
  cLBO=elf(cLBO,tgt.lBO,spMid);  cRBO=elf(cRBO,tgt.rBO,spMid);
  cRhS=elf(cRhS,tgt.rhS,spSlow);
}

// ════════════════════════════════════════════════════════════
//  IDLE — START (with behaviour chaining)
// ════════════════════════════════════════════════════════════
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

  // Behaviour chains — last anim biases next
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

// ════════════════════════════════════════════════════════════
//  IDLE — UPDATE
// ════════════════════════════════════════════════════════════
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
      // Stage 0 — eyes swell big, bottom lids crinkle up
      if(petJoyStage==0){
        petJoyScale  = elf(petJoyScale,  1.0f, dt*14.f);
        petJoySquish = elf(petJoySquish, 0.32f,dt*10.f);
        petJoyPhase += dt*28.f;
        petJoyBounce = sinf(petJoyPhase)*8.f;
        if(petJoyScale>0.92f){ petJoyStage=1; idleTimer=ms; }
      }
      // Stage 1 — hold bouncing joy for 1.2s
      else if(petJoyStage==1){
        petJoyPhase  += dt*28.f;
        petJoyBounce  = sinf(petJoyPhase)*8.f;
        petJoyScale   = elf(petJoyScale,  0.8f, dt*2.f);
        petJoySquish  = elf(petJoySquish, 0.28f,dt*2.f);
        if(ms-idleTimer>1200){ petJoyStage=2; idleTimer=ms; }
      }
      // Stage 2 — gentle settle
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

// ════════════════════════════════════════════════════════════
//  TEARDROP
// ════════════════════════════════════════════════════════════
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

// ════════════════════════════════════════════════════════════
//  TWITCH
// ════════════════════════════════════════════════════════════
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

// ════════════════════════════════════════════════════════════
//  WIFI + TIME
// ════════════════════════════════════════════════════════════
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

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
// ════════════════════════════════════════════════════════════
//  HEART ANIMATION
// ════════════════════════════════════════════════════════════
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

void tftHeart(int cx, int cy, int r, uint16_t col){
  if(r < 2) return;
  tft.fillCircle(cx - r/2, cy - r/4, r/2, col);
  tft.fillCircle(cx + r/2, cy - r/4, r/2, col);
  int top = cy - r/4 + r/2;
  for(int row=0; row<=r; row++){
    int hw = r - row + 1;
    if(hw < 1) break;
    tft.drawFastHLine(cx - hw + 1, top + row, (hw-1)*2, col);
  }
}

void eraseHeart(int cx, int cy, int r){
  if(r < 2) return;
  tft.fillCircle(cx - r/2, cy - r/4, r/2 + 1, BLK);
  tft.fillCircle(cx + r/2, cy - r/4, r/2 + 1, BLK);
  int top = cy - r/4 + r/2;
  for(int row=0; row<=r+2; row++){
    int hw = r - row + 3;
    if(hw < 1) break;
    tft.drawFastHLine(cx - hw + 1, top + row, (hw-1)*2, BLK);
  }
}

void updateAndDrawHearts(float dt){
  for(int i=0;i<MAX_HEARTS;i++){
    if(!hearts[i].active) continue;
    // Erase old
    eraseHeart((int)hearts[i].x,(int)hearts[i].y,(int)(hearts[i].size*(0.5f+0.5f*hearts[i].life))+1);
    // Move
    hearts[i].y    -= hearts[i].vy * dt;
    hearts[i].life -= dt * 0.65f;  // ~1.5s lifetime
    if(hearts[i].life <= 0 || hearts[i].y < -20){
      hearts[i].active = false;
      continue;
    }
    // Colour: vivid pink fading out
    float l = hearts[i].life;
    uint8_t r5 = (uint8_t)cf(8  + 23*l, 0, 31);
    uint8_t g6 = (uint8_t)cf(1  +  6*l, 0, 63);
    uint8_t b5 = (uint8_t)cf(5  + 13*l, 0, 31);
    uint16_t col = ((uint16_t)r5<<11)|((uint16_t)g6<<5)|b5;
    int nr = (int)(hearts[i].size * (0.5f + 0.5f*l));
    if(nr < 2) nr = 2;
    tftHeart((int)hearts[i].x,(int)hearts[i].y, nr, col);
  }
}

// ════════════════════════════════════════════════════════════
//  EEPROM — pet counter + name
// ════════════════════════════════════════════════════════════
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
  buf[EEPROM_NAME_LEN-1]=' ';
  if(buf[0]>='A' && buf[0]<='Z') strncpy(robotName, buf, EEPROM_NAME_LEN);
  if(petsTotal < 0 || petsTotal > 99999) petsTotal = 0;
  if(petsToday < 0 || petsToday > 9999)  petsToday = 0;
}

void eepromSavePet(){
  petsToday++;
  petsTotal++;
  struct tm ti; getLocalTime(&ti, 200);
  EEPROM.put(EEPROM_PETS_TODAY, petsToday);
  EEPROM.put(EEPROM_PETS_TOTAL, petsTotal);
  EEPROM.put(EEPROM_LAST_DAY,   ti.tm_yday);
  EEPROM.commit();
}

// ════════════════════════════════════════════════════════════
//  ANGRY SPARKS
// ════════════════════════════════════════════════════════════
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
    // Update — zigzag by adding sin to vx
    sparks[i].x  += sparks[i].vx * dt;
    sparks[i].y  += sparks[i].vy * dt;
    sparks[i].vy += 120.f * dt; // gravity pulls down
    sparks[i].vx += sinf(sparks[i].y * 0.3f) * 40.f * dt; // zigzag
    sparks[i].life -= dt * 1.8f;
    if(sparks[i].life <= 0 || sparks[i].y > SH || sparks[i].y < 0){
      sparks[i].active = false; continue;
    }
    // Colour: red → orange → yellow as it fades
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

// ════════════════════════════════════════════════════════════
//  SLEEP ZZZs
// ════════════════════════════════════════════════════════════
void initZzls(){ for(int i=0;i<MAX_ZZLS;i++) zzls[i].active=false; }

void spawnZzz(){
  for(int i=0;i<MAX_ZZLS;i++){
    if(!zzls[i].active){
      // Float up from right eye area
      zzls[i].x    = (float)(BASE_ERX + random(0,20));
      zzls[i].y    = (float)(SPR_Y + ECY - 10);
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

// ════════════════════════════════════════════════════════════
//  TOUCH HELPERS
// ════════════════════════════════════════════════════════════
// Map raw XPT2046 coords → screen pixels for landscape rotation 1
void getTouchXY(int16_t &sx, int16_t &sy){
  TS_Point p = ts.getPoint();
  // On ESP32-2432S028R in rotation 1: raw X maps to screen Y, raw Y maps to screen X
  sx = map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SW);
  sy = map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, SH);
  sx = constrain(sx, 0, SW-1);
  sy = constrain(sy, 0, SH-1);
}

// Called every loop — full state machine for touch/swipe/tap
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
      touchLookX = map(tx, 0, SW, -30, 30);
      touchLookY = map(ty, 0, SH, -20, 20);
      touchReacting = true; touchReactTimer = ms;
    } else {
      // Still held — update eye tracking
      touchLookX = map(tx, 0, SW, -30, 30);
      touchLookY = map(ty, 0, SH, -20, 20);
      touchState = TS_HELD;

      // Hold anticipation — eyes widen progressively on eyes page
      if(curPage == PAGE_EYES && !holdTriggered){
        unsigned long heldMs = ms - touchDownMs;
        holdAntScale = cf((float)heldMs / 1800.f, 0, 1.0f);
        // At 800ms cross → face shows anticipation expression
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

    // ── Swipe ──────────────────────────────────────────────
    if(adx > SWIPE_THRESH && adx > ady && held < SWIPE_MS){
      curPage = (Page)((curPage + 1) % 2);
      tft.fillScreen(BLK);

    // ── Hold release (1.5s+) — sneeze/shake with relief ───
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

    // ── Tap ───────────────────────────────────────────────
    } else if(adx < TAP_MAX_MOVE && ady < TAP_MAX_MOVE && held < TAP_MAX_MS){
      holdAntScale = 0;
      if(curPage == PAGE_EYES){
        bool isDoubleTap = (ms - lastTapMs < DOUBLE_TAP_MS);
        lastTapMs = ms;

        if(isDoubleTap){
          // ── DOUBLE-TAP: wide-eye surprise + quick shake ─
          idleAnim=IA_STARTLED; startledSc=0; ist=0; idleTimer=ms;
          pushMood(SURPRISED); setExpr(SURPRISED);
          // Burst of hearts
          for(int h=0;h<6;h++) spawnHeart((float)touchDownX,(float)touchDownY);
          lastExpr=ms; nextExprMs=random(5000,10000);

        } else if(curExpr == ANGRY){
          // ── TAP WHILE ANGRY: recoil + sparks ───────────
          for(int s=0;s<MAX_SPARKS;s++)
            spawnSparks((float)touchDownX,(float)touchDownY);
          // Eyes narrow more briefly then soften
          idleAnim=IA_SQUINT; squintEye=2; squintAmt=0; ist=0; idleTimer=ms;
          // After a moment shift to suspicious
          lastExpr=ms; nextExprMs=random(3000,5000);

        } else {
          // ── NORMAL TAP: pet joy + hearts ───────────────
          int n = random(2,5);
          for(int h=0;h<n;h++) spawnHeart((float)touchDownX,(float)touchDownY);
          pushMood(HAPPY); setExpr(HAPPY);
          idleAnim=IA_PETJOY;
          petJoyScale=0; petJoySquish=0;
          petJoyBounce=0; petJoyPhase=0;
          petJoyStage=0; idleTimer=ms;
          dTX=0; dTY=0;
          lastExpr=ms; nextExprMs=random(8000,15000);
          eepromSavePet();
        }
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

// ════════════════════════════════════════════════════════════
//  WEATHER FETCH  (OpenWeatherMap free tier, no key needed
//  for basic current conditions via wttr.in JSON)
// ════════════════════════════════════════════════════════════
// ════════════════════════════════════════════════════════════
//  DRAW CLOCK PAGE
// ════════════════════════════════════════════════════════════
void drawClock(){
  tft.fillScreen(BLK);
  struct tm ti;
  if(!getLocalTime(&ti, 200)){
    tft.setTextColor(0x630C); tft.setTextSize(2);
    tft.setCursor(80,110); tft.print("No time sync");
    return;
  }

  // Time — big centred
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

  // Name + pet counts
  tft.setTextSize(2);
  tft.setTextColor(eyeColour);
  char nameBuf[24];
  snprintf(nameBuf, sizeof(nameBuf), "%s", robotName);
  int nw = strlen(nameBuf)*6*2;
  tft.setCursor((SW-nw)/2, 152);
  tft.print(nameBuf);

  tft.setTextSize(1);
  tft.setTextColor(0x7BEF);
  char petBuf[32];
  snprintf(petBuf, sizeof(petBuf), "pets today: %d   total: %d", petsToday, petsTotal);
  int pw = strlen(petBuf)*6;
  tft.setCursor((SW-pw)/2, 178);
  tft.print(petBuf);

  // Swipe hint
  tft.setTextSize(1);
  tft.setTextColor(0x2945);
  tft.setCursor(100, 220);
  tft.print("swipe: eyes");
}

// ════════════════════════════════════════════════════════════
//  WAKE ANIMATION
//  Eyes start fully closed, RGB off. Sequence:
//  1. Backlight fades in (0→255, 600ms)
//  2. Lids flicker slightly — half-open, close again (like REM)
//  3. Slow open to ~30%, hold 400ms (groggy)
//  4. Close again briefly (200ms)
//  5. Open fully with a small overshoot
//  6. RGB fades to expression colour
// ════════════════════════════════════════════════════════════
void wakeUp(){
  // Start with lids fully shut, backlight off
  cLTI=1.f;cLTO=1.f;cRTI=1.f;cRTO=1.f;
  cLBI=0.f;cLBO=0.f;cRBI=0.f;cRBO=0.f;
  cRhS=1.0f; blinkT=0;
  analogWrite(LCD_BL_PIN,0);
  setRGB(0,0,0);

  // Stage 1 — backlight fades in while eyes stay shut
  for(int i=0;i<=255;i+=3){
    analogWrite(LCD_BL_PIN,i);
    renderEyes();
    delay(7);
  }
  analogWrite(LCD_BL_PIN,255);
  delay(200);

  // Stage 2 — REM flicker: half-open, close
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

  // Stage 3 — groggy half-open
  for(float t=1.f;t>0.55f;t-=0.025f){
    cLTI=t;cLTO=t;cRTI=t;cRTO=t;
    renderEyes(); delay(22);
  }
  delay(500);

  // Stage 4 — close again (resisting waking)
  for(float t=0.55f;t<1.f;t+=0.05f){
    cLTI=t;cLTO=t;cRTI=t;cRTO=t;
    renderEyes(); delay(18);
  }
  delay(300);

  // Stage 5 — open fully, with slight overshoot
  // Expression lids are all 0 for NORMAL — open means lids go to 0
  for(float t=1.f;t>-0.08f;t-=0.03f){ // overshoot past 0
    float v=max(t,0.f);
    cLTI=v;cLTO=v;cRTI=v;cRTO=v;
    renderEyes(); delay(14);
  }
  // Settle back from overshoot
  cLTI=0;cLTO=0;cRTI=0;cRTO=0;
  // Brief wide-open hold
  delay(180);

  // Stage 6 — RGB fades in to expression colour
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
  eepromLoad();
  wakeUp(); // boot sequence before entering main loop
}

// ════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════
void loop(){
  unsigned long ms=millis();
  float dt=cf((ms-prevMs)/1000.f,0.001f,0.05f);
  prevMs=ms;

  // Handle touch / swipe
  handleTouch();

  // Non-eye pages — draw and yield, don't run eye logic
  if(curPage == PAGE_CLOCK){
    static unsigned long lastClockDraw = 0;
    if(ms - lastClockDraw > 1000){ lastClockDraw = ms; drawClock(); }
    unsigned long spent=millis()-ms; if(spent<25) delay(25-spent);
    return;
  }

  // Time poll
  if(wifiOK&&ms-lastTimePoll>60000){
    lastTimePoll=ms;
    if(pollTime()&&ms-lastExpr>nextExprMs/2) setExpr(timeExpr());
  }

  // Expression cycle
  if(ms-lastExpr>nextExprMs){
    lastExpr=ms;nextExprMs=random(5000,12000);
    Expr next=pickExpr(timeExpr());
    pushMood(next);setExpr(next);
    if(curExpr!=ANGRY) twitchOn=false;
    if(curExpr!=SAD&&tearOn){eraseTearCol((int)tearX,SPR_Y+SPR_H,(int)tearDrawY+2);tearOn=false;}
  }

  // Expression lerp with stagger + transition pause
  updateExprLerp(dt);

  // Breathing — speed linked to mood
  float bspd;
  switch(curExpr){case SLEEPY:bspd=0.35f;break;case SURPRISED:bspd=1.8f;break;case ANGRY:bspd=1.4f;break;default:bspd=0.8f;}
  breathPhase+=dt*bspd;
  if(breathPhase>2*PI) breathPhase-=2*PI;

  // ── Momentum drift X ─────────────────────────────────────
  // Suppress autonomous drift while finger is actively held
  if(touchIsDown){ velX*=0.85f; velY*=0.85f; } // damp momentum while touching
  // Attention variance: longer since big move = more likely to do a large one
  unsigned long stillFor=ms-lastBigMove;

  if(ms-ldX>dpX){
    ldX=ms;
    float attentionBoost=(stillFor>8000)?1.6f:1.0f; // been still a while
    switch(curExpr){
      case ANGRY:    dpX=random(1200,2200);dTX=(float)(random(2)?1:-1)*random(20,45); break;
      case SLEEPY:   dpX=random(9000,15000);dTX=(float)random(-8,8); break;
      case HAPPY:    dpX=random(2000,4000);dTX=lf(dTX,(float)random(-25,25),0.5f); break;
      case SURPRISED:dpX=random(1500,3000);dTX=(float)(random(2)?1:-1)*random(15,40); break;
      default:{dpX=random(4000,8000);int r=random(10);
        float rng=(r<5)?random(15,22):(r<8)?random(28,40):random(38,50);
        dTX=(float)(random(2)?1:-1)*rng*attentionBoost;break;}
    }
    if(fabsf(dTX)>30) lastBigMove=ms;
  }

  // ── Momentum drift Y ─────────────────────────────────────
  if(ms-ldY>dpY){
    ldY=ms;
    float attentionBoost=(stillFor>8000)?1.5f:1.0f;
    switch(curExpr){
      case SLEEPY:   dpY=random(7000,12000);dTY=(float)random(18,34); break;
      case SAD:      dpY=random(4000,8000); dTY=(float)random(14,30);  break;
      case HAPPY:    dpY=random(1500,3500); dTY=(float)random(-28,28)*attentionBoost; break;
      case ANGRY:    dpY=random(1200,2500); dTY=(float)(random(2)?1:-1)*random(16,32); break;
      case SURPRISED:dpY=random(1200,2500); dTY=(float)(random(2)?1:-1)*random(20,36); break;
      default:{dpY=random(3500,7000);int r=random(10);
        float rng=(r<5)?random(16,26):(r<8)?random(26,38):random(34,46);
        dTY=(float)(random(2)?1:-1)*rng*attentionBoost;break;}
    }
    if(fabsf(dTY)>24) lastBigMove=ms;
  }

  // Apply momentum: velocity driven toward target, with drag
  // This gives the eye mass — it accelerates and decelerates naturally
  float targetVX=(dTX-driftX)*2.5f; // spring constant
  float targetVY=(dTY-driftY)*2.5f;
  float drag;
  switch(curExpr){case ANGRY:drag=0.82f;break;case SLEEPY:drag=0.94f;break;case HAPPY:drag=0.86f;break;default:drag=0.90f;}
  velX=lf(velX,targetVX,dt*3.5f)*drag;
  velY=lf(velY,targetVY,dt*3.5f)*drag;
  driftX+=velX*dt;
  driftY+=velY*dt;

  // Overshoot: when eye passes close to target, give it a small kick past
  if(!didOvershootX&&fabsf(driftX-dTX)<3.f&&fabsf(velX)>8.f){
    overshootX=velX*0.18f; didOvershootX=true;
  }
  if(!didOvershootY&&fabsf(driftY-dTY)<3.f&&fabsf(velY)>6.f){
    overshootY=velY*0.18f; didOvershootY=true;
  }
  // Settle overshoot back to 0
  overshootX=elf(overshootX,0,dt*6.f); if(fabsf(overshootX)<0.1f){overshootX=0;didOvershootX=false;}
  overshootY=elf(overshootY,0,dt*6.f); if(fabsf(overshootY)<0.1f){overshootY=0;didOvershootY=false;}

  // Micro-jitter — ±0.5px noise, new value every 3 frames (~75ms)
  if(ms-lastJitter>75){
    lastJitter=ms;
    microJitterX=((float)(random(100)-50))/100.f; // -0.5 to +0.5
    microJitterY=((float)(random(100)-50))/100.f;
  }

  // Sleep ZZZs — spawn when SLEEPY expression has held for 5+ seconds
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
  // Rate: breath-linked — slower breath = rarer blinks, faster = more frequent
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
          // Brief lid overshoot — eyes go fractionally wider after opening
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

  renderEyes();
  if(curPage==PAGE_EYES) updateAndDrawHearts(dt);
  if(curPage==PAGE_EYES) updateAndDrawSparks(dt);
  unsigned long spent=millis()-ms;
  if(spent<25) delay(25-spent);
}
