#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// =========================================================
// OLED
// =========================================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1, 1700000UL, 1700000UL);

// =========================================================
// TOUCH PADS (YOUR mapping)
// A     -> TOUCH0 / D10 / GPIO3
// B     -> TOUCH1 / D9  / GPIO4
// UP    -> TOUCH5 / D0  / GPIO26
// DOWN  -> TOUCH2 / D8  / GPIO2
// RIGHT -> TOUCH4 / D7  / GPIO1
// LEFT  -> TOUCH3 / D1  / GPIO27
// =========================================================
static const int PIN_A     = 3;
static const int PIN_B     = 4;
static const int PIN_UP    = 26;
static const int PIN_DOWN  = 2;
static const int PIN_RIGHT = 1;
static const int PIN_LEFT  = 27;

enum TouchIdx : uint8_t { T_A=0, T_B=1, T_UP=2, T_DOWN=3, T_RIGHT=4, T_LEFT=5 };
#define N_TOUCH 6

int  touch_pins[N_TOUCH]   = { PIN_A, PIN_B, PIN_UP, PIN_DOWN, PIN_RIGHT, PIN_LEFT };
int  touch_values[N_TOUCH] = {0,0,0,0,0,0};
int  touch_base[N_TOUCH]   = {0,0,0,0,0,0};
int  touch_thresh[N_TOUCH] = {0,0,0,0,0,0};
bool pin_now[N_TOUCH]      = {0,0,0,0,0,0};
bool pin_prev[N_TOUCH]     = {0,0,0,0,0,0};

// If your core doesn't provide digitalWriteFast/digitalReadFast, uncomment:
// #define digitalWriteFast digitalWrite
// #define digitalReadFast  digitalRead

static inline bool pressed(uint8_t i) { return pin_now[i] && !pin_prev[i]; }
static inline bool held(uint8_t i)    { return pin_now[i]; }

// A+B hold global quit to menu
bool abHeld=false; uint32_t abHoldStart=0;
static inline bool abEdgeToggle() {
  bool now = held(T_A) && held(T_B);
  bool prev = pin_prev[T_A] && pin_prev[T_B];
  return now && !prev;
}
static inline bool abLongHeld() {
  bool both = held(T_A) && held(T_B);
  if (both && !abHeld) { abHeld=true; abHoldStart=millis(); }
  if (!both) abHeld=false;
  return both && abHeld && (millis()-abHoldStart > 900);
}

void update_touch() {
  const int t_max = 250;
  for (int i=0;i<N_TOUCH;i++) {
    int p = touch_pins[i];
    pinMode(p, OUTPUT);
    digitalWriteFast(p, LOW);
    delayMicroseconds(25);
    pinMode(p, INPUT_PULLUP);

    int t=0;
    while(!digitalReadFast(p) && t < t_max) t++;
    touch_values[i] = t;

    pin_prev[i] = pin_now[i];
    pin_now[i]  = (touch_values[i] > touch_thresh[i]);
  }
}

void calibrate_touch() {
  const int samples=55;
  int acc[N_TOUCH]={0,0,0,0,0,0};

  for(int s=0;s<samples;s++){
    const int t_max=250;
    for(int i=0;i<N_TOUCH;i++){
      int p=touch_pins[i];
      pinMode(p, OUTPUT);
      digitalWriteFast(p, LOW);
      delayMicroseconds(25);
      pinMode(p, INPUT_PULLUP);
      int t=0;
      while(!digitalReadFast(p) && t<t_max) t++;
      acc[i]+=t;
    }
    delay(5);
  }

  for(int i=0;i<N_TOUCH;i++){
    touch_base[i]=acc[i]/samples;
    const int margin=12; // 8 more sensitive, 16 less
    touch_thresh[i]=touch_base[i]+margin;
    pin_now[i]=pin_prev[i]=false;
  }
}

// =========================================================
// RNG
// =========================================================
uint32_t rngState=1;
static inline uint32_t seedRand(){ return (uint32_t)(micros() ^ (millis()<<10)); }
static inline uint32_t urand(){ rngState^=rngState<<13; rngState^=rngState>>17; rngState^=rngState<<5; return rngState; }
static inline int irand(int n){ return (int)(urand() % (uint32_t)n); }

// =========================================================
// UI helpers
// =========================================================
static inline void headerBar(const char* t){
  display.fillRect(0,0,128,10,SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2,1);
  display.print(t);
  display.setTextColor(SSD1306_WHITE);
}

static inline void wipe(){
  for(int x=0;x<128;x+=8){
    display.fillRect(0,0,x,64,SSD1306_BLACK);
    display.display();
    delay(3);
  }
}

// =========================================================
// Grid
// =========================================================
static const int CELL = 4;
static const int GW = 32;
static const int GH = 13;
static const int ORGX = 0;
static const int ORGY = 11;

struct Pt { int8_t x,y; };

static inline void drawCell(int x,int y,bool fill){
  int px = ORGX + x*CELL;
  int py = ORGY + y*CELL;
  if(fill) display.fillRect(px,py,CELL,CELL,SSD1306_WHITE);
  else     display.drawRect(px,py,CELL,CELL,SSD1306_WHITE);
}

