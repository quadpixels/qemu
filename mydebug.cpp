// 2022-01-03
//
// Build in standalone mode:
// g++ mydebug.cpp -lGL -lGLU -lGLEW -lglut -lX11 -DQEMU_BUDDY_STANDALONE

// For usage with QEMU:
// * Add "-icount auto" for CPU Inst counts.

#include "mydebug.hpp"

#include <unistd.h>
#include <assert.h>

#include <GL/glew.h>
#include <GL/glut.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <string>
#include <chrono>
#include <algorithm>
#include <set>

extern "C" {
  extern void InjectNpcm7xxSMBusNack(int i2cid);
  extern void DumpPhysicalMemoryForMyDebug(int64_t addr, int stride, int64_t size, unsigned char* outbuf);
}

int WIN_W = 960, WIN_H = 480;
int FRAME_RATE = 20; // Improves speed and don't slow down QEMU too much
int g_argc; char** g_argv;
bool g_is_dirty = false;
int g_mouse_x = 0, g_mouse_y = 0;
int g_i2cbus_serial = 0;

LogView* g_logview;
CPUStateView* g_cpustateview;
NPCM7XXStateView* g_npcm7xxstateview;
I2CBusStateView* g_i2cbusstateview;
MemView* g_memview;
std::vector<MyView*> g_views;
int g_highlighted_view_idx = -999;
MyView* g_highlighted_view;

long g_last_millis = 0;
std::chrono::time_point<std::chrono::steady_clock> g_timepoint0;

static bool g_buddy_started = false;
int IsBuddyStarted() {
  if (g_buddy_started) return 1;
  else return 0;
}

static bool g_flags[12]; // Keyboard flags: Up, Down, Right, Left, Tab, PgUp, PgDn
static std::set<std::string> g_in_flight_i2c_txns;
static bool g_inject_flag = false;

// ====================== To be called from QEMU side ================

int IsBuddNeedsUpdate() {
  return 1;
}

void AddLogEntry(const char* s) {
  g_logview->AddLogEntry(std::string(s));
}

void UpdateCPUICount(CPUState* cpu, int64_t executed) {
  g_cpustateview->UpdateCPUICount(cpu, executed);
}

void UpdateWatchdogState(int idx_, const char* irq_, const char* reset_, int64_t rem_ns_, int64_t qemu_ns_) {
  g_npcm7xxstateview->UpdateWatchdogState(idx_, std::string(irq_), std::string(reset_), rem_ns_, qemu_ns_);
}

void AddI2CBus(const char* desc, void* opaque, int i2cid) {
  g_i2cbusstateview->AddI2CBus(desc, opaque, i2cid);
}

void OnI2CTransactionStart(const char* desc, char* is_inject) {
  g_in_flight_i2c_txns.insert(std::string(desc));
  g_i2cbusstateview->OnI2CTransactionStart(desc, is_inject);
  if (is_inject && (*is_inject)) {
    g_inject_flag = true;
  }
}

void OnI2CTransactionEnd(const char* desc) {
  g_in_flight_i2c_txns.clear();
  g_inject_flag = false;
}

int GetI2CSerial(void) {
  return g_i2cbus_serial++;
}

int ShouldInjectNACK(const char* desc) {
  if (g_inject_flag) {
    if (g_in_flight_i2c_txns.find(std::string(desc)) !=
        g_in_flight_i2c_txns.end()) {
      return true;
    }
  }
  return false;
}

// ============================================================

int TextWidth(const std::string info) {
  glWindowPos2i(0, -1);
  for (const char c : info) {
    glutBitmapCharacter(GLUT_BITMAP_HELVETICA_10, c);
  }
  int raster_pos[4];
  glGetIntegerv(GL_CURRENT_RASTER_POSITION, raster_pos);
  return raster_pos[0];
}

void GlutBitmapString(int canvas_x, int canvas_y, const std::string& info) {
  const int dy = WIN_H - canvas_y;
  glWindowPos2i(canvas_x, dy);
  for (const char c : info) {
    glutBitmapCharacter(GLUT_BITMAP_HELVETICA_10, c);
  }
}

// Mini p5js-like drawing routines
float g_red = 1, g_green = 1, g_blue = 1;

void color(const float r, const float g, const float b) {
  g_red = r; g_green = g; g_blue = b;
}

