/* ------------------------------------------------------------
name: "synth"
Code generated with Faust 2.70.3 (https://faust.grame.fr)
Compilation options: -lang cpp -ct 1 -cn KnobSynth -es 1 -mcd 16 -mdd 1024 -mdy 33 -single -ftz 0
------------------------------------------------------------ */

#ifndef  __KnobSynth_H__
#define  __KnobSynth_H__

#ifndef FAUSTFLOAT
#define FAUSTFLOAT float
#endif 

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <math.h>

#ifndef FAUSTCLASS 
#define FAUSTCLASS KnobSynth
#endif

#ifdef __APPLE__ 
#define exp10f __exp10f
#define exp10 __exp10
#endif

#if defined(_WIN32)
#define RESTRICT __restrict
#else
#define RESTRICT __restrict__
#endif

class KnobSynthSIG0 {
	
  private:
	
	int iVec1[2];
	int iRec1[2];
	
  public:
	
	int getNumInputsKnobSynthSIG0() {
		return 0;
	}
	int getNumOutputsKnobSynthSIG0() {
		return 1;
	}
	
	void instanceInitKnobSynthSIG0(int sample_rate) {
		for (int l2 = 0; l2 < 2; l2 = l2 + 1) {
			iVec1[l2] = 0;
		}
		for (int l3 = 0; l3 < 2; l3 = l3 + 1) {
			iRec1[l3] = 0;
		}
	}
	
	void fillKnobSynthSIG0(int count, float* table) {
		for (int i1 = 0; i1 < count; i1 = i1 + 1) {
			iVec1[0] = 1;
			iRec1[0] = (iVec1[1] + iRec1[1]) % 4096;
			table[i1] = std::sin(0.0015339808f * float(iRec1[0]));
			iVec1[1] = iVec1[0];
			iRec1[1] = iRec1[0];
		}
	}

};

static KnobSynthSIG0* newKnobSynthSIG0() { return (KnobSynthSIG0*)new KnobSynthSIG0(); }
static void deleteKnobSynthSIG0(KnobSynthSIG0* dsp) { delete dsp; }

static float ftbl0KnobSynthSIG0[4096];

class KnobSynth : public dsp {
	
 private:
	
	int fSampleRate;
	float fConst1;
	FAUSTFLOAT fHslider0;
	float fConst2;
	int iVec0[2];
	float fRec0[2];
	float fConst3;
	FAUSTFLOAT fHslider1;
	float fRec3[2];
	float fRec2[2];
	FAUSTFLOAT fHslider2;
	float fRec4[2];
	FAUSTFLOAT fHslider3;
	float fRec6[2];
	float fRec5[2];
	
 public:
	KnobSynth() {}

	void metadata(Meta* m) { 
		m->declare("basics.lib/name", "Faust Basic Element Library");
		m->declare("basics.lib/tabulateNd", "Copyright (C) 2023 Bart Brouns <bart@magnetophon.nl>");
		m->declare("basics.lib/version", "1.12.0");
		m->declare("compile_options", "-lang cpp -ct 1 -cn KnobSynth -es 1 -mcd 16 -mdd 1024 -mdy 33 -single -ftz 0");
		m->declare("filename", "synth.dsp");
		m->declare("maths.lib/author", "GRAME");
		m->declare("maths.lib/copyright", "GRAME");
		m->declare("maths.lib/license", "LGPL with exception");
		m->declare("maths.lib/name", "Faust Math Library");
		m->declare("maths.lib/version", "2.7.0");
		m->declare("name", "synth");
		m->declare("oscillators.lib/name", "Faust Oscillator Library");
		m->declare("oscillators.lib/version", "1.5.0");
		m->declare("platform.lib/name", "Generic Platform Library");
		m->declare("platform.lib/version", "1.3.0");
		m->declare("signals.lib/name", "Faust Signal Routing Library");
		m->declare("signals.lib/version", "1.5.0");
	}

	virtual int getNumInputs() {
		return 0;
	}
	virtual int getNumOutputs() {
		return 1;
	}
	
	static void classInit(int sample_rate) {
		KnobSynthSIG0* sig0 = newKnobSynthSIG0();
		sig0->instanceInitKnobSynthSIG0(sample_rate);
		sig0->fillKnobSynthSIG0(4096, ftbl0KnobSynthSIG0);
		deleteKnobSynthSIG0(sig0);
	}
	
	virtual void instanceConstants(int sample_rate) {
		fSampleRate = sample_rate;
		float fConst0 = std::min<float>(1.92e+05f, std::max<float>(1.0f, float(fSampleRate)));
		fConst1 = 44.1f / fConst0;
		fConst2 = 1.0f - fConst1;
		fConst3 = 1.0f / fConst0;
	}
	