// =========================================================
// Settings / Modes
// =========================================================
enum : uint8_t { DIFF_EASY=0, DIFF_NORMAL=1, DIFF_HARD=2 };
enum : uint8_t { MODE_CLASSIC=0, MODE_MAZE=1 };

uint8_t gDiff = DIFF_NORMAL;
bool    gWrapWalls = false;          // Classic only
uint8_t gMazeDensity = 1;            // 0 low, 1 med, 2 high

static inline uint16_t tickMsForDiff(uint8_t d){
  if(d==DIFF_EASY) return 150;
  if(d==DIFF_NORMAL) return 110;
  return 80;
}

// =========================================================
// EEPROM High Scores (Classic + Maze, per difficulty)
// Each entry: score(uint16) + initials(3)
// =========================================================
#define EEPROM_SIZE 256
#define SAVE_MAGIC  0x534E4B44UL // 'SNKD'

uint16_t hsScore[2][3][5];      // [mode][diff][rank]
char     hsName [2][3][5][3];   // [mode][diff][rank][3]

static inline void hsSetEmpty(uint8_t mode, uint8_t diff, uint8_t i){
  hsScore[mode][diff][i] = 0;
  hsName[mode][diff][i][0] = '-';
  hsName[mode][diff][i][1] = '-';
  hsName[mode][diff][i][2] = '-';
}

static inline void saveDefaults(){
  for(uint8_t m=0;m<2;m++){
    for(uint8_t d=0;d<3;d++){
      for(uint8_t i=0;i<5;i++){
        hsSetEmpty(m,d,i);
      }
    }
  }
}

static inline int hsQualifies(uint8_t mode, uint8_t diff, uint16_t score){
  for(int i=0;i<5;i++){
    if(score > hsScore[mode][diff][i]) return i;
  }
  return -1;
}

static inline void hsInsert(uint8_t mode, uint8_t diff, int rank, uint16_t score, const char name3[3]){
  for(int i=4;i>rank;i--){
    hsScore[mode][diff][i] = hsScore[mode][diff][i-1];
    hsName[mode][diff][i][0] = hsName[mode][diff][i-1][0];
    hsName[mode][diff][i][1] = hsName[mode][diff][i-1][1];
    hsName[mode][diff][i][2] = hsName[mode][diff][i-1][2];
  }
  hsScore[mode][diff][rank] = score;
  hsName[mode][diff][rank][0] = name3[0];
  hsName[mode][diff][rank][1] = name3[1];
  hsName[mode][diff][rank][2] = name3[2];
}

static inline void loadSave(){
  EEPROM.begin(EEPROM_SIZE);
  uint32_t magic=0;
  EEPROM.get(0, magic);
  if(magic != SAVE_MAGIC){
    saveDefaults();
    EEPROM.put(0, (uint32_t)SAVE_MAGIC);
    int addr=4;
    EEPROM.put(addr, hsScore); addr += (int)sizeof(hsScore);
    EEPROM.put(addr, hsName);  addr += (int)sizeof(hsName);
    EEPROM.commit();
    return;
  }
  int addr=4;
  EEPROM.get(addr, hsScore); addr += (int)sizeof(hsScore);
  EEPROM.get(addr, hsName);  addr += (int)sizeof(hsName);
}

static inline void commitSave(){
  EEPROM.put(0, (uint32_t)SAVE_MAGIC);
  int addr=4;
  EEPROM.put(addr, hsScore); addr += (int)sizeof(hsScore);
  EEPROM.put(addr, hsName);  addr += (int)sizeof(hsName);
  EEPROM.commit();
}

// =========================================================
// MENU attract snake
// =========================================================
static const int MENU_SNAKE_LEN = 12;
Pt menuSnake[MENU_SNAKE_LEN];
int menuDir = 1; // 0 up,1 right,2 down,3 left
uint32_t lastMenuMove = 0;

static inline bool menuForbiddenCell(int gx, int gy){
  int px = ORGX + gx*CELL;
  int py = ORGY + gy*CELL;
  bool inY = (py >= 12 && py <= 62);
  bool inX = (px >= 8 && px <= 120);
  bool nearEdge = (px < 4 || px > 124);
  return inY && inX && !nearEdge;
}

static inline void resetMenuSnake(){
  int sx = irand(GW);
  int sy = irand(GH);
  for(int i=0;i<MENU_SNAKE_LEN;i++){
    int x = sx - i;
    if(x<0) x += GW;
    menuSnake[i] = {(int8_t)x,(int8_t)sy};
  }
  menuDir = irand(4);
  lastMenuMove = millis();
}

static inline void stepMenuSnake(){
  if(millis() - lastMenuMove < 170) return;
  lastMenuMove = millis();

  if(irand(100) < 28){
    int turn = irand(3) - 1; // -1,0,+1
    menuDir = (menuDir + turn + 4) % 4;
  }

  Pt head = menuSnake[0];
  int tryDir = menuDir;
  Pt nh = head;

  for(int attempt=0; attempt<4; attempt++){
    nh = head;
    if(tryDir==0) nh.y--;
    if(tryDir==1) nh.x++;
    if(tryDir==2) nh.y++;
    if(tryDir==3) nh.x--;

    if(nh.x < 0) nh.x = GW-1;
    if(nh.x >= GW) nh.x = 0;
    if(nh.y < 0) nh.y = GH-1;
    if(nh.y >= GH) nh.y = 0;

    if(!menuForbiddenCell(nh.x, nh.y)){
      menuDir = tryDir;
      break;
    }
    tryDir = (tryDir + 1) & 3;
  }

  for(int i=MENU_SNAKE_LEN-1;i>0;i--) menuSnake[i]=menuSnake[i-1];
  menuSnake[0]=nh;
}