void rect(int canvas_x0, int canvas_y0, int canvas_x1, int canvas_y1) {
  // With the transformation currently set, top-left is (0, 0).
  float r=g_red, g=g_green, b=g_blue;
  glBegin(GL_LINE_LOOP);
  glColor3f(r, g, b); glVertex2i(canvas_x0, canvas_y0);
  glColor3f(r, g, b); glVertex2i(canvas_x1, canvas_y0);
  glColor3f(r, g, b); glVertex2i(canvas_x1, canvas_y1);
  glColor3f(r, g, b); glVertex2i(canvas_x0, canvas_y1);
  glEnd();

  // To make sure the corners are drawn
  glBegin(GL_POINTS);
  glColor3f(r, g, b); glVertex2i(canvas_x0, canvas_y0);
  glColor3f(r, g, b); glVertex2i(canvas_x1, canvas_y0);
  glColor3f(r, g, b); glVertex2i(canvas_x1, canvas_y1);
  glColor3f(r, g, b); glVertex2i(canvas_x0, canvas_y1);
  glEnd();
}

void fillRect(int canvas_x0, int canvas_y0, int canvas_x1, int canvas_y1) {
  // With the transformation currently set, top-left is (0, 0).
  float r=1, g=1, b=1;
  glBegin(GL_TRIANGLE_FAN);
  glColor3f(r, g, b); glVertex2i(canvas_x0, canvas_y0);
  glColor3f(r, g, b); glVertex2i(canvas_x1, canvas_y0);
  glColor3f(r, g, b); glVertex2i(canvas_x1, canvas_y1);
  glColor3f(r, g, b); glVertex2i(canvas_x0, canvas_y1);
  glEnd();
  rect(canvas_x0, canvas_y0, canvas_x1, canvas_y1); // make sure the boundary is drawn
}

#ifdef QEMU_BUDDY_STANDALONE
int main(int argc, char** argv) {
  MyBuddyInit(nullptr);
  return 0;
}
#endif

void keyboard(unsigned char key, int x, int y) {
  if (key == 9 || key == '`') {
    g_flags[4] = true;
    if (g_highlighted_view_idx == -999) {
      if (key == '`') { g_highlighted_view_idx = g_views.size()-1; }
      else g_highlighted_view_idx = 0;
    } else {
      if (key == '`') {
        g_highlighted_view_idx = (g_highlighted_view_idx + g_views.size() - 1) % g_views.size();
      } else {
        g_highlighted_view_idx ++;
      }

      if (g_highlighted_view_idx == int(g_views.size())) {
        g_highlighted_view_idx = -999;
      }
    }
    if (g_highlighted_view_idx >= 0 && g_highlighted_view_idx < int(g_views.size())) {
      g_highlighted_view = g_views[g_highlighted_view_idx];
    } else {
      g_highlighted_view = nullptr;
    }
  }

  if (g_highlighted_view != nullptr) {
    g_highlighted_view->OnKeyDown(key);
  }
}

void keyboardUp(unsigned char key, int x, int y) {
  switch (key) {
    case 9: {
      g_flags[4] = false; break;
    }
  }
}

void keyboard2(int key, int x, int y) {
  switch (key) {
    case GLUT_KEY_UP: {
      g_flags[0] = true; break;
    }
    case GLUT_KEY_DOWN: {
      g_flags[1] = true; break;
    }
    case GLUT_KEY_RIGHT: {
      g_flags[2] = true; break;
    }
    case GLUT_KEY_LEFT: {
      g_flags[3] = true; break;
    }
    case GLUT_KEY_PAGE_UP: {
      g_flags[5] = true; break;
    }
    case GLUT_KEY_PAGE_DOWN: {
      g_flags[6] = true; break;
    }
  }

  if (g_highlighted_view != nullptr) {
    g_highlighted_view->OnKeyDown(key);
  }
}

void keyboard2Up(int key, int x, int y) {
  switch (key) {
    case GLUT_KEY_UP: {
      g_flags[0] = false; break;
    }
    case GLUT_KEY_DOWN: {
      g_flags[1] = false; break;
    }
    case GLUT_KEY_RIGHT: {
      g_flags[2] = false; break;
    }
    case GLUT_KEY_LEFT: {
      g_flags[3] = false; break;
    }
    case GLUT_KEY_PAGE_UP: {
      g_flags[5] = false; break;
    }
    case GLUT_KEY_PAGE_DOWN: {
      g_flags[6] = false; break;
    }
  }
}