	virtual void instanceResetUserInterface() {
		fHslider0 = FAUSTFLOAT(0.0f);
		fHslider1 = FAUSTFLOAT(2.2e+02f);
		fHslider2 = FAUSTFLOAT(0.0f);
		fHslider3 = FAUSTFLOAT(2.2e+02f);
	}
	
	virtual void instanceClear() {
		for (int l0 = 0; l0 < 2; l0 = l0 + 1) {
			iVec0[l0] = 0;
		}
		for (int l1 = 0; l1 < 2; l1 = l1 + 1) {
			fRec0[l1] = 0.0f;
		}
		for (int l4 = 0; l4 < 2; l4 = l4 + 1) {
			fRec3[l4] = 0.0f;
		}
		for (int l5 = 0; l5 < 2; l5 = l5 + 1) {
			fRec2[l5] = 0.0f;
		}
		for (int l6 = 0; l6 < 2; l6 = l6 + 1) {
			fRec4[l6] = 0.0f;
		}
		for (int l7 = 0; l7 < 2; l7 = l7 + 1) {
			fRec6[l7] = 0.0f;
		}
		for (int l8 = 0; l8 < 2; l8 = l8 + 1) {
			fRec5[l8] = 0.0f;
		}
	}
	
	virtual void init(int sample_rate) {
		classInit(sample_rate);
		instanceInit(sample_rate);
	}
	
	virtual void instanceInit(int sample_rate) {
		instanceConstants(sample_rate);
		instanceResetUserInterface();
		instanceClear();
	}
	
	virtual KnobSynth* clone() {
		return new KnobSynth();
	}
	
	virtual int getSampleRate() {
		return fSampleRate;
	}
	
	virtual void buildUserInterface(UI* ui_interface) {
		ui_interface->openVerticalBox("synth");
		ui_interface->addHorizontalSlider("freq1", &fHslider1, FAUSTFLOAT(2.2e+02f), FAUSTFLOAT(5e+01f), FAUSTFLOAT(1e+04f), FAUSTFLOAT(0.01f));
		ui_interface->addHorizontalSlider("freq2", &fHslider3, FAUSTFLOAT(2.2e+02f), FAUSTFLOAT(5e+01f), FAUSTFLOAT(1e+04f), FAUSTFLOAT(0.01f));
		ui_interface->addHorizontalSlider("gain1", &fHslider0, FAUSTFLOAT(0.0f), FAUSTFLOAT(0.0f), FAUSTFLOAT(1.0f), FAUSTFLOAT(0.01f));
		ui_interface->addHorizontalSlider("gain2", &fHslider2, FAUSTFLOAT(0.0f), FAUSTFLOAT(0.0f), FAUSTFLOAT(1.0f), FAUSTFLOAT(0.01f));
		ui_interface->closeBox();
	}
	
	virtual void compute(int count, FAUSTFLOAT** RESTRICT inputs, FAUSTFLOAT** RESTRICT outputs) {
		FAUSTFLOAT* output0 = outputs[0];
		float fSlow0 = fConst1 * float(fHslider0);
		float fSlow1 = fConst1 * float(fHslider1);
		float fSlow2 = fConst1 * float(fHslider2);
		float fSlow3 = fConst1 * float(fHslider3);
		for (int i0 = 0; i0 < count; i0 = i0 + 1) {
			iVec0[0] = 1;
			fRec0[0] = fSlow0 + fConst2 * fRec0[1];
			int iTemp0 = 1 - iVec0[1];
			fRec3[0] = fSlow1 + fConst2 * fRec3[1];
			float fTemp1 = ((iTemp0) ? 0.0f : fRec2[1] + fConst3 * fRec3[0]);
			fRec2[0] = fTemp1 - std::floor(fTemp1);
			fRec4[0] = fSlow2 + fConst2 * fRec4[1];
			fRec6[0] = fSlow3 + fConst2 * fRec6[1];
			float fTemp2 = ((iTemp0) ? 0.0f : fRec5[1] + fConst3 * fRec6[0]);
			fRec5[0] = fTemp2 - std::floor(fTemp2);
			output0[i0] = FAUSTFLOAT(0.5f * (fRec0[0] * ftbl0KnobSynthSIG0[std::max<int>(0, std::min<int>(int(4096.0f * fRec2[0]), 4095))] + fRec4[0] * ftbl0KnobSynthSIG0[std::max<int>(0, std::min<int>(int(4096.0f * fRec5[0]), 4095))]));
			iVec0[1] = iVec0[0];
			fRec0[1] = fRec0[0];
			fRec3[1] = fRec3[0];
			fRec2[1] = fRec2[0];
			fRec4[1] = fRec4[0];
			fRec6[1] = fRec6[0];
			fRec5[1] = fRec5[0];
		}
	}

};

#endif