static inline void drawMenuSnake(){
  for(int i=0;i<MENU_SNAKE_LEN;i++){
    int px = ORGX + menuSnake[i].x * CELL;
    int py = ORGY + menuSnake[i].y * CELL;
    if(i==0){
      display.fillRect(px,py,CELL,CELL,SSD1306_WHITE);
      display.drawPixel(px+1,py+1,SSD1306_BLACK);
    } else {
      display.drawRect(px,py,CELL,CELL,SSD1306_WHITE);
    }
  }
}

// =========================================================
// GAME: Classic Snake
// =========================================================
static const int MAX_SNAKE = GW*GH;
Pt snake[MAX_SNAKE];
int snakeLen=0;
int dir=1;
int queuedDir=1;
Pt food;
bool foodSpark=false;
uint16_t classicScore=0;
bool paused=false;
bool dead=false;

static inline bool isOpposite(int a,int b){
  return (a==0 && b==2) || (a==2 && b==0) || (a==1 && b==3) || (a==3 && b==1);
}
static inline bool cellOccupied(int x,int y){
  for(int i=0;i<snakeLen;i++){
    if(snake[i].x==x && snake[i].y==y) return true;
  }
  return false;
}
static inline void spawnFood(){
  for(int tries=0; tries<250; tries++){
    int x=irand(GW), y=irand(GH);
    if(!cellOccupied(x,y)){
      food={(int8_t)x,(int8_t)y};
      foodSpark=true;
      return;
    }
  }
  food={(int8_t)(GW/2),(int8_t)(GH/2)};
  foodSpark=true;
}
static inline void resetClassic(){
  classicScore=0;
  paused=false;
  dead=false;
  snakeLen=4;
  snake[0] = {(int8_t)(GW/2),(int8_t)(GH/2)};
  snake[1] = {(int8_t)(GW/2-1),(int8_t)(GH/2)};
  snake[2] = {(int8_t)(GW/2-2),(int8_t)(GH/2)};
  snake[3] = {(int8_t)(GW/2-3),(int8_t)(GH/2)};
  dir=1; queuedDir=1;
  spawnFood();
}
static inline void applyMoveInput(){
  int want=queuedDir;
  if(pressed(T_UP)) want=0;
  if(pressed(T_RIGHT)) want=1;
  if(pressed(T_DOWN)) want=2;
  if(pressed(T_LEFT)) want=3;
  if(!isOpposite(dir,want)) queuedDir=want;
}
static inline void stepClassic(){
  if(!isOpposite(dir,queuedDir)) dir=queuedDir;

  Pt head = snake[0];
  Pt nh = head;
  if(dir==0) nh.y--;
  if(dir==1) nh.x++;
  if(dir==2) nh.y++;
  if(dir==3) nh.x--;

  if(gWrapWalls){
    if(nh.x<0) nh.x=GW-1;
    if(nh.x>=GW) nh.x=0;
    if(nh.y<0) nh.y=GH-1;
    if(nh.y>=GH) nh.y=0;
  } else {
    if(nh.x<0||nh.x>=GW||nh.y<0||nh.y>=GH){ dead=true; return; }
  }

  for(int i=0;i<snakeLen;i++){
    if(snake[i].x==nh.x && snake[i].y==nh.y){ dead=true; return; }
  }

  bool ate = (nh.x==food.x && nh.y==food.y);
  if(ate){
    if(snakeLen<MAX_SNAKE) snakeLen++;
    // 1 point per food on ALL difficulties
    classicScore += 1;
    spawnFood();
  }

  for(int i=snakeLen-1;i>0;i--) snake[i]=snake[i-1];
  snake[0]=nh;
  foodSpark = !foodSpark;
}

static inline void renderClassic(){
  display.clearDisplay();

  // Border ONLY when walls are SOLID (wrap = no border visible)
  if(!gWrapWalls){
    display.drawRect(0, ORGY-1, 128, GH*CELL+2, SSD1306_WHITE);
  }

  // Minimal score (top-right)
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int sx = 128 - 6 * (int)String(classicScore).length() - 2;
  if(sx < 0) sx = 0;
  display.setCursor(sx, 2);
  display.print(classicScore);

  // Snake
  for(int i=0;i<snakeLen;i++){
    drawCell(snake[i].x, snake[i].y, true);
    if(i==0){
      int px = ORGX + snake[i].x * CELL;
      int py = ORGY + snake[i].y * CELL;
      display.drawPixel(px+1, py+1, SSD1306_BLACK);
      display.drawPixel(px+2, py+1, SSD1306_BLACK);
    }
  }

  // Food (pulse)
  if(foodSpark) {
    drawCell(food.x, food.y, false);
  } else {
    int fx = ORGX + food.x * CELL;
    int fy = ORGY + food.y * CELL;
    display.drawCircle(fx+2, fy+2, 1, SSD1306_WHITE);
  }

  // Pause overlay
  if(paused){
    display.fillRect(28, 24, 72, 16, SSD1306_BLACK);
    display.drawRect(28, 24, 72, 16, SSD1306_WHITE);
    display.setCursor(46, 30);
    display.print("PAUSED");
  }

  display.display();
}

