// For exposing declaration to QEMU code
// Not all QEMU headers are easy to #include, this one is easier
#include "include/qemu/typedefs.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
  int IsBuddNeedsUpdate(void);
  void* MyBuddyInit(void* x);
  int IsBuddyStarted(void);
  void AddLogEntry(const char* x);
  void AddI2CBus(const char*, void*, int);
  void UpdateCPUICount(CPUState* cpu, int64_t executed);
  void UpdateWatchdogState(int idx_, const char* irq_, const char* reset_, int64_t rem_ns_, int64_t qemu_ns_);

  // Count by I2C buses
  void OnI2CTransactionStart(const char*, char*);
  void OnI2CWrite(const char*);
  void OnI2CRead(const char*);
  void OnI2CTransactionEnd(const char*);

  int GetI2CSerial(void);
  int ShouldInjectNACK(const char*); // For muxed buses, inject on parent buses(?)
#ifdef __cplusplus
}
#endif

// For usage with mydebug.cpp
#ifdef __cplusplus
#include <vector>
#include <mutex>
#include <unordered_map>
struct MyView {
  bool is_visible;
  virtual void Render() = 0;
  MyView() : is_visible(true) {}
  virtual ~MyView() {}
  int x, y, w, h;
  void DrawBorder();
  void SetPosition(int _x, int _y);
  void SetSize(int _w, int _h);
  virtual void OnKeyDown(int keycode) {}
};

struct CPUStateView : public MyView {
  CPUStateView();
  std::unordered_map<CPUState*, int64_t> inst_counts_;
  std::vector<CPUState*> cpus_;  // in creation order

  void UpdateCPUICount(CPUState* cpu, int64_t executed);
  void Render() override;
  std::mutex mtx_;
};

struct LogView : public MyView {
  LogView();
  std::vector<std::string> logs_;
  int num_entries_;
  int log_idx_;

  void AddLogEntry(const std::string& s);
  void Render() override;
};

struct NPCM7XXStateView : public MyView {
  struct MyNPCM7XXWDState {
    std::string irq, reset;
    int64_t expires_ns;
  };
  NPCM7XXStateView();
  std::vector<struct MyNPCM7XXWDState> states_;

  int64_t qemu_ns;
  void UpdateWatchdogState(int idx_, std::string irq_, std::string reset_, int64_t exp_ns_, int64_t qemu_ns_);
  void Render() override;
};

struct I2CBusStateView : public MyView {
  struct I2CBusState {
    int serial;
    int read_count, write_count, tx_count;
    I2CBusState() {
      tx_count = 0; read_count = 0; write_count = 0;
    }
  };

  int hovered_i2c_idx;
  std::vector<int> pending_i2c_nacks;

  std::vector<int> tx_count_last_interval, read_count_last_interval, write_count_last_interval;
  std::unordered_map<std::string, int> i2c2idx_;
  std::unordered_map<std::string, void*> i2c2bus_;
  std::vector<struct I2CBusState> states_;
  I2CBusStateView();
  void AddI2CBus(const char*, void*, int);
  void Render() override;
  long last_update_millis;
  void OnI2CTransactionStart(const char*, char*);
  void OnI2CWrite(const char*);
  void OnI2CRead(const char*);
  void OnMouseDown(int button);
};

// How to visualize bytes/words
class BytesToPixelIntf {
public:
	virtual int NumBytesPerPixel() = 0;
	virtual void BytesToPixel(unsigned char* byte_ptr, unsigned char* pixel_ptr) = 0;
	virtual unsigned int Format() = 0;
	virtual ~BytesToPixelIntf() {}
	virtual int NumPixelDataChannels() = 0;
};

struct MemView : public MyView {
  MemView();
  virtual void Render() = 0;

  std::vector<unsigned char> bytes;  // bytes buffer
  std::vector<unsigned char> pixels; // RGB or RG pixels for display

  int64_t start_address;
  int stride;

  int update_interval_ms; // Update interval; 0 = do not update
  long last_update_ms;
  bool ShouldUpdate();

  virtual void ScrollLines(int nlines) = 0;
  void ZoomOut();
  void ZoomIn();

  virtual void SetSize(int _w, int _h) = 0;
  void ReadMemoryFromQEMU();
  virtual void ConvertToPixels() = 0;
  BytesToPixelIntf* bytes2pixel;
};

struct MemLinearView : public MemView {
  MemLinearView();
  void Render() override;

  int pixel_w, pixel_h;

  void SetSize(int _w, int _h) override;
  void OnKeyDown(int k);

  void ScrollLines(int nlines) override;
  void ConvertToPixels() override;
};

struct MemTiledView : public MemView {
  MemTiledView();
  void Render() override;

  int pad_x, pad_y;
  int nrows, ncols, tile_size;
  void SetSize(int _w, int _h) override;
  void OnKeyDown(int k);
  void ConvertToPixels() override;

  int64_t BytesPerRow();
  void ScrollLines(int nlines) override;
};

#endif