void SetOrthographicProjection() {
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrtho(0, WIN_W, 0, WIN_H, -1, 1);
  glScalef(1, -1, 1);
  glTranslatef(0, -WIN_H, 0);
  glMatrixMode(GL_MODELVIEW);
}

static void resize(int width, int height)
{
  const float ar = (float) width / (float) height;
  WIN_W = width;
  WIN_H = height;
  glViewport(0, 0, width, height);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glFrustum(-ar, ar, -1.0, 1.0, 2.0, 100.0);     glMatrixMode(GL_MODELVIEW);
  glLoadIdentity() ;
}

long millis() {
  std::chrono::time_point<std::chrono::steady_clock> n = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(n - g_timepoint0).count();
}

void render() {
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glClearColor(0.3f, 0.3f, 0.3f, 1.0f);
  SetOrthographicProjection();
  glPushMatrix();
  glLoadIdentity();
  glDepthFunc(GL_ALWAYS);

  for (MyView* v : g_views) {
    v->Render();
  }


  const int btn_w = 6;
  const int nrows = 3, ncols = 4;
  const int dx0 = WIN_W - 1 - btn_w * ncols;
  const int dy0 = WIN_H - 1 - btn_w * nrows;
  GlutBitmapString(WIN_W-24, dy0-1, std::string("Keys:"));
  int row = 0, col = 0;
  for (int i=0; i<12; i++) {
    const int dx = dx0+btn_w*col, dy = dy0+btn_w*row;
    rect(dx, dy, dx+btn_w, dy+btn_w);
    if (g_flags[i]) {
      fillRect(dx+2, dy+2, dx+btn_w-2, dy+btn_w-2);
    }
    ++ col;
    if (col == ncols) {
      col = 0; ++ row;
    }
  }

  {
    int dy1 = WIN_H - 1 - 11*2;
    GlutBitmapString(WIN_W-64, dy1, "Mouse:"); dy1 += 11;
    GlutBitmapString(WIN_W-64, dy1, "X: " + std::to_string(g_mouse_x)); dy1 += 11;
    GlutBitmapString(WIN_W-64, dy1, "Y: " + std::to_string(g_mouse_y));
  }

  glPopMatrix();
  glutSwapBuffers();

  long ms = millis();
  long delta_ms = ms - g_last_millis;
  g_last_millis = ms;

  const long preferred_sleep_ms = 1000 / FRAME_RATE;
  if (delta_ms < preferred_sleep_ms) {
    usleep(1000 * (preferred_sleep_ms - delta_ms));
  }

  glutPostRedisplay();
}

void update() {
}

static void mouse(int button, int state, int x, int y) {
  g_mouse_x = x; g_mouse_y = y;
  if (state == GLUT_DOWN) {
    g_i2cbusstateview->OnMouseDown(button);
  }
}

static void motion(int x, int y) {
  g_mouse_x = x; g_mouse_y = y;
}

static void passiveMotion(int x, int y) {
  g_mouse_x = x; g_mouse_y = y;
}

void* MyBuddyInit(void* x) {
  if (g_buddy_started) return nullptr;
  g_buddy_started = true;
  g_timepoint0 = std::chrono::steady_clock::now();

  // Set up
  g_logview = new LogView();
  g_logview->SetPosition(0, 160);
  g_logview->SetSize(320, 320);

  g_cpustateview = new CPUStateView();
  g_cpustateview->SetPosition(0, 0);
  g_cpustateview->SetSize(180, 80);

  g_npcm7xxstateview = new NPCM7XXStateView();
  g_npcm7xxstateview->SetPosition(0, 80);
  g_npcm7xxstateview->SetSize(320, 80);

  g_i2cbusstateview = new I2CBusStateView();
  g_i2cbusstateview->SetPosition(180, 0);
  g_i2cbusstateview->SetSize(320, 80);

  g_memview = new MemView();
  g_memview->SetPosition(320, 80);
  g_memview->SetSize(640, 320);

  XInitThreads();
  printf("[MyBuddyInit] Hey!\n");
  glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE);
  glutInitWindowSize(WIN_W, WIN_H);
  glutInit(&g_argc, g_argv);
  glutCreateWindow("My QEMU Buddy System.");
  glutDisplayFunc(render);
  glutIdleFunc(update);
  glutReshapeFunc(resize);
  glutKeyboardFunc(keyboard);
  glutKeyboardUpFunc(keyboardUp);
  glutSpecialFunc(keyboard2);
  glutSpecialUpFunc(keyboard2Up);
  glutMouseFunc(mouse);
  glutMotionFunc(motion);
  glutPassiveMotionFunc(passiveMotion);
  
  glewInit();

  int major_version, minor_version;
  glGetIntegerv(GL_MAJOR_VERSION, &major_version);
  glGetIntegerv(GL_MINOR_VERSION, &minor_version);
  printf("GL_MAJOR_VERSION=%d GL_MINOR_VERSION=%d\n", major_version, minor_version);
  printf("GL_VERSION=%s\n", glGetString(GL_VERSION));