// =========================================================
// GAME: Maze Trial
// Score = seconds survived (higher is better)
// =========================================================
uint8_t maze[GW*GH]; // 0 empty, 1 wall
Pt player;
int pdir=1;
int pqueued=1;
uint32_t mazeStartMs=0;
uint16_t mazeScoreSeconds=0;
bool mazeDead=false;
bool mazePaused=false;

// hazard pulse (now ONLY on Hard, and lighter)
uint32_t hazardNextMs=0;
uint8_t hazardOn=false;

static inline int idx(int x,int y){ return y*GW+x; }
static inline bool isWall(int x,int y){ return maze[idx(x,y)] != 0; }
static inline void setWall(int x,int y, bool w){ maze[idx(x,y)] = w ? 1 : 0; }

static inline void clearMaze(){
  for(int i=0;i<GW*GH;i++) maze[i]=0;
}

static inline void genMaze(){
  clearMaze();

  // border walls
  for(int x=0;x<GW;x++){ setWall(x,0,true); setWall(x,GH-1,true); }
  for(int y=0;y<GH;y++){ setWall(0,y,true); setWall(GW-1,y,true); }

  // ---------------------------------------------------------
  // EASIER MAZE: fewer obstacles overall
  // (Low/Med/High still matters, but all are reduced)
  // ---------------------------------------------------------
  int density = (gMazeDensity==0? 30 : gMazeDensity==1? 50 : 70);
  if(gDiff==DIFF_EASY) density = density * 8 / 10;
  if(gDiff==DIFF_HARD) density = density * 11 / 10;

  // spawn bubble
  player = {(int8_t)(GW/2),(int8_t)(GH/2)};
  for(int dy=-2;dy<=2;dy++){
    for(int dx=-3;dx<=3;dx++){
      int xx=player.x+dx, yy=player.y+dy;
      if(xx>=0&&xx<GW&&yy>=0&&yy<GH) setWall(xx,yy,false);
    }
  }

  // random walls
  for(int k=0;k<density;k++){
    int x=irand(GW), y=irand(GH);
    if(abs(x-player.x)<=3 && abs(y-player.y)<=2) continue;
    if(x==0||y==0||x==GW-1||y==GH-1) continue;
    setWall(x,y,true);
  }

  // ensure more lanes (was 25%)
  for(int y=2;y<GH-2;y+=3){
    for(int x=2;x<GW-2;x++){
      if(irand(100) < 50) setWall(x,y,false);
    }
  }

  pdir=1; pqueued=1;
  mazeStartMs = millis();
  mazeScoreSeconds = 0;
  mazeDead=false;
  mazePaused=false;

  hazardOn=false;
  // hazards only on HARD; timer still set but tick will no-op otherwise
  hazardNextMs = millis() + 5200;
}

static inline void mazeApplyInput(){
  int want=pqueued;
  if(pressed(T_UP)) want=0;
  if(pressed(T_RIGHT)) want=1;
  if(pressed(T_DOWN)) want=2;
  if(pressed(T_LEFT)) want=3;
  pqueued=want; // allow reverse
}

static inline void mazeHazardTick(){
  if(gDiff != DIFF_HARD) return;          // ONLY on HARD
  if(millis() < hazardNextMs) return;

  hazardNextMs = millis() + 5200;         // slower hazard rate
  hazardOn = !hazardOn;

  // toggle fewer edge-adjacent tiles (was 8)
  for(int i=0;i<6;i++){
    int x = (irand(2)==0) ? 1 : (GW-2);
    int y = 1 + irand(GH-2);
    if(abs(x-player.x)<=2 && abs(y-player.y)<=2) continue;
    setWall(x,y, hazardOn);
  }
}

static inline void mazeStep(){
  pdir = pqueued;

  Pt nh = player;
  if(pdir==0) nh.y--;
  if(pdir==1) nh.x++;
  if(pdir==2) nh.y++;
  if(pdir==3) nh.x--;

  // Maze Trial always SOLID boundaries
  if(nh.x<0||nh.x>=GW||nh.y<0||nh.y>=GH){ mazeDead=true; return; }
  if(isWall(nh.x, nh.y)){ mazeDead=true; return; }

  player = nh;

  uint32_t aliveMs = millis() - mazeStartMs;
  mazeScoreSeconds = (uint16_t)(aliveMs / 1000UL);
}

