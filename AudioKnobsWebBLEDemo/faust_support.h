/*
  faust_support.h - minimal Faust C++ architecture support

  Faust (faust.grame.fr) compiles a .dsp file into a C++ class (see
  synth.h, generated from synth.dsp) that assumes three small interface
  types already exist: `dsp` (the base class it derives from), `UI` (the
  interface its buildUserInterface() method populates), and `Meta` (used
  only by a metadata() method this sketch never calls, but whose type
  still has to be visible for the header to compile).

  This is a trimmed, combined copy of Faust's own architecture headers
  (faust/dsp/dsp.h, faust/gui/UI.h, faust/gui/meta.h - installed
  alongside the `faust` compiler at /opt/faust/include/faust/ on this
  Mac), simplified for a single embedded target rather than a shared
  library: FAUST_API is just dropped rather than conditionally defined
  for DLL export, and unused pieces (dsp_factory, dsp_memory_manager,
  decorator_dsp, an SSE/ARM64-specific denormal-handling class - none
  of which a plain generated DSP class or this sketch's own code
  touches) are left out entirely.

  Original copyright (C) 2003-2022 GRAME, Centre National de Creation
  Musicale, distributed under Faust's architecture-file license (LGPL
  with a static-linking exception); see https://faust.grame.fr for the
  unabridged originals.
*/

#ifndef FAUST_SUPPORT_H
#define FAUST_SUPPORT_H

#ifndef FAUSTFLOAT
#define FAUSTFLOAT float
#endif

struct Soundfile;  // referenced by UI::addSoundfile(); never actually used here

// -- UI: what a generated DSP's buildUserInterface() populates. --------
//
// A real Faust UI implementation would create actual widgets here; this
// sketch's CaptureUI (see AudioKnobsDemo.ino) instead just records the
// pointer Faust hands it for each named control, so the sketch can
// write through it directly - no GUI involved.
struct UI {
  virtual ~UI() {}

  virtual void openTabBox(const char *label) = 0;
  virtual void openHorizontalBox(const char *label) = 0;
  virtual void openVerticalBox(const char *label) = 0;
  virtual void closeBox() = 0;

  virtual void addButton(const char *label, FAUSTFLOAT *zone) = 0;
  virtual void addCheckButton(const char *label, FAUSTFLOAT *zone) = 0;
  virtual void addVerticalSlider(const char *label, FAUSTFLOAT *zone, FAUSTFLOAT init,
                                  FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) = 0;
  virtual void addHorizontalSlider(const char *label, FAUSTFLOAT *zone, FAUSTFLOAT init,
                                    FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) = 0;
  virtual void addNumEntry(const char *label, FAUSTFLOAT *zone, FAUSTFLOAT init,
                            FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT step) = 0;

  virtual void addHorizontalBargraph(const char *label, FAUSTFLOAT *zone, FAUSTFLOAT min, FAUSTFLOAT max) = 0;
  virtual void addVerticalBargraph(const char *label, FAUSTFLOAT *zone, FAUSTFLOAT min, FAUSTFLOAT max) = 0;

  virtual void addSoundfile(const char *label, const char *filename, Soundfile **sf_zone) = 0;

  virtual void declare(FAUSTFLOAT * /*zone*/, const char * /*key*/, const char * /*val*/) {}
};

// -- Meta: visible only so dsp::metadata()'s signature compiles. -------
// This sketch never constructs one or calls metadata().
struct Meta {
  virtual ~Meta() {}
  virtual void declare(const char *key, const char *value) = 0;
};

// -- dsp: the base class every Faust-generated class derives from. -----
class dsp {
 public:
  dsp() {}
  virtual ~dsp() {}

  virtual int getNumInputs() = 0;
  virtual int getNumOutputs() = 0;
  virtual void buildUserInterface(UI *ui_interface) = 0;
  virtual int getSampleRate() = 0;
  virtual void init(int sample_rate) = 0;
  virtual void instanceInit(int sample_rate) = 0;
  virtual void instanceConstants(int sample_rate) = 0;
  virtual void instanceResetUserInterface() = 0;
  virtual void instanceClear() = 0;
  virtual dsp *clone() = 0;
  virtual void metadata(Meta *m) = 0;
  virtual void compute(int count, FAUSTFLOAT **inputs, FAUSTFLOAT **outputs) = 0;
};

#endif