#ifdef QEMU_BUDDY_STANDALONE
  for (int i=0;i<10;i++) {
    g_logview->AddLogEntry("Log Entry #" + std::to_string(i));
  }
  g_cpustateview->UpdateCPUICount((CPUState*)0x1234, 10000);
#endif

  g_views.push_back(g_cpustateview);
  g_views.push_back(g_i2cbusstateview);
  g_views.push_back(g_npcm7xxstateview);
  g_views.push_back(g_logview);
  g_views.push_back(g_memview);
  
  glutMainLoop();
  return nullptr;
}

void MyView::SetPosition(int _x, int _y) {
  x = _x; y = _y;
}

void MyView::SetSize(int _w, int _h) {
  w = _w; h = _h;
}

void MyView::DrawBorder() {
  float r=1, g=1, b=1, line_width = 1;
  if (this == g_highlighted_view) {
    r=0; b=0; line_width = 2;
  }
  // With the transformation currently set, top-left is (0, 0).
  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glLineWidth(line_width);
  glBegin(GL_LINE_LOOP);
  glColor3f(r, g, b); glVertex2i(x, y);
  glColor3f(r, g, b); glVertex2i(x+w, y);
  glColor3f(r, g, b); glVertex2i(x+w, y+h);
  glColor3f(r, g, b); glVertex2i(x, y+h);
  glEnd();

  glPopAttrib();
}

CPUStateView::CPUStateView() {
  SetPosition(0, 0);
  SetSize(320, 40);
}

void CPUStateView::UpdateCPUICount(CPUState* cpu, int64_t executed) {
  //mtx_.lock();
  if (inst_counts_.find(cpu) == inst_counts_.end()) {
    inst_counts_[cpu] = 0;
    cpus_.push_back(cpu);
  }
  inst_counts_[cpu] += executed;
  //mtx_.unlock();
}


void CPUStateView::Render() {
  const int TEXT_SIZE = 11;
  DrawBorder();

  int canvas_y = y + TEXT_SIZE;
  std::string info = std::to_string(cpus_.size()) + " CPUs";
  GlutBitmapString(x, canvas_y, info);
  canvas_y += TEXT_SIZE;
  for (int i=0; i<int(cpus_.size()); i++) {
    int64_t cnt = inst_counts_[cpus_[i]];
    if (cnt > 0) {
      bool overflowed = false;
      if (canvas_y + TEXT_SIZE >= y+h) {
        overflowed = true;
      }

      if (cnt > 1000000L) {
        info = "#" + std::to_string(i) + ": " + std::to_string(cnt/1000000) + " M";
      } else {
        info = "#" + std::to_string(i) + ": " + std::to_string(cnt);
      }
      if (overflowed) {
        info += " ...(omitted)";
      }
      GlutBitmapString(x, canvas_y, info);

      if (overflowed) break;
      canvas_y += TEXT_SIZE;
    }
  }
}

LogView::LogView() {
  logs_.resize(500);
  num_entries_ = 0;
  log_idx_ = 0;
}

void LogView::AddLogEntry(const std::string& s) {
  logs_[log_idx_] = s;
  log_idx_ = (log_idx_ + 1) % (int(logs_.size()));
  ++ num_entries_;
}