static inline void renderMaze(){
  display.clearDisplay();

  // Minimal timer (top-right, seconds only)
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  int sx = 128 - 6 * (int)String(mazeScoreSeconds).length() - 2;
  if(sx < 0) sx = 0;
  display.setCursor(sx, 2);
  display.print(mazeScoreSeconds);

  // Maze walls
  for(int y=0;y<GH;y++){
    for(int x=0;x<GW;x++){
      if(isWall(x,y)){
        if(x==0 || y==0 || x==GW-1 || y==GH-1)
          drawCell(x,y,true);
        else
          drawCell(x,y,false);
      }
    }
  }

  // Player
  drawCell(player.x, player.y, true);
  int px = ORGX + player.x * CELL;
  int py = ORGY + player.y * CELL;
  display.drawPixel(px+1, py+1, SSD1306_BLACK);

  // Pause overlay
  if(mazePaused){
    display.fillRect(28, 24, 72, 16, SSD1306_BLACK);
    display.drawRect(28, 24, 72, 16, SSD1306_WHITE);
    display.setCursor(46, 30);
    display.print("PAUSED");
  }

  display.display();
}

// =========================================================
// High score celebration + name entry
// =========================================================
const char NAMESET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
static const int NAMESET_N = (int)(sizeof(NAMESET)-1);

static inline int nameIndex(char c){
  for(int i=0;i<NAMESET_N;i++) if(NAMESET[i]==c) return i;
  return 0;
}
static inline char nameAt(int idx){
  idx %= NAMESET_N; if(idx<0) idx += NAMESET_N;
  return NAMESET[idx];
}

uint8_t  hsEntryMode = MODE_CLASSIC;
uint8_t  hsEntryDiff = DIFF_NORMAL;
int      hsEntryRank = -1;
uint16_t hsEntryScore = 0;
char     hsEntryName[3] = {'A','A','A'};
int      hsEntryPos = 0;

// celebration particles
struct Spark { int8_t x,y,vx,vy; uint8_t life; };
static const int N_SPARK=18;
Spark sparks[N_SPARK];
uint32_t celebrateStart=0;

static inline void startCelebration(){
  celebrateStart = millis();
  for(int i=0;i<N_SPARK;i++){
    sparks[i].x = (int8_t)(8 + irand(112));
    sparks[i].y = (int8_t)(14 + irand(44));
    sparks[i].vx = (int8_t)(irand(3)-1);
    sparks[i].vy = (int8_t)(-1 - irand(2));
    sparks[i].life = (uint8_t)(18 + irand(25));
  }
}

static inline void stepCelebration(){
  for(int i=0;i<N_SPARK;i++){
    if(sparks[i].life==0) continue;
    sparks[i].x += sparks[i].vx;
    sparks[i].y += sparks[i].vy;
    if((millis() & 1) == 0) sparks[i].vy++; // gravity
    if(sparks[i].x<1) sparks[i].x=1;
    if(sparks[i].x>126) sparks[i].x=126;
    if(sparks[i].y<11) sparks[i].y=11;
    if(sparks[i].y>62) sparks[i].y=62;
    sparks[i].life--;
  }
}

static inline void renderCelebration(const char* subtitle){
  display.clearDisplay();
  headerBar("NEW HIGH SCORE!");
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  for(int i=0;i<N_SPARK;i++){
    if(sparks[i].life==0) continue;
    display.drawPixel(sparks[i].x, sparks[i].y, SSD1306_WHITE);
    if((sparks[i].life & 3)==0) display.drawPixel(sparks[i].x+1, sparks[i].y, SSD1306_WHITE);
  }

  display.setCursor(18, 26);
  display.print(subtitle);

  display.setCursor(14, 42);
  display.print("Press A to enter name");
  display.display();
}