void LogView::Render() {
  const int TEXT_SIZE = 11;
  DrawBorder();
  glBegin(GL_LINES);
  glVertex2d(x, y+TEXT_SIZE);
  glVertex2d(x+w, y+TEXT_SIZE);
  glEnd();
  const int BORDER_LINES = 1;


  int canvas_y = y+h-2;
  const int N = int(logs_.size());
  int idx_max = -1, idx_min = 100000000;
  int ndisp = 0;
  for (int idx=(log_idx_-1+N)%N, i=0; i<num_entries_ && (i+1+BORDER_LINES)*TEXT_SIZE <= h; i++, idx=(idx-1+N)%N) {
    glWindowPos2i(x, canvas_y);
    GlutBitmapString(x, canvas_y, logs_[idx]);
    canvas_y -= TEXT_SIZE;
    idx_max = std::max(idx, idx_max);
    idx_min = std::min(idx, idx_min);
    ndisp ++;
  }
  char stats[100];

  const int shown_idx_min = num_entries_ - ndisp + 1;
  const int shown_idx_max = num_entries_;

  if (num_entries_ > N) {
    sprintf(stats, "Log entries %d-%d (Showing last %d)",
      shown_idx_min, shown_idx_max, N);
  } else {
    sprintf(stats, "Log entries %d-%d", shown_idx_min, shown_idx_max);
  }
  GlutBitmapString(x, y+TEXT_SIZE-1, stats);
}

NPCM7XXStateView::NPCM7XXStateView() {
  SetPosition(0, 80);
  SetSize(320, 80);
}

void NPCM7XXStateView::UpdateWatchdogState(int idx_, std::string irq_, std::string reset_, int64_t exp_ns_, int64_t qemu_ns_) {
  if (idx_ >= int(states_.size())) {
    states_.resize(idx_+1);
  }
  MyNPCM7XXWDState& s = states_[idx_];
  s.irq = irq_; s.reset = reset_; s.expires_ns = exp_ns_;
  qemu_ns = qemu_ns_;
}

void NPCM7XXStateView::Render() {
  const int TEXT_SIZE = 11;
  DrawBorder();

  int canvas_y = y+TEXT_SIZE;
  GlutBitmapString(x, canvas_y, std::to_string(states_.size()) + " NPCM7XX WatchDogs (IRQ,Reset,Expire_ns,Approx.Rem)");
  canvas_y += TEXT_SIZE;
  const int N = int(states_.size());
  for (int i=0; i<N; i++) {
    MyNPCM7XXWDState& s = states_[i];
    std::string info = "#" + std::to_string(i) + ": ";
    if (s.irq == "") {
      info += "(null)";
    } else {
      info += s.irq;
    }
    info += ", ";

    if (s.reset == "") { info += "(null)"; }
    else { info += s.reset; }
    info += ", ";

    char buf[100];
    sprintf(buf, "%.1fms, ", s.expires_ns/1000000.0f);
    info += std::string(buf);

    int64_t rem_ns = s.expires_ns - qemu_ns;
    
    if (rem_ns > 0) {
      sprintf(buf, "%.1fms", rem_ns/1000000.0f);
    } else {
      sprintf(buf, " expired");
    }

    info += std::string(buf);

    if (canvas_y + TEXT_SIZE >= y + h) {
      info += " ... (omitted)";
    }
    GlutBitmapString(x, canvas_y, info);
    canvas_y += TEXT_SIZE;
  }
}

I2CBusStateView::I2CBusStateView() {
  hovered_i2c_idx = -999;
  pending_i2c_nacks.resize(256);
}

void I2CBusStateView::AddI2CBus(const char* desc, void* opaque, int i2c) {
  I2CBusState bs;
  states_.push_back(bs);
  i2c2idx_[desc] = int(states_.size());
  i2c2bus_[desc] = opaque;
  tx_count_last_interval.push_back(0);
}

void I2CBusStateView::Render() {
  int tx_cnt_physical_buses = 0;
  int tx_cnt_muxed_buses = 0;

  const long ms = millis();
  if (last_update_millis + 1000 < ms) {
    last_update_millis = ms;
    for (int i=0; i<int(states_.size()); i++) {
      tx_count_last_interval[i] = states_[i].tx_count;
      states_[i].tx_count = 0;
    }
  }

  const int TEXT_SIZE = 11;
  DrawBorder();
  int canvas_y = y + TEXT_SIZE;
  int nshown = 0;

  int max_count = 0;
  for (const int x : tx_count_last_interval) {
    max_count = std::max(max_count, x);
  }
  int range = max_count;
  //int range = 1;
  //while (range < max_count) {
  //  range *= 2;
  //}

  int grid_y = canvas_y + 13;
  int grid_x0 = x + 16, grid_x = grid_x0;
  const int grid_h = 8, grid_w = 16;
  int idx = 0;

  hovered_i2c_idx = -999;

  while (grid_y + grid_h < y+h && idx < int(states_.size())) {
    rect(grid_x, grid_y, grid_x+grid_w, grid_y+grid_h);

    int fill_w = 0;
    const int c = tx_count_last_interval[idx];

    if (idx <= 15) {
      tx_cnt_physical_buses += c;
    } else {
      tx_cnt_muxed_buses += c;
    }

    if (range > 0 && c > 0) {
      fill_w = int(c * 1.0f / range * (grid_w - 4));
    }
    if (c > 0) {
      fillRect(grid_x + 2, grid_y + 2, grid_x + 2 + fill_w, grid_y + grid_h - 2);
    }

    if (pending_i2c_nacks[idx] == true) {
      color(1, 1, 0);
      rect(grid_x+1, grid_y+1, grid_x+grid_w-1, grid_y+grid_h-1);
      color(1, 1, 1);
      rect(0, 0, 0, 0);
    }

    if (g_mouse_x >= grid_x && g_mouse_x <= grid_x + grid_w &&
        g_mouse_y >= grid_y && g_mouse_y <= grid_y + grid_h) {
      hovered_i2c_idx = idx;
    }

    grid_x += grid_w;
    if (grid_x + grid_w > x+w) {
      grid_x = grid_x0; grid_y += grid_h;
    }
    ++ nshown;
    ++ idx;
  }

  GlutBitmapString(x, canvas_y, std::to_string(states_.size()) + 
    " I2C Buses (showing " + std::to_string(nshown) + ")" + 
    " Max range: " + std::to_string(range) + " tx/s");
  std::string txt = "Physical/Muxed Tx: " + std::to_string(tx_cnt_physical_buses) + "/" +
    std::to_string(tx_cnt_muxed_buses);
  if (hovered_i2c_idx != -999) {
    txt = txt + " Hover: i2c-" + std::to_string(hovered_i2c_idx);
  }
  GlutBitmapString(x, canvas_y + 11, txt);
}

void I2CBusStateView::OnI2CTransactionStart(const char* desc, char* is_inject) {
  int idx = i2c2idx_.at(std::string(desc));
  states_[idx].tx_count ++;
  if (is_inject) {
    if (pending_i2c_nacks[idx]) {
      *is_inject = true;
      pending_i2c_nacks[idx] = false;
    }
  }
}

void I2CBusStateView::OnMouseDown(int button) {
  char x[100];
  if (hovered_i2c_idx != -999) {
    if (button == GLUT_LEFT_BUTTON) {
      if (pending_i2c_nacks[hovered_i2c_idx] == false) {
        sprintf(x, "pending NACK injection to i2c-%d\n", hovered_i2c_idx);
        AddLogEntry(x);
      }
      pending_i2c_nacks[hovered_i2c_idx] = true;
    } else if (button == GLUT_RIGHT_BUTTON) {
      // Assuming we're using NPCM7XX
      if (hovered_i2c_idx < 16) {
        InjectNpcm7xxSMBusNack(hovered_i2c_idx);
        sprintf(x, "Injected NACK to i2c-%d (SMBus)", hovered_i2c_idx);
        AddLogEntry(x);
      }
    }
  }
}

class BytesToRGB : public BytesToPixelIntf {
public:
	int NumBytesPerPixel() override { return 3; }
	int NumPixelDataChannels() override { return 3; }
	void BytesToPixel(unsigned char* byte_ptr, unsigned char* pixel_ptr) override {
		for (int i=0; i<3; i++) pixel_ptr[i] = byte_ptr[i];
	}
	unsigned int Format() override { return GL_RGB; }
};

class BytesToRG : public BytesToPixelIntf {
public:
	int NumBytesPerPixel() override { return 2; }
	int NumPixelDataChannels() override { return 3; }
	void BytesToPixel(unsigned char* byte_ptr, unsigned char* pixel_ptr) override {
		for (int i=0; i<2; i++) pixel_ptr[i] = byte_ptr[i];
	}
	unsigned int Format() override { return GL_RGB; }
};

MemView::MemView() {
  x = 320; y = 80; w = 320; h = 320;
  start_address = 0; stride = 1;
  update_interval_ms = 100;
  bytes2pixel = new BytesToRG();
}