static inline void renderNameEntry(){
  display.clearDisplay();
  headerBar("ENTER INITIALS");

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(10, 14);
  display.print("Top ");
  display.print(hsEntryRank+1);
  display.print("  Score ");
  display.print(hsEntryScore);

  display.setCursor(10, 24);
  display.print(hsEntryMode==MODE_CLASSIC ? "Classic" : "Maze");
  display.print(" ");
  display.print(hsEntryDiff==DIFF_EASY?"Easy":hsEntryDiff==DIFF_NORMAL?"Norm":"Hard");

  // Big letters
  display.setTextSize(2);
  int baseX=32, y=36;
  for(int i=0;i<3;i++){
    int x = baseX + i*22;
    if(i==hsEntryPos){
      display.fillRect(x-2, y-2, 18, 20, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(x,y);
    char c = hsEntryName[i];
    if(c=='_') c=' ';
    display.print(c);
  }

  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(4, 56);
  display.print("UP/DN letter  L/R move  A ok  B back");
  display.display();
}

// =========================================================
// Screens / State machine
// =========================================================
enum Screen : uint8_t {
  SC_SPLASH,
  SC_MENU,
  SC_SETTINGS,
  SC_HISCORES,
  SC_ABOUT,
  SC_PLAY_CLASSIC,
  SC_PLAY_MAZE,
  SC_GAMEOVER,
  SC_CELEBRATE,
  SC_NAMEENTRY
};

Screen screen = SC_SPLASH;
int animTick=0;

// menu (scrollable list so About is not cut)
int menuIndex=0;
int menuScroll=0;
static const int MENU_VISIBLE = 4;
static const int MENU_COUNT   = 5;

static const char* menuLabels[MENU_COUNT] = {
  "Classic Snake",
  "Maze Trial",
  "Settings",
  "High Scores",
  "About"
};

// settings
int settingsRow=0; // 0 diff, 1 walls, 2 maze density

// high scores viewing
uint8_t viewMode = MODE_CLASSIC;
uint8_t viewDiff = DIFF_NORMAL;

// timers
uint32_t lastStepMs=0;
uint32_t lastRenderMs=0;

// last played mode for game over display
uint8_t lastModePlayed = MODE_CLASSIC;

// =========================================================
// Render screens
// =========================================================
static inline void renderSplash(){
  display.clearDisplay();
  headerBar("SNAKE ARCADE");
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  display.setCursor(20, 20);
  display.print("SNAKE");

  int x = 10 + (animTick % 108);
  display.drawFastHLine(10, 42, 108, SSD1306_WHITE);
  display.fillRect(x, 40, 8, 4, SSD1306_WHITE);

  if((millis() / 500) % 2 == 0){
    display.setTextSize(1);
    display.setCursor(18, 54);
    display.print("Press A to start");
  }

  display.display();
}

static inline void drawMenuItem(int y, const char* label, bool sel){
  if(sel){
    display.fillRect(10, y-1, 108, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(14, y);
  display.print(label);
  display.setTextColor(SSD1306_WHITE);
}

static inline void renderMenu(){
  display.clearDisplay();

  stepMenuSnake();
  drawMenuSnake();

  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(34, 0);
  display.print("SNAKE");

  display.setTextSize(1);
  for(int i=0;i<MENU_VISIBLE;i++){
    int item = menuScroll + i;
    if(item >= MENU_COUNT) break;
    drawMenuItem(18 + i*10, menuLabels[item], item==menuIndex);
  }

  if(menuScroll > 0) display.drawTriangle(120,18, 124,18, 122,14, SSD1306_WHITE);
  if(menuScroll + MENU_VISIBLE < MENU_COUNT)
    display.drawTriangle(120,58, 124,58, 122,62, SSD1306_WHITE);

  display.display();
}

static inline void renderSettings(){
  display.clearDisplay();
  headerBar("SETTINGS");
  display.setTextSize(1);

  if(settingsRow==0) display.fillRect(6, 18, 116, 12, SSD1306_WHITE);
  if(settingsRow==1) display.fillRect(6, 34, 116, 12, SSD1306_WHITE);
  if(settingsRow==2) display.fillRect(6, 50, 116, 12, SSD1306_WHITE);

  display.setCursor(10, 20);
  display.setTextColor(settingsRow==0 ? SSD1306_BLACK : SSD1306_WHITE);
  display.print("Difficulty: ");
  display.print(gDiff==DIFF_EASY?"Easy":gDiff==DIFF_NORMAL?"Normal":"Hard");

  display.setCursor(10, 36);
  display.setTextColor(settingsRow==1 ? SSD1306_BLACK : SSD1306_WHITE);
  display.print("Walls: ");
  display.print(gWrapWalls ? "Wrap" : "Solid");

  display.setCursor(10, 52);
  display.setTextColor(settingsRow==2 ? SSD1306_BLACK : SSD1306_WHITE);
  display.print("Maze: ");
  display.print(gMazeDensity==0?"Low":gMazeDensity==1?"Med":"High");

  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 12);
  display.print("L/R change  A/B back");
  display.display();
}

static inline void renderAbout(){
  display.clearDisplay();
  headerBar("ABOUT");
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(10, 16);
  display.print("Snake Arcade Deluxe");
  display.setCursor(10, 28);
  display.print("XIAO RP2040 + OLED");
  display.setCursor(10, 40);
  display.print("A pause  A+B hold menu");
  display.setCursor(10, 54);
  display.print("Build: Deluxe");
  display.display();
}

static inline void renderHighScores(){
  display.clearDisplay();
  headerBar("HIGH SCORES");
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(2,12);
  display.print(viewMode==MODE_CLASSIC ? "CLASSIC" : "MAZE");

  display.setCursor(72,12);
  display.print(viewDiff==DIFF_EASY?"EASY":viewDiff==DIFF_NORMAL?"NORMAL":"HARD");

  for(int i=0;i<5;i++){
    int y = 22 + i*8;

    if(i==0){
      display.fillRect(0, y-1, 128, 9, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    display.setCursor(6, y);
    display.print(i+1);
    display.print(".");

    display.setCursor(24, y);
    display.print(hsName[viewMode][viewDiff][i][0]);
    display.print(hsName[viewMode][viewDiff][i][1]);
    display.print(hsName[viewMode][viewDiff][i][2]);

    display.setCursor(78, y);
    display.print(hsScore[viewMode][viewDiff][i]);

    display.setTextColor(SSD1306_WHITE);
  }

  display.setCursor(2,56);
  display.print("UP/DN mode  L/R diff  A/B back");
  display.display();
}

static inline void renderGameOver(uint16_t finalScore){
  display.clearDisplay();
  headerBar("GAME OVER");
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(10, 18);
  display.print("Mode: ");
  display.print(lastModePlayed==MODE_CLASSIC ? "Classic" : "Maze Trial");

  display.setCursor(10, 30);
  display.print("Score: ");
  display.print(finalScore);

  display.setCursor(10, 42);
  display.print("Best: ");
  display.print(hsScore[lastModePlayed][gDiff][0]);

  display.setCursor(6, 56);
  display.print("A=Retry  B=Menu");
  display.display();
}

// =========================================================
// Flow helpers
// =========================================================
static inline void goMenu(){
  screen = SC_MENU;
  menuIndex = 0;
  menuScroll = 0;
  resetMenuSnake();
  lastRenderMs = 0;
}

static inline void startClassic(){
  lastModePlayed = MODE_CLASSIC;
  resetClassic();
  lastStepMs = millis();
  lastRenderMs = 0;
  screen = SC_PLAY_CLASSIC;
}

static inline void startMaze(){
  lastModePlayed = MODE_MAZE;
  genMaze();
  lastStepMs = millis();
  lastRenderMs = 0;
  screen = SC_PLAY_MAZE;
}

static inline void beginHighScoreFlow(uint8_t mode, uint8_t diff, int rank, uint16_t score){
  hsEntryMode = mode;
  hsEntryDiff = diff;
  hsEntryRank = rank;
  hsEntryScore = score;
  hsEntryName[0]='A'; hsEntryName[1]='A'; hsEntryName[2]='A';
  hsEntryPos = 0;

  startCelebration();
  screen = SC_CELEBRATE;
  lastRenderMs = 0;
}

// =========================================================
// Setup / Loop
// =========================================================
void setup(){
  Serial.begin(115200);
  delay(50);

  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)){
    while(true) delay(100);
  }

  rngState = seedRand() | 1;
  loadSave();
  calibrate_touch();

  resetMenuSnake();
  screen = SC_SPLASH;
  lastRenderMs = 0;
  renderSplash();
}

void loop(){
  update_touch();
  animTick++;

  // global exit to menu (A+B hold)
  if(screen != SC_SPLASH && abLongHeld()){
    wipe();
    goMenu();
    return;
  }

  // ---------------- Splash ----------------
  if(screen == SC_SPLASH){
    if(pressed(T_A) || pressed(T_B)){
      wipe();
      goMenu();
      return;
    }

    if(millis() - lastRenderMs > 120){
      renderSplash();
      lastRenderMs = millis();
    }
    delay(8);
    return;
  }

  // ---------------- Menu ----------------
  if(screen == SC_MENU){
    if(pressed(T_UP)){
      if(menuIndex>0) menuIndex--;
    }
    if(pressed(T_DOWN)){
      if(menuIndex<MENU_COUNT-1) menuIndex++;
    }

    // auto-scroll window
    if(menuIndex < menuScroll) menuScroll = menuIndex;
    if(menuIndex >= menuScroll + MENU_VISIBLE) menuScroll = menuIndex - MENU_VISIBLE + 1;

    if(pressed(T_A)){
      wipe();
      if(menuIndex==0) { startClassic(); return; }
      if(menuIndex==1) { startMaze(); return; }
      if(menuIndex==2) { screen=SC_SETTINGS; settingsRow=0; lastRenderMs=0; return; }
      if(menuIndex==3) { screen=SC_HISCORES; viewMode=MODE_CLASSIC; viewDiff=gDiff; lastRenderMs=0; return; }
      if(menuIndex==4) { screen=SC_ABOUT; lastRenderMs=0; return; }
    }

    if(pressed(T_B)){
      wipe();
      screen = SC_SPLASH;
      lastRenderMs = 0;
      return;
    }

    if(millis() - lastRenderMs > 70){
      renderMenu();
      lastRenderMs = millis();
    }
    delay(5);
    return;
  }

  // ---------------- Settings ----------------
  if(screen == SC_SETTINGS){
    if(pressed(T_UP) && settingsRow>0) settingsRow--;
    if(pressed(T_DOWN) && settingsRow<2) settingsRow++;

    if(pressed(T_LEFT) || pressed(T_RIGHT)){
      int dirLR = pressed(T_RIGHT) ? 1 : -1;

      if(settingsRow==0){
        int v = (int)gDiff + dirLR;
        if(v<0) v=2; if(v>2) v=0;
        gDiff = (uint8_t)v;
      } else if(settingsRow==1){
        gWrapWalls = !gWrapWalls;
      } else {
        int v = (int)gMazeDensity + dirLR;
        if(v<0) v=2; if(v>2) v=0;
        gMazeDensity = (uint8_t)v;
      }
    }

    if(pressed(T_A) || pressed(T_B)){
      wipe();
      goMenu();
      return;
    }

    if(millis() - lastRenderMs > 90){
      renderSettings();
      lastRenderMs = millis();
    }
    delay(8);
    return;
  }

  // ---------------- High Scores ----------------
  if(screen == SC_HISCORES){
    if(pressed(T_UP) || pressed(T_DOWN)){
      viewMode = (viewMode==MODE_CLASSIC) ? MODE_MAZE : MODE_CLASSIC; // shows MAZE highs too
    }
    if(pressed(T_LEFT) && viewDiff>0) viewDiff--;
    if(pressed(T_RIGHT) && viewDiff<2) viewDiff++;

    if(pressed(T_B) || pressed(T_A)){
      wipe();
      goMenu();
      return;
    }

    if(millis() - lastRenderMs > 120){
      renderHighScores();
      lastRenderMs = millis();
    }
    delay(8);
    return;
  }

  // ---------------- About ----------------
  if(screen == SC_ABOUT){
    if(pressed(T_A) || pressed(T_B)){
      wipe();
      goMenu();
      return;
    }
    if(millis() - lastRenderMs > 200){
      renderAbout();
      lastRenderMs = millis();
    }
    delay(8);
    return;
  }

  // ---------------- Play: Classic ----------------
  if(screen == SC_PLAY_CLASSIC){
    if(pressed(T_A)) paused = !paused;
    if(abEdgeToggle()) paused = !paused;

    if(paused && pressed(T_B)){
      wipe();
      paused=false;
      goMenu();
      return;
    }

    applyMoveInput();

    uint16_t tick = tickMsForDiff(gDiff);
    if(!paused && (millis() - lastStepMs >= tick)){
      lastStepMs += tick;
      stepClassic();
      if(dead){
        int r = hsQualifies(MODE_CLASSIC, gDiff, classicScore);
        wipe();
        if(r >= 0){
          beginHighScoreFlow(MODE_CLASSIC, gDiff, r, classicScore);
        } else {
          screen = SC_GAMEOVER;
          lastRenderMs = 0;
        }
        dead=false;
        return;
      }
    }

    if(millis() - lastRenderMs > 45){
      renderClassic();
      lastRenderMs = millis();
    }
    delay(4);
    return;
  }

  // ---------------- Play: Maze Trial ----------------
  if(screen == SC_PLAY_MAZE){
    if(pressed(T_A)) mazePaused = !mazePaused;
    if(abEdgeToggle()) mazePaused = !mazePaused;

    if(mazePaused && pressed(T_B)){
      wipe();
      mazePaused=false;
      goMenu();
      return;
    }

    mazeApplyInput();

    if(!mazePaused) mazeHazardTick();

    uint16_t tick = tickMsForDiff(gDiff);
    if(gDiff==DIFF_HARD && tick>15) tick -= 15;

    if(!mazePaused && (millis() - lastStepMs >= tick)){
      lastStepMs += tick;
      mazeStep();
      if(mazeDead){
        int r = hsQualifies(MODE_MAZE, gDiff, mazeScoreSeconds);
        wipe();
        if(r >= 0){
          beginHighScoreFlow(MODE_MAZE, gDiff, r, mazeScoreSeconds);
        } else {
          screen = SC_GAMEOVER;
          lastRenderMs = 0;
        }
        mazeDead=false;
        return;
      }
    }

    if(millis() - lastRenderMs > 55){
      renderMaze();
      lastRenderMs = millis();
    }
    delay(4);
    return;
  }

  // ---------------- Celebrate ----------------
  if(screen == SC_CELEBRATE){
    stepCelebration();

    const char* sub = (hsEntryMode==MODE_CLASSIC) ? "Classic record!" : "Maze record!";
    if(millis() - lastRenderMs > 40){
      renderCelebration(sub);
      lastRenderMs = millis();
    }

    if(pressed(T_A)){
      wipe();
      screen = SC_NAMEENTRY;
      lastRenderMs = 0;
      return;
    }

    if(millis() - celebrateStart > 1800){
      wipe();
      screen = SC_NAMEENTRY;
      lastRenderMs = 0;
      return;
    }

    delay(6);
    return;
  }

  // ---------------- Name Entry ----------------
  if(screen == SC_NAMEENTRY){
    if(pressed(T_UP) || pressed(T_DOWN)){
      int idxc = nameIndex(hsEntryName[hsEntryPos]);
      idxc += pressed(T_UP) ? 1 : -1;
      hsEntryName[hsEntryPos] = nameAt(idxc);
    }
    if(pressed(T_LEFT) && hsEntryPos>0) hsEntryPos--;
    if(pressed(T_RIGHT) && hsEntryPos<2) hsEntryPos++;

    if(pressed(T_B)){
      if(hsEntryPos>0) hsEntryPos--;
      hsEntryName[hsEntryPos] = 'A';
    }

    if(pressed(T_A)){
      if(hsEntryPos<2) hsEntryPos++;
      else {
        hsInsert(hsEntryMode, hsEntryDiff, hsEntryRank, hsEntryScore, hsEntryName);
        commitSave();

        wipe();
        viewMode = hsEntryMode;
        viewDiff = hsEntryDiff;
        screen = SC_HISCORES;
        lastRenderMs = 0;
        return;
      }
    }

    if(millis() - lastRenderMs > 60){
      renderNameEntry();
      lastRenderMs = millis();
    }
    delay(6);
    return;
  }

  // ---------------- Game Over ----------------
  if(screen == SC_GAMEOVER){
    uint16_t finalScore = (lastModePlayed==MODE_CLASSIC) ? classicScore : mazeScoreSeconds;

    if(pressed(T_A)){
      wipe();
      if(lastModePlayed==MODE_CLASSIC) startClassic();
      else startMaze();
      return;
    }
    if(pressed(T_B)){
      wipe();
      goMenu();
      return;
    }

    if(millis() - lastRenderMs > 150){
      renderGameOver(finalScore);
      lastRenderMs = millis();
    }
    delay(8);
    return;
  }
}