void MemView::Render() {

  if (ShouldUpdate()) {
    ReadMemoryFromQEMU();
  }

  DrawBorder();
  const int px = x+4, py = y+20;
  rect(px, py, px+2+pixel_w, py+2+pixel_h);
  glWindowPos2i(px+1, WIN_H - (py+1+pixel_h));
  glDrawPixels(pixel_w, pixel_h, bytes2pixel->Format(), GL_UNSIGNED_BYTE, pixels.data());

  char buf[100];
  const int64_t range = pixel_w * pixel_h * bytes2pixel->NumBytesPerPixel() * stride;
  const float mib_lb = float(start_address / 1024.0f / 1024.0f);
  const float mib_ub = float((start_address + range) / 1024.0f / 1024.0f);

  char buf2[20];
  if (range > 1024*1024) {
    snprintf(buf2, 20, "%.2f MiB", range/1024.0f/1024.0f);
  } else if (range > 1024) {
    snprintf(buf2, 20, "%.2f KiB", range/1024.0f);
  } else {
    snprintf(buf2, 20, "%ld B", range);
  }

  snprintf(buf, 100, "0x%08lX-0x%08lX (%.2f-%.2f MiB) Stride=%d Showing %s",
    start_address, start_address + range, mib_lb, mib_ub, stride, buf2);
  GlutBitmapString(x, y+11, std::string(buf));

  if (update_interval_ms > 0) {
    snprintf(buf, 100, "Refresh every %dms", update_interval_ms);
  } else {
    snprintf(buf, 100, "Press [space] to refresh");
  }
  int tw = TextWidth(buf);
  GlutBitmapString(x+w-1-tw, y+11, std::string(buf));
}

void MemView::SetSize(int _w, int _h) {
  w = _w; h = _h;
  pixel_w = w - 16; pixel_h = h - 32;

  bytes.resize(pixel_w * pixel_h * bytes2pixel->NumBytesPerPixel());
  pixels.resize(pixel_w * pixel_h * bytes2pixel->NumPixelDataChannels());

  for (int i=0; i<int(pixels.size()); i++) {
    pixels[i] = i % 256;
  }
}

void MemView::ConvertToPixels() {
  const int nc = bytes2pixel->NumPixelDataChannels();
  const int bp = bytes2pixel->NumBytesPerPixel();
  unsigned char *byte_ptr = bytes.data();
  int px = 0, py = 0;
  for (int i=0; i<pixel_w * pixel_h; byte_ptr += bp, i++) {
    unsigned char *pixel_ptr = pixels.data() + (nc * ((pixel_h - py) * pixel_w + px));
    bytes2pixel->BytesToPixel(byte_ptr, pixel_ptr);
    px ++;
    if (px >= pixel_w) { px = 0; py ++; }
  }
}

void MemView::ReadMemoryFromQEMU() {
  std::fill(pixels.begin(), pixels.end(), 0);
  DumpPhysicalMemoryForMyDebug(start_address, stride, bytes.size(), bytes.data());
  ConvertToPixels();
  last_update_ms = millis();
}

void MemView::OnKeyDown(int k) {
  switch (k) {
    case GLUT_KEY_UP: { ScrollLines(-8); break; }
    case GLUT_KEY_DOWN: { ScrollLines(8); break; }
    case GLUT_KEY_PAGE_UP: { ScrollLines(-pixel_h); break; }
    case GLUT_KEY_PAGE_DOWN: { ScrollLines(pixel_h); break; }
    case '-': { ZoomOut(); break; }
    case '=': case '+': { ZoomIn(); break; }
    case 32: { ReadMemoryFromQEMU(); break; }
  }
}

bool MemView::ShouldUpdate() {
  if (update_interval_ms <= 0) return false;
  else if (millis() - last_update_ms >= update_interval_ms) return true;
  else return false;
}

void MemView::ScrollLines(int nlines) {
  const int delta = nlines * bytes2pixel->NumPixelDataChannels() * pixel_w * stride;
  start_address += delta;
  if (start_address < 0) {
    start_address = 0;
  }
}

void MemView::ZoomOut() {
  stride *= 2;
  if (stride >= 1024) stride = 1024;
  ReadMemoryFromQEMU();
}

void MemView::ZoomIn() {
  stride /= 2;
  if (stride <= 1) stride = 1;
  ReadMemoryFromQEMU();
}