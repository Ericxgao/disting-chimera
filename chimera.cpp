/*
 * Chimera
 * CV-addressable breakbeat slicer for the Expert Sleepers Disting NT.
 * One beast stitched from two breaks.
 * Inspired by amen (norns) and zeptocore/ectocore.
 *
 * Architecture:
 *   1 or 2 WAV loops -> DRAM buffers -> sliced into 4/8/16/32 slices
 *   (equal grid, or transient mode: grid points snap to nearest onset)
 *
 * Triggering:
 *   Select CV in (0-5V -> slice index, or 1V/oct note -> slice index with
 *   an adjustable root note) sampled on Trigger in rising edge
 *   Random in     -> plays a random slice (Free; Beat: groove-preserving;
 *                    Role: picks a slice tagged for the grid position's role)
 *   Clock in      -> steps through slices (forward/reverse/pingpong/walk/
 *                    shuffle, or Pattern: a groove template of role tags)
 *   Reset in      -> stepping back to slice 1
 *   Ratchet in    -> gate held retrigs current slice at a clock subdivision
 *   MIDI notes from C1 (36) play slices directly
 *   MIDI clock    -> tempo + transport-driven stepping (if no analog Clock)
 *
 * Two-loop intermingle:
 *   Blend sets the probability that a slice event draws from loop B
 *   instead of loop A (same slice index, other break).
 *
 * Per-loop trims (Lion / Goat pages):
 *   level, rate (varispeed %), pitch (semitones) and a bipolar filter
 *   (negative = low-pass, positive = high-pass) applied to each voice
 *   before the per-event effect dice, to match unlike breaks.
 *
 * Effects (rolled per slice event, amen style):
 *   each has a 0-100% probability parameter; map it to a fader for
 *   controlled chaos, or map it to a gate (0/100) for manual punch-in.
 *   Reverse, Pitch up, Pitch down, Stutter (sub-loop retrig),
 *   Stretch (grain retrigger timestretch), Gate (tight chop).
 *   The Break macro scales all probabilities (50 = as set).
 *
 * Clock sync:
 *   Filename convention "name_<bpm>.wav" gives the loop tempo; slices
 *   then conform to the measured clock (granular Stretch or Repitch).
 *   The clock is either the analog Clock input or incoming MIDI clock
 *   (24 PPQN); the analog input wins when both are present.
 */

#include <math.h>
#include <string.h>
#include <new>
#include <distingnt/api.h>
#include <distingnt/wav.h>
#include <distingnt/serialisation.h>

// ---------------------------------------------------------------------------
// constants

enum
{
	kNumLoops		= 2,
	kMaxSlices		= 32,
	kEchoFrames		= 48000,	// serpent delay line, 1s stereo
	kWaveBuckets	= 128,		// waveform display resolution
	kAnalysisHop	= 128,		// frames per onset-envelope hop
	kAnalysisChunk	= 8192,		// frames analysed per step() call
	kMinSliceFrames	= 256,
	kEnvRampFrames	= 32,		// declick attack
	kReleaseFrames	= 48,		// declick release, in output frames
};

static const float kTrigHi = 1.0f;		// volts, rising edge threshold
static const float kTrigLo = 0.5f;		// volts, re-arm threshold

// per-slice drum-role tags (3 bits), for pattern-aware playback
enum
{
	kRoleNone = 0,
	kRoleKick,
	kRoleSnare,
	kRolePerc,
	kRoleHat,
	kRoleCrash,
	kNumRoles,
};

static const int kNumBeefSlots = kNumRoles - 1;	// one per role, excluding None
static const int kNumLoadSlots = kNumLoops + kNumBeefSlots;	// loops + beef one-shots share the loader

// display initial per role ('\0' = untagged, nothing drawn)
static const char kRoleInitials[ kNumRoles ] = { 0, 'K', 'S', 'P', 'H', 'C' };

// ---------------------------------------------------------------------------
// (a*b)/c in 64-bit, without __aeabi_uldivmod (firmware doesn't export libgcc
// helpers). Quotient must fit in 32 bits, as it does for frame/pixel scaling.

static uint32_t muldivU32( uint32_t a, uint32_t b, uint32_t c )
{
	uint64_t n = (uint64_t)a * b;
	uint32_t hi = (uint32_t)( n >> 32 );
	uint32_t lo = (uint32_t)n;
	if ( !hi )
		return lo / c;
	uint32_t rem = hi % c;
	uint32_t q = 0;
	for ( int i=31; i>=0; --i )
	{
		uint32_t carry = rem >> 31;
		rem = ( rem << 1 ) | ( ( lo >> i ) & 1 );
		if ( carry || rem >= c )
		{
			rem -= c;
			q |= 1u << i;
		}
	}
	return q;
}

static inline int roundToInt( float x )
{
	return ( x >= 0.0f ) ? (int)( x + 0.5f ) : -(int)( -x + 0.5f );
}

// ---------------------------------------------------------------------------
// small deterministic RNG (xorshift32)

struct Rng
{
	uint32_t s;

	void seed( uint32_t v ) { s = v ? v : 0x9e3779b9u; }

	uint32_t next()
	{
		uint32_t x = s;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		s = x;
		return x;
	}

	// [0,1)
	float uniform() { return ( next() >> 8 ) * ( 1.0f / 16777216.0f ); }
};

// ---------------------------------------------------------------------------
// parameters

enum
{
	kParamOutputL,
	kParamOutputMode,
	kParamOutputR,
	kParamLevel,

	kParamLoops,
	kParamFolder,
	kParamSample,
	kParamFolderB,
	kParamSampleB,
	kParamSlices,
	kParamSliceMode,
	kParamBars,

	kParamSelectInput,
	kParamTrigInput,
	kParamRandomInput,
	kParamClockInput,
	kParamResetInput,
	kParamRatchetInput,

	kParamSync,
	kParamClockDiv,
	kParamStepMode,
	kParamRandomMode,
	kParamRatchetDiv,
	kParamMidiChannel,

	kParamReverse,
	kParamPitchUp,
	kParamPitchDown,
	kParamStutter,
	kParamStretch,
	kParamGate,
	kParamFilter,
	kParamSerpent,
	kParamBlend,
	kParamBlendMode,
	kParamQuarrel,
	kParamBreak,

	kParamPitchAmount,
	kParamStutterDiv,
	kParamStretchAmount,
	kParamCrush,
	kParamDrive,

	kParamSelectMode,
	kParamSelectOffset,

	// per-loop pre-fx trims (appended so older presets still load)
	kParamLionLevel,
	kParamLionRate,
	kParamLionPitch,
	kParamLionFilter,
	kParamGoatLevel,
	kParamGoatRate,
	kParamGoatPitch,
	kParamGoatFilter,

	kParamBackbeat,

	kParamLpg,
	kParamLpgDecay,
	kParamLpgRes,

	kParamPattern,

	// beef: one-shots layered on role-tagged slices, straight (no fx)
	kParamBeefFolder,
	kParamBeefSampleKick,
	kParamBeefLevelKick,
	kParamBeefDuckKick,
	kParamBeefSampleSnare,
	kParamBeefLevelSnare,
	kParamBeefDuckSnare,
	kParamBeefSamplePerc,
	kParamBeefLevelPerc,
	kParamBeefDuckPerc,
	kParamBeefSampleHat,
	kParamBeefLevelHat,
	kParamBeefDuckHat,
	kParamBeefSampleCrash,
	kParamBeefLevelCrash,
	kParamBeefDuckCrash,

	kNumParams,
};

static const char* const loopsStrings[] = { "1", "2" };
static const char* const blendModeStrings[] = { "Chance", "Xfade" };
static const char* const sliceCountStrings[] = { "4", "8", "16", "32" };
static const char* const sliceModeStrings[] = { "Equal", "Transient" };
static const char* const barsStrings[] = { "1", "2" };
static const char* const stutterDivStrings[] = { "1/2", "1/4", "1/8", "1/16", "Random" };
static const char* const syncModeStrings[] = { "Off", "Stretch", "Repitch" };
static const char* const clockDivStrings[] = { "Auto", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar" };

// quarter notes per clock tick, indexed by kParamClockDiv (entry 0 unused: Auto)
static const float clockDivQuarters[] = { 1.0f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };

static const char* const selectModeStrings[] = { "Linear", "1V/Oct" };
static const char* const stepModeStrings[] = { "Forward", "Reverse", "PingPong", "Walk", "Shuffle", "Pattern" };
static const char* const randomModeStrings[] = { "Free", "Beat", "Role" };

// Pattern step mode: classic groove templates mapping the 16 sixteenth-note
// slots of a bar to a drum role. Pattern mode picks a random slice of the
// slot's role (falling back to any slice when that tag pool is empty), so a
// tagged break reassembles into the groove. Blend/Break/dice still apply.
enum { kPatBoomBap, kPatFourFloor, kPatTwoStep, kPatHalfTime, kNumPatterns };
static const char* const patternStrings[] = { "Boom-bap", "4-floor", "Two-step", "Half-time" };
static const uint8_t patternTable[ kNumPatterns ][ 16 ] = {
	// Boom-bap: kick on 1 and the "and" of 3, snare on 2 & 4, hats between
	{ kRoleKick,  kRoleHat, kRoleHat, kRoleHat,   kRoleSnare, kRoleHat, kRoleHat, kRoleHat,
	  kRoleHat,   kRoleHat, kRoleKick, kRoleHat,  kRoleSnare, kRoleHat, kRoleKick, kRoleHat },
	// Four-on-the-floor: kick every quarter, hats on the offbeat eighths
	{ kRoleKick,  kRoleHat, kRoleHat, kRoleHat,   kRoleKick,  kRoleHat, kRoleHat, kRoleHat,
	  kRoleKick,  kRoleHat, kRoleHat, kRoleHat,   kRoleKick,  kRoleHat, kRoleHat, kRoleHat },
	// DnB two-step: kick on 1, snare on 3, sparse ghost kick, hats between
	{ kRoleKick,  kRoleHat, kRoleHat, kRoleHat,   kRoleHat,   kRoleHat, kRoleHat, kRoleHat,
	  kRoleSnare, kRoleHat, kRoleKick, kRoleHat,  kRoleHat,   kRoleHat, kRoleSnare, kRoleHat },
	// Half-time: kick on 1, snare on 3 only, lots of space (perc fills)
	{ kRoleKick,  kRolePerc, kRoleHat, kRolePerc, kRolePerc,  kRolePerc, kRoleHat, kRolePerc,
	  kRoleSnare, kRolePerc, kRoleHat, kRolePerc, kRolePerc,  kRolePerc, kRoleHat, kRolePerc },
};
static const char* const ratchetDivStrings[] = { "1/1", "1/2", "1/3", "1/4", "1/8" };
static const float ratchetDivValues[] = { 1.0f, 2.0f, 3.0f, 4.0f, 8.0f };
static const char* const midiChannelStrings[] = {
	"Omni", "1", "2", "3", "4", "5", "6", "7", "8",
	"9", "10", "11", "12", "13", "14", "15", "16" };

static const _NT_parameter parameters[] = {
	NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output L", 1, 13 )
	NT_PARAMETER_AUDIO_OUTPUT( "Output R", 1, 14 )
	{ .name = "Level", .min = -40, .max = 6, .def = 0, .unit = kNT_unitDb, .scaling = 0, .enumStrings = NULL },

	{ .name = "Loops", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = loopsStrings },
	{ .name = "Lion folder", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitHasStrings, .scaling = 0, .enumStrings = NULL },
	{ .name = "Lion sample", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitConfirm, .scaling = 0, .enumStrings = NULL },
	{ .name = "Goat folder", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitHasStrings, .scaling = 0, .enumStrings = NULL },
	{ .name = "Goat sample", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitConfirm, .scaling = 0, .enumStrings = NULL },
	{ .name = "Slices", .min = 0, .max = 3, .def = 2, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = sliceCountStrings },
	{ .name = "Slice mode", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = sliceModeStrings },
	{ .name = "Bars", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = barsStrings },

	NT_PARAMETER_CV_INPUT( "Select input", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Trig input", 0, 1 )
	NT_PARAMETER_CV_INPUT( "Random input", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Clock input", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Reset input", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Ratchet input", 0, 0 )

	{ .name = "Sync", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = syncModeStrings },
	{ .name = "Clock div", .min = 0, .max = 6, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = clockDivStrings },
	{ .name = "Step mode", .min = 0, .max = 5, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = stepModeStrings },
	{ .name = "Random mode", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = randomModeStrings },
	{ .name = "Ratchet div", .min = 0, .max = 4, .def = 3, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = ratchetDivStrings },
	{ .name = "MIDI channel", .min = 0, .max = 16, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = midiChannelStrings },

	{ .name = "Reverse", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Pitch up", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Pitch down", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Stutter", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Stretch", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Gate", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Filter", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Serpent", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Blend", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Blend mode", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = blendModeStrings },
	{ .name = "Quarrel", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Break", .min = 0, .max = 100, .def = 50, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },

	{ .name = "Pitch amount", .min = 1, .max = 24, .def = 12, .unit = kNT_unitSemitones, .scaling = 0, .enumStrings = NULL },
	{ .name = "Stutter div", .min = 0, .max = 4, .def = 4, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = stutterDivStrings },
	{ .name = "Stretch amount", .min = 110, .max = 400, .def = 200, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Crush", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Drive", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },

	{ .name = "Select mode", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = selectModeStrings },
	{ .name = "Select offset", .min = 0, .max = 127, .def = 12, .unit = kNT_unitMIDINote, .scaling = 0, .enumStrings = NULL },

	{ .name = "Lion level", .min = -40, .max = 6, .def = 0, .unit = kNT_unitDb, .scaling = 0, .enumStrings = NULL },
	{ .name = "Lion rate", .min = 25, .max = 400, .def = 100, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Lion pitch", .min = -24, .max = 24, .def = 0, .unit = kNT_unitSemitones, .scaling = 0, .enumStrings = NULL },
	{ .name = "Lion filter", .min = -100, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Goat level", .min = -40, .max = 6, .def = 0, .unit = kNT_unitDb, .scaling = 0, .enumStrings = NULL },
	{ .name = "Goat rate", .min = 25, .max = 400, .def = 100, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Goat pitch", .min = -24, .max = 24, .def = 0, .unit = kNT_unitSemitones, .scaling = 0, .enumStrings = NULL },
	{ .name = "Goat filter", .min = -100, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },

	{ .name = "Backbeat", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },

	{ .name = "LPG", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "LPG decay", .min = 0, .max = 100, .def = 30, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "LPG res", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },

	{ .name = "Pattern", .min = 0, .max = kNumPatterns - 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = patternStrings },

	{ .name = "Beef folder", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitHasStrings, .scaling = 0, .enumStrings = NULL },
	{ .name = "Kick sample", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitConfirm, .scaling = 0, .enumStrings = NULL },
	{ .name = "Kick level", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Kick duck", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Snare sample", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitConfirm, .scaling = 0, .enumStrings = NULL },
	{ .name = "Snare level", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Snare duck", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Perc sample", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitConfirm, .scaling = 0, .enumStrings = NULL },
	{ .name = "Perc level", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Perc duck", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Hat sample", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitConfirm, .scaling = 0, .enumStrings = NULL },
	{ .name = "Hat level", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Hat duck", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Crash sample", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitConfirm, .scaling = 0, .enumStrings = NULL },
	{ .name = "Crash level", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Crash duck", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
};

static const uint8_t pageSample[] = { kParamLoops, kParamSlices, kParamSliceMode, kParamBars };
static const uint8_t pageLion[] = { kParamFolder, kParamSample, kParamLionLevel, kParamLionRate, kParamLionPitch, kParamLionFilter };
static const uint8_t pageGoat[] = { kParamFolderB, kParamSampleB, kParamGoatLevel, kParamGoatRate, kParamGoatPitch, kParamGoatFilter };
static const uint8_t pageTriggers[] = { kParamSelectInput, kParamSelectMode, kParamSelectOffset, kParamTrigInput, kParamRandomInput, kParamClockInput, kParamResetInput, kParamRatchetInput };
static const uint8_t pageSeq[] = { kParamSync, kParamClockDiv, kParamStepMode, kParamPattern, kParamRandomMode, kParamRatchetDiv, kParamMidiChannel };
static const uint8_t pageFx[] = { kParamReverse, kParamPitchUp, kParamPitchDown, kParamStutter, kParamStretch, kParamGate, kParamFilter, kParamSerpent, kParamBlend, kParamBlendMode, kParamQuarrel, kParamBreak, kParamBackbeat };
static const uint8_t pageFxSetup[] = { kParamPitchAmount, kParamStutterDiv, kParamStretchAmount, kParamCrush, kParamDrive, kParamLpg, kParamLpgDecay, kParamLpgRes };
static const uint8_t pageRouting[] = { kParamOutputL, kParamOutputR, kParamOutputMode, kParamLevel };
static const uint8_t pageBeef[] = {
	kParamBeefFolder,
	kParamBeefSampleKick, kParamBeefLevelKick, kParamBeefDuckKick,
	kParamBeefSampleSnare, kParamBeefLevelSnare, kParamBeefDuckSnare,
	kParamBeefSamplePerc, kParamBeefLevelPerc, kParamBeefDuckPerc,
	kParamBeefSampleHat, kParamBeefLevelHat, kParamBeefDuckHat,
	kParamBeefSampleCrash, kParamBeefLevelCrash, kParamBeefDuckCrash,
};

static const _NT_parameterPage pages[] = {
	{ .name = "Sample", .numParams = ARRAY_SIZE(pageSample), .params = pageSample },
	{ .name = "Lion", .numParams = ARRAY_SIZE(pageLion), .params = pageLion },
	{ .name = "Goat", .numParams = ARRAY_SIZE(pageGoat), .params = pageGoat },
	{ .name = "Triggers", .numParams = ARRAY_SIZE(pageTriggers), .params = pageTriggers },
	{ .name = "Sequence", .numParams = ARRAY_SIZE(pageSeq), .params = pageSeq },
	{ .name = "FX", .numParams = ARRAY_SIZE(pageFx), .params = pageFx },
	{ .name = "FX setup", .numParams = ARRAY_SIZE(pageFxSetup), .params = pageFxSetup },
	{ .name = "Beef", .numParams = ARRAY_SIZE(pageBeef), .params = pageBeef },
	{ .name = "Routing", .numParams = ARRAY_SIZE(pageRouting), .params = pageRouting },
};

static const _NT_parameterPages parameterPages = {
	.numPages = ARRAY_SIZE(pages),
	.pages = pages,
};

// per-loop folder/sample parameter indices
static const uint8_t loopFolderParam[ kNumLoops ] = { kParamFolder, kParamFolderB };
static const uint8_t loopSampleParam[ kNumLoops ] = { kParamSample, kParamSampleB };

// beef slot index (0..kNumBeefSlots-1) <-> role (kRoleKick..kRoleCrash) is slot+1.
// all slots share one folder; each has its own sample/level/duck.
static const uint8_t beefSampleParam[ kNumBeefSlots ] = {
	kParamBeefSampleKick, kParamBeefSampleSnare, kParamBeefSamplePerc, kParamBeefSampleHat, kParamBeefSampleCrash };
static const uint8_t beefLevelParam[ kNumBeefSlots ] = {
	kParamBeefLevelKick, kParamBeefLevelSnare, kParamBeefLevelPerc, kParamBeefLevelHat, kParamBeefLevelCrash };
static const uint8_t beefDuckParam[ kNumBeefSlots ] = {
	kParamBeefDuckKick, kParamBeefDuckSnare, kParamBeefDuckPerc, kParamBeefDuckHat, kParamBeefDuckCrash };

// ---------------------------------------------------------------------------
// specifications

static const _NT_specification specifications[] = {
	{ .name = "Max length", .min = 1, .max = 32, .def = 8, .type = kNT_typeSeconds },
	{ .name = "Beef length", .min = 1, .max = 8, .def = 2, .type = kNT_typeSeconds },
};

// ---------------------------------------------------------------------------
// voice

struct Voice
{
	uint8_t		active;
	uint8_t		stutter;
	uint8_t		stretch;
	uint8_t		serpent;		// send this voice into the echo tail
	int8_t		sliceIdx;
	uint8_t		loopIdx;

	float		dir;			// +1 / -1
	float		rate;			// source frames per output frame

	const float* buf;			// source loop buffer (interleaved stereo)
	uint32_t	bufFrames;

	float		pos;			// plain/stutter playback position (frames)
	uint32_t	start, end;		// slice bounds (frames)
	uint32_t	loopLen;		// stutter sub-loop length (frames)

	float		grainStart;		// stretch: grain source start
	float		grainPhase;		// stretch: position within grain
	float		grainLen;		// stretch: grain length (frames)
	float		grainAdv;		// stretch: source advance per grain (< grainLen)

	float		framesLeft;		// output frames until natural end
	float		env;			// declick envelope
	float		envTarget;

	// per-event filter sweep (Chamberlin SVF)
	uint8_t		fType;			// 0 none, 1 LP, 2 HP
	float		fCoef;			// current frequency coefficient
	float		fCoefStep;		// per-frame sweep
	float		svLowL, svBandL, svLowR, svBandR;

	// per-slice low-pass gate (vactrol-style), a second SVF stage struck on
	// the slice event: env drives cutoff and (skewed) output amplitude together
	uint8_t		lpgActive;
	float		lpgEnv;			// 1.0 at strike, decays toward 0
	float		lpgDecayCoef;	// per-frame multiplier for the decay
	float		lpgDepth;		// 0..1 dry/gated blend
	float		lpgDamp;		// SVF damping from resonance
	float		lpgCoefMin;		// cutoff coef at env 0 (closed)
	float		lpgCoefMax;		// cutoff coef at env 1 (open)
	float		lpgLowL, lpgBandL, lpgLowR, lpgBandR;
};

static const float kFilterDamp = 0.5f;	// SVF damping (some resonance)

// ---------------------------------------------------------------------------
// per-loop state

struct Loop
{
	// buffers, fixed at construct
	float*		sample;			// DRAM: interleaved stereo floats
	float*		onset;			// DRAM: onset strength per hop
	float*		hopPeak;		// DRAM: peak level per hop (zoom mipmap)

	// load state
	bool		loaded;
	bool		sliced;
	bool		analysed;
	uint32_t	numFrames;
	uint32_t	numHops;
	float		srRatio;		// file rate / host rate
	float		fileBpm;		// parsed from filename "_<bpm>.wav", 0 = unknown

	// analysis progress (amortised over step() calls)
	uint32_t	analysisPos;
	float		prevHopEnergy;

	// slices
	uint32_t	sliceStart[ kMaxSlices + 1 ];
	int			numSlices;
	bool		manualSlices;	// user-moved points; auto re-slice won't clobber
	uint32_t	lockMask;		// locked slice always plays straight
	uint8_t		sliceRole[ kMaxSlices ];	// drum-role tag per slice (kRole*)

	// manual points restored from a preset, waiting for the sample to load
	bool		havePending;
	int			pendingNumSlices;
	uint32_t	pendingPoints[ kMaxSlices ];

	// display
	float		wave[ kWaveBuckets ];
	float		waveMax;
};

// one Beef layer: a one-shot sample layered on top of a drum-role tag,
// played straight (no fx) with its own level and an original-slice duck
struct OneShot
{
	float*		sample;		// DRAM: interleaved stereo floats
	bool		loaded;
	uint32_t	numFrames;
	float		srRatio;	// file rate / host rate
};

// ---------------------------------------------------------------------------
// algorithm

struct _breakSlicer : public _NT_algorithm
{
	_breakSlicer() {}
	~_breakSlicer() {}

	_NT_parameter	params[ kNumParams ];

	Loop			loops[ kNumLoops ];
	uint32_t		capFrames;			// per-loop buffer capacity in frames

	OneShot			beefSample[ kNumBeefSlots ];	// one per role: Kick, Snare, Perc, Hat, Crash
	uint32_t		beefCapFrames;					// per-one-shot buffer capacity in frames

	// sample loading (one read at a time). slots 0..kNumLoops-1 are the
	// loops, kNumLoops..kNumLoops+kNumBeefSlots-1 are the beef one-shots
	_NT_wavRequest	request;
	bool			cardMounted;
	bool			awaitingCallback;
	int				loadingSlot;
	bool			queuedLoad[ kNumLoadSlots ];

	// triggering
	bool			trigArmed, randArmed, clockArmed, resetArmed;
	int				seqStep;
	int				stepPhase;			// rhythmic grid position since Reset (for Backbeat)
	int				roleRandomPhase;	// groove-slot scan for Role random mode
	int				lastSlice;			// most recently triggered slice
	int				ppDir;				// ping-pong direction
	int				shufflePos;
	int				permN;				// slice count the permutation was built for
	uint8_t			perm[ kMaxSlices ];

	// ratchet
	bool			ratchetHigh;
	float			ratchetTimer;		// output frames until next retrig

	// crush (SP-1200 style decimator)
	float			crushQ;				// quantisation levels
	float			crushDiv;			// output frames per held sample
	float			crushPhase;
	float			crushL, crushR;

	// drive (soft-clip saturation)
	float			drivePre;			// input gain into the shaper
	float			driveMakeup;

	// low-pass gate (per-slice, precomputed from the LPG params)
	float			lpgDepth;			// 0..1, 0 = off
	float			lpgDamp;			// SVF damping from LPG res
	float			lpgCoefMin;			// cutoff coef, gate closed
	float			lpgCoefMax;			// cutoff coef, gate open
	float			lpgDecayFrac;		// 0..1 -> short..full-slice decay

	// serpent (dub delay tail)
	float*			echo;				// DRAM: stereo interleaved ring buffer
	uint32_t		echoPos;
	float			echoTime;			// delay in frames, slewed
	float			echoLpL, echoLpR;	// darkening filter in the feedback path

	// tame (button 1 held: beast behaves)
	bool			tamed;

	// quarrel (auto random-walk on Blend)
	float			quarrelWalk;		// wander offset, -50..+50

	// clock measurement (for Sync)
	float			clockPeriod;		// output frames per clock tick, 0 = unknown
	uint32_t		framesSinceClock;

	// MIDI clock (24 PPQN) as an alternative to the analog Clock input
	uint32_t		frameClock;			// free-running per-frame counter
	uint32_t		midiTickCount;		// ticks toward the next quarter measurement
	uint32_t		midiLastQuarterFrame;	// frameClock at the last quarter boundary
	uint32_t		midiStepTicks;		// ticks toward the next sequence step
	bool			midiRunning;		// transport state (Start/Continue vs Stop)

	// slice editor
	bool			editMode;
	int				editLoop;
	int				selPoint;			// selected slice point, 0..numSlices-1
	float			zoomPot;			// 0..1 -> 1x..256x
	uint8_t			lastControl;		// most recently used editor control, for the legend highlight

	// cached parameter values
	float			gain, gainTarget;
	float			pitchUpFactor, pitchDownFactor;

	// per-loop pre-fx trims
	float			loopGain[ kNumLoops ], loopGainTarget[ kNumLoops ];
	float			loopSpeed[ kNumLoops ];		// rate param, as a factor
	float			loopTune[ kNumLoops ];		// pitch param, as a factor
	uint8_t			loopFType[ kNumLoops ];		// 0 none, 1 LP, 2 HP
	float			loopFCoef[ kNumLoops ];		// SVF frequency coefficient

	Voice			cur, fade;		// primary layer (chance mode; loop A in xfade)
	Voice			curB, fadeB;	// second layer, used only in xfade blend mode
	float			xfA, xfB;		// smoothed crossfade amplitudes

	// beef: a one-shot layered on top of a role-tagged slice, straight (no fx)
	Voice			beefVoice, beefFadeVoice;
	float			beefAmp, beefFadeAmp;		// Level% captured at trigger time
	float			beefDuck;					// Duck% for the current beefVoice's role

	Rng				rng;
};

// the slice count used for sequencing / CV / MIDI addressing (loop A is master)
static int canonicalSlices( _breakSlicer* pThis )
{
	if ( pThis->loops[0].sliced )
		return pThis->loops[0].numSlices;
	if ( pThis->loops[1].sliced )
		return pThis->loops[1].numSlices;
	return 0;
}

// ---------------------------------------------------------------------------
// slicing

static void computeSlices( _breakSlicer* pThis, Loop& L )
{
	int n = 4 << pThis->v[ kParamSlices ];

	uint32_t total = L.numFrames;
	while ( n > 1 && total < (uint32_t)( n * kMinSliceFrames ) )
		n >>= 1;		// sample too short for this many slices
	L.numSlices = n;

	bool transient = pThis->v[ kParamSliceMode ] && L.analysed;

	L.sliceStart[0] = 0;
	L.sliceStart[n] = total;

	for ( int i=1; i<n; ++i )
	{
		uint32_t grid = muldivU32( i, total, n );

		if ( transient )
		{
			// snap grid point to the strongest onset within +/- 40% of a slice
			uint32_t gridHop = grid / kAnalysisHop;
			uint32_t range = ( total / n ) * 2 / 5 / kAnalysisHop;
			uint32_t lo = ( gridHop > range ) ? gridHop - range : 0;
			uint32_t hi = gridHop + range;
			if ( hi >= L.numHops )
				hi = L.numHops - 1;
			uint32_t best = gridHop;
			float bestV = -1.0f;
			for ( uint32_t k=lo; k<=hi; ++k )
			{
				if ( L.onset[k] > bestV )
				{
					bestV = L.onset[k];
					best = k;
				}
			}
			grid = best * kAnalysisHop;
		}

		// enforce monotonic, minimum slice length
		uint32_t minStart = L.sliceStart[i-1] + kMinSliceFrames;
		if ( grid < minStart )
			grid = minStart;
		if ( grid > total )
			grid = total;
		L.sliceStart[i] = grid;
	}

	L.sliced = ( total > 0 );

	if ( pThis->selPoint >= L.numSlices )
		pThis->selPoint = L.numSlices - 1;
	if ( pThis->selPoint < 0 )
		pThis->selPoint = 0;
}

// apply slice points restored from a preset, once the sample is loaded
static void applyPendingPoints( _breakSlicer* pThis, Loop& L )
{
	(void)pThis;
	if ( !L.havePending || !L.sliced )
		return;
	if ( L.pendingNumSlices != L.numSlices )
	{
		L.havePending = false;
		return;
	}

	uint32_t total = L.numFrames;
	uint32_t prev = 0;
	for ( int i=1; i<L.numSlices; ++i )
	{
		uint32_t pt = L.pendingPoints[ i-1 ];
		uint32_t minStart = prev + kMinSliceFrames;
		if ( pt < minStart )
			pt = minStart;
		if ( pt > total )
			pt = total;
		L.sliceStart[ i ] = pt;
		prev = pt;
	}
	L.manualSlices = true;
	L.havePending = false;
}

// ---------------------------------------------------------------------------
// construction

void	calculateRequirements( _NT_algorithmRequirements& req, const int32_t* specifications )
{
	uint32_t capFrames = (uint32_t)specifications[0] * 48000;
	uint32_t numHops = capFrames / kAnalysisHop + 2;
	uint32_t beefCapFrames = (uint32_t)specifications[1] * 48000;

	req.numParameters = kNumParams;
	req.sram = sizeof(_breakSlicer);
	req.dram = kNumLoops * ( capFrames * 2 * sizeof(float) + 2 * numHops * sizeof(float) )
		+ kEchoFrames * 2 * sizeof(float)
		+ kNumBeefSlots * beefCapFrames * 2 * sizeof(float);
	req.dtc = 0;
	req.itc = 0;
}

static void wavCallback( void* callbackData, bool success )
{
	_breakSlicer* pThis = (_breakSlicer*)callbackData;
	pThis->awaitingCallback = false;
	int slot = pThis->loadingSlot;
	if ( success && slot >= kNumLoops )
	{
		OneShot& os = pThis->beefSample[ slot - kNumLoops ];
		os.loaded = true;
		return;
	}
	if ( success )
	{
		Loop& L = pThis->loops[ slot ];
		L.loaded = true;
		L.analysisPos = 0;
		L.prevHopEnergy = 0.0f;
		L.analysed = false;
		L.waveMax = 0.0f;
		memset( L.wave, 0, sizeof(L.wave) );
		computeSlices( pThis, L );	// equal grid immediately; transient re-snaps after analysis
		applyPendingPoints( pThis, L );
	}
	// queued loads are kicked from step()
}

_NT_algorithm*	construct( const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements& req, const int32_t* specifications )
{
	static_assert( kNumParams == ARRAY_SIZE(parameters), "parameter count mismatch" );
	(void)req;

	memset( ptrs.sram, 0, sizeof(_breakSlicer) );
	_breakSlicer* alg = new (ptrs.sram) _breakSlicer();

	uint32_t capFrames = (uint32_t)specifications[0] * 48000;
	uint32_t maxHops = capFrames / kAnalysisHop + 2;
	alg->capFrames = capFrames;

	float* dram = (float*)ptrs.dram;
	for ( int li=0; li<kNumLoops; ++li )
	{
		Loop& L = alg->loops[li];
		L.sample = dram;
		L.onset = L.sample + capFrames * 2;
		L.hopPeak = L.onset + maxHops;
		dram = L.hopPeak + maxHops;
		L.srRatio = 1.0f;
	}
	uint32_t beefCapFrames = (uint32_t)specifications[1] * 48000;
	alg->beefCapFrames = beefCapFrames;
	for ( int bi=0; bi<kNumBeefSlots; ++bi )
	{
		OneShot& os = alg->beefSample[bi];
		os.sample = dram;
		dram += beefCapFrames * 2;
		os.srRatio = 1.0f;
	}

	alg->echo = dram;
	memset( alg->echo, 0, kEchoFrames * 2 * sizeof(float) );
	alg->echoTime = 18000.0f;	// 375ms until a clock teaches us better

	memcpy( alg->params, parameters, sizeof parameters );
	alg->parameters = alg->params;
	alg->parameterPages = &parameterPages;

	alg->request.callback = wavCallback;
	alg->request.callbackData = alg;
	alg->request.bits = kNT_WavBits32;
	alg->request.channels = kNT_WavStereo;
	alg->request.progress = kNT_WavProgress;
	alg->request.startOffset = 0;

	alg->gain = alg->gainTarget = 1.0f;
	alg->pitchUpFactor = 2.0f;
	alg->pitchDownFactor = 0.5f;
	for ( int li=0; li<kNumLoops; ++li )
	{
		alg->loopGain[li] = alg->loopGainTarget[li] = 1.0f;
		alg->loopSpeed[li] = 1.0f;
		alg->loopTune[li] = 1.0f;
	}
	alg->trigArmed = alg->randArmed = alg->clockArmed = alg->resetArmed = true;
	alg->ppDir = 1;
	alg->rng.seed( 0xBEA7BEA7u );

	return alg;
}

// ---------------------------------------------------------------------------
// parameters

// Parse a BPM from a filename ending "_<bpm>.wav" (fraction allowed, e.g. "_87.5").
// Returns 0 if absent or implausible.
static float parseBpmFromName( const char* name )
{
	if ( !name )
		return 0.0f;

	const char* underscore = NULL;
	for ( const char* p = name; *p; ++p )
		if ( *p == '_' )
			underscore = p;
	if ( !underscore )
		return 0.0f;

	const char* p = underscore + 1;
	if ( *p < '0' || *p > '9' )
		return 0.0f;

	float bpm = 0.0f;
	while ( *p >= '0' && *p <= '9' )
		bpm = bpm * 10.0f + ( *p++ - '0' );

	// fractional part only if a digit follows the dot (don't eat ".wav")
	if ( *p == '.' && p[1] >= '0' && p[1] <= '9' )
	{
		++p;
		float scale = 0.1f;
		while ( *p >= '0' && *p <= '9' )
		{
			bpm += ( *p++ - '0' ) * scale;
			scale *= 0.1f;
		}
	}

	// rest must be end of string or an extension
	if ( *p != 0 && *p != '.' )
		return 0.0f;

	if ( bpm < 30.0f || bpm > 300.0f )
		return 0.0f;
	return bpm;
}

static void startLoadOneShot( _breakSlicer* pThis, int slot )
{
	int bi = slot - kNumLoops;
	OneShot& os = pThis->beefSample[ bi ];

	// sample param 0 = "None": stay unloaded, don't touch DRAM/the loader
	int sampleIdx = pThis->v[ beefSampleParam[bi] ] - 1;
	if ( sampleIdx < 0 )
	{
		os.loaded = false;
		if ( pThis->beefVoice.active && pThis->beefVoice.loopIdx == bi )
			pThis->beefVoice.active = 0;
		if ( pThis->beefFadeVoice.active && pThis->beefFadeVoice.loopIdx == bi )
			pThis->beefFadeVoice.active = 0;
		return;
	}

	_NT_wavInfo info;
	NT_getSampleFileInfo( pThis->v[ kParamBeefFolder ], sampleIdx, info );
	if ( !info.name || !info.numFrames )
		return;

	os.loaded = false;
	if ( pThis->beefVoice.active && pThis->beefVoice.loopIdx == bi )
		pThis->beefVoice.active = 0;
	if ( pThis->beefFadeVoice.active && pThis->beefFadeVoice.loopIdx == bi )
		pThis->beefFadeVoice.active = 0;

	os.numFrames = ( info.numFrames < pThis->beefCapFrames ) ? info.numFrames : pThis->beefCapFrames;
	os.srRatio = info.sampleRate / (float)NT_globals.sampleRate;

	pThis->request.folder = pThis->v[ kParamBeefFolder ];
	pThis->request.sample = sampleIdx;
	pThis->request.numFrames = os.numFrames;
	pThis->request.dst = os.sample;

	pThis->loadingSlot = slot;
	if ( NT_readSampleFrames( pThis->request ) )
		pThis->awaitingCallback = true;
}

static void startLoad( _breakSlicer* pThis, int slot )
{
	if ( slot >= kNumLoops )
	{
		startLoadOneShot( pThis, slot );
		return;
	}

	int li = slot;
	Loop& L = pThis->loops[ li ];

	_NT_wavInfo info;
	NT_getSampleFileInfo( pThis->v[ loopFolderParam[li] ], pThis->v[ loopSampleParam[li] ], info );
	if ( !info.name || !info.numFrames )
		return;

	L.loaded = false;
	L.sliced = false;
	L.analysed = false;
	if ( pThis->cur.active && pThis->cur.loopIdx == li )
		pThis->cur.active = 0;
	if ( pThis->fade.active && pThis->fade.loopIdx == li )
		pThis->fade.active = 0;
	if ( pThis->curB.active && pThis->curB.loopIdx == li )
		pThis->curB.active = 0;
	if ( pThis->fadeB.active && pThis->fadeB.loopIdx == li )
		pThis->fadeB.active = 0;

	L.numFrames = ( info.numFrames < pThis->capFrames ) ? info.numFrames : pThis->capFrames;
	L.numHops = L.numFrames / kAnalysisHop;
	L.srRatio = info.sampleRate / (float)NT_globals.sampleRate;
	L.fileBpm = parseBpmFromName( info.name );

	pThis->request.folder = pThis->v[ loopFolderParam[li] ];
	pThis->request.sample = pThis->v[ loopSampleParam[li] ];
	pThis->request.numFrames = L.numFrames;
	pThis->request.dst = L.sample;

	pThis->loadingSlot = li;
	if ( NT_readSampleFrames( pThis->request ) )
		pThis->awaitingCallback = true;
}

static void requestLoad( _breakSlicer* pThis, int slot )
{
	if ( pThis->awaitingCallback )
		pThis->queuedLoad[ slot ] = true;
	else
		startLoad( pThis, slot );
}

int 	parameterString( _NT_algorithm* self, int p, int v, char* buff )
{
	_breakSlicer* pThis = (_breakSlicer*)self;
	int len = 0;

	switch ( p )
	{
	case kParamFolder:
	case kParamFolderB:
	{
		_NT_wavFolderInfo folderInfo;
		NT_getSampleFolderInfo( v, folderInfo );
		if ( folderInfo.name )
		{
			strncpy( buff, folderInfo.name, kNT_parameterStringSize-1 );
			buff[ kNT_parameterStringSize-1 ] = 0;
			len = strlen( buff );
		}
	}
		break;
	case kParamSample:
	case kParamSampleB:
	{
		int folderParam = ( p == kParamSample ) ? kParamFolder : kParamFolderB;
		_NT_wavInfo info;
		NT_getSampleFileInfo( pThis->v[ folderParam ], v, info );
		if ( info.name )
		{
			strncpy( buff, info.name, kNT_parameterStringSize-1 );
			buff[ kNT_parameterStringSize-1 ] = 0;
			len = strlen( buff );
		}
	}
		break;
	case kParamBeefFolder:
	{
		_NT_wavFolderInfo folderInfo;
		NT_getSampleFolderInfo( v, folderInfo );
		if ( folderInfo.name )
		{
			strncpy( buff, folderInfo.name, kNT_parameterStringSize-1 );
			buff[ kNT_parameterStringSize-1 ] = 0;
			len = strlen( buff );
		}
	}
		break;
	case kParamBeefSampleKick:
	case kParamBeefSampleSnare:
	case kParamBeefSamplePerc:
	case kParamBeefSampleHat:
	case kParamBeefSampleCrash:
	{
		if ( v == 0 )
		{
			strncpy( buff, "None", kNT_parameterStringSize-1 );
			buff[ kNT_parameterStringSize-1 ] = 0;
			len = strlen( buff );
			break;
		}
		_NT_wavInfo info;
		NT_getSampleFileInfo( pThis->v[ kParamBeefFolder ], v - 1, info );
		if ( info.name )
		{
			strncpy( buff, info.name, kNT_parameterStringSize-1 );
			buff[ kNT_parameterStringSize-1 ] = 0;
			len = strlen( buff );
		}
	}
		break;
	}

	return len;
}

static void updateGrayedOut( _breakSlicer* pThis )
{
	int algIdx = NT_algorithmIndex( pThis );
	if ( algIdx < 0 )
		return;
	bool gray = ( pThis->v[ kParamLoops ] == 0 );
	uint32_t off = NT_parameterOffset();
	NT_setParameterGrayedOut( algIdx, kParamFolderB + off, gray );
	NT_setParameterGrayedOut( algIdx, kParamSampleB + off, gray );
	NT_setParameterGrayedOut( algIdx, kParamGoatLevel + off, gray );
	NT_setParameterGrayedOut( algIdx, kParamGoatRate + off, gray );
	NT_setParameterGrayedOut( algIdx, kParamGoatPitch + off, gray );
	NT_setParameterGrayedOut( algIdx, kParamGoatFilter + off, gray );
	NT_setParameterGrayedOut( algIdx, kParamBlend + off, gray );
	NT_setParameterGrayedOut( algIdx, kParamBlendMode + off, gray );
	NT_setParameterGrayedOut( algIdx, kParamQuarrel + off, gray );
	NT_setParameterGrayedOut( algIdx, kParamSelectOffset + off, pThis->v[ kParamSelectMode ] == 0 );
	NT_setParameterGrayedOut( algIdx, kParamPattern + off, pThis->v[ kParamStepMode ] != 5 );
}

// recompute the low-pass gate coefficients from the three LPG params
static void updateLpg( _breakSlicer* pThis )
{
	pThis->lpgDepth = pThis->v[ kParamLpg ] / 100.0f;

	// resonance lowers the SVF damping; the floor keeps hard settings ringing
	// hard (pinged-filter kicks) without the state exploding
	float res = pThis->v[ kParamLpgRes ] / 100.0f;
	pThis->lpgDamp = 1.4f - 1.3f * res;			// 1.4 (none) .. 0.1 (pinged)

	// cutoff sweep: gate closed ~120Hz, open ~9kHz (coef clamped for stability)
	float k = 6.2831853f / (float)NT_globals.sampleRate;
	pThis->lpgCoefMin = 2.0f * sinf( 0.5f * k * 120.0f );
	float cMax = 2.0f * sinf( 0.5f * k * 9000.0f );
	pThis->lpgCoefMax = ( cMax > 1.0f ) ? 1.0f : cMax;

	pThis->lpgDecayFrac = pThis->v[ kParamLpgDecay ] / 100.0f;
}

void	parameterChanged( _NT_algorithm* self, int p )
{
	_breakSlicer* pThis = (_breakSlicer*)self;

	switch ( p )
	{
	case kParamLpg:
	case kParamLpgDecay:
	case kParamLpgRes:
		updateLpg( pThis );
		break;
	case kParamLoops:
		updateGrayedOut( pThis );
		if ( pThis->v[ kParamLoops ] && !pThis->loops[1].loaded )
			requestLoad( pThis, 1 );
		if ( !pThis->v[ kParamLoops ] )
			pThis->editLoop = 0;
		break;
	case kParamSelectMode:
	case kParamStepMode:
		updateGrayedOut( pThis );
		break;
	case kParamFolder:
	case kParamFolderB:
	{
		int sampleParam = ( p == kParamFolder ) ? kParamSample : kParamSampleB;
		_NT_wavFolderInfo folderInfo;
		NT_getSampleFolderInfo( pThis->v[ p ], folderInfo );
		pThis->params[ sampleParam ].max = folderInfo.numSampleFiles ? folderInfo.numSampleFiles - 1 : 0;
		NT_updateParameterDefinition( NT_algorithmIndex( self ), sampleParam );
		// folder change alone never fires the sample param, so kick the
		// load of the (now re-clamped) current index here
		int li = ( p == kParamFolder ) ? 0 : 1;
		if ( li == 0 || pThis->v[ kParamLoops ] )
			requestLoad( pThis, li );
	}
		break;
	case kParamSample:
		requestLoad( pThis, 0 );
		break;
	case kParamSampleB:
		if ( pThis->v[ kParamLoops ] )
			requestLoad( pThis, 1 );
		break;
	case kParamBeefFolder:
	{
		_NT_wavFolderInfo folderInfo;
		NT_getSampleFolderInfo( pThis->v[ kParamBeefFolder ], folderInfo );
		int maxIdx = folderInfo.numSampleFiles;	// 0 = None, 1..N = files
		for ( int bi=0; bi<kNumBeefSlots; ++bi )
		{
			pThis->params[ beefSampleParam[bi] ].max = maxIdx;
			NT_updateParameterDefinition( NT_algorithmIndex( self ), beefSampleParam[bi] );
			requestLoad( pThis, kNumLoops + bi );
		}
	}
		break;
	case kParamBeefSampleKick:
	case kParamBeefSampleSnare:
	case kParamBeefSamplePerc:
	case kParamBeefSampleHat:
	case kParamBeefSampleCrash:
	{
		int bi = 0;
		while ( beefSampleParam[bi] != p )
			++bi;
		requestLoad( pThis, kNumLoops + bi );
	}
		break;
	case kParamSlices:
	case kParamSliceMode:
		for ( int li=0; li<kNumLoops; ++li )
		{
			Loop& L = pThis->loops[li];
			L.manualSlices = false;		// explicit re-slice discards manual edits
			if ( L.loaded )
				computeSlices( pThis, L );
		}
		break;
	case kParamLevel:
		pThis->gainTarget = powf( 10.0f, pThis->v[ kParamLevel ] / 20.0f );
		break;
	case kParamPitchAmount:
	{
		float semis = (float)pThis->v[ kParamPitchAmount ];
		pThis->pitchUpFactor = powf( 2.0f, semis / 12.0f );
		pThis->pitchDownFactor = 1.0f / pThis->pitchUpFactor;
	}
		break;
	case kParamCrush:
	{
		// calibrated so Crush = 50 is an SP-1200: 12 bits at 26.04kHz
		float c = (float)pThis->v[ kParamCrush ];
		pThis->crushQ = exp2f( 16.0f - c * 0.08f );		// 16 -> 12 (at 50) -> 8 bits
		pThis->crushDiv = exp2f( c * 0.017644f );		// 48k -> 26.04k (at 50) -> 14.1k
	}
		break;
	case kParamDrive:
	{
		float d = (float)pThis->v[ kParamDrive ];
		pThis->drivePre = 1.0f + d * 0.15f;				// 1x .. 16x into the shaper
		pThis->driveMakeup = 1.0f / sqrtf( pThis->drivePre );
	}
		break;
	case kParamLionLevel:
	case kParamGoatLevel:
	{
		int li = ( p == kParamGoatLevel );
		pThis->loopGainTarget[li] = powf( 10.0f, pThis->v[p] / 20.0f );
	}
		break;
	case kParamLionRate:
	case kParamGoatRate:
	{
		int li = ( p == kParamGoatRate );
		pThis->loopSpeed[li] = pThis->v[p] / 100.0f;
	}
		break;
	case kParamLionPitch:
	case kParamGoatPitch:
	{
		int li = ( p == kParamGoatPitch );
		pThis->loopTune[li] = powf( 2.0f, pThis->v[p] / 12.0f );
	}
		break;
	case kParamLionFilter:
	case kParamGoatFilter:
	{
		int li = ( p == kParamGoatFilter );
		int fv = pThis->v[p];
		if ( !fv )
			pThis->loopFType[li] = 0;
		else
		{
			// negative: LP 18kHz -> 150Hz; positive: HP 20Hz -> 6kHz (log)
			float t = ( ( fv < 0 ) ? -fv : fv ) / 100.0f;
			float hz = ( fv < 0 ) ? 18000.0f * expf( -4.7875f * t )
								  : 20.0f * expf( 5.7038f * t );
			pThis->loopFType[li] = ( fv < 0 ) ? 1 : 2;
			float k = 6.2831853f / (float)NT_globals.sampleRate;
			pThis->loopFCoef[li] = 2.0f * sinf( 0.5f * k * hz );
		}
	}
		break;
	}
}

// cheap tanh-shaped soft clip (Pade approximation, hard limit past +/-3)
static inline float softClip( float x )
{
	if ( x > 3.0f ) return 1.0f;
	if ( x < -3.0f ) return -1.0f;
	float x2 = x * x;
	return x * ( 27.0f + x2 ) / ( 27.0f + 9.0f * x2 );
}

// ---------------------------------------------------------------------------
// triggering

// one per-event roll of all the effect dice, shared by both xfade layers
struct FxRolls
{
	bool	rev, up, down, stut, stretch, gatefx, filt, serp;
	int		stutterDiv;
	int		filtType;			// 0 LP, 1 HP
	float	filtC0, filtC1;		// SVF coefficient sweep endpoints
};

// Backbeat weight for the effect dice, applied on top of the Break macro.
// gridPos is the rhythmic grid position (0-based slice) the event lands on.
// At Backbeat 0 every event weighs 1.0 (unchanged). As it rises, downbeats
// (odd beats: 1 & 3 in 4/4) are attenuated toward zero, so at 100% the dice
// only roll on backbeats (even beats: 2 & 4) and downbeats always play straight.
static float backbeatWeight( _breakSlicer* pThis, int gridPos )
{
	int bb = pThis->v[ kParamBackbeat ];
	if ( bb <= 0 )
		return 1.0f;

	int n = canonicalSlices( pThis );
	if ( n < 1 )
		return 1.0f;
	if ( gridPos < 0 )
		gridPos = 0;
	if ( gridPos >= n )
		gridPos = n - 1;							// CV can address one past the end
	int beats = ( pThis->v[ kParamBars ] + 1 ) * 4;		// 4/4 assumed
	// proportional so sparse grids (e.g. 4 slices over 2 bars) land on the
	// right beats, not just the first few
	int beat = ( gridPos * beats ) / n;					// 0-based beat in the bar(s)
	bool backbeat = ( beat & 1 );						// beats 2 & 4 (0-based 1,3)
	if ( backbeat )
		return 1.0f;
	return 1.0f - bb / 100.0f;							// suppress downbeats
}

// weight scales all dice equally (Serpent included) so the whole beast leans
// on the backbeat, not just one effect.
static void rollFx( _breakSlicer* pThis, FxRolls& r, float weight )
{
	const int16_t* pv = pThis->v;

	// Break macro scales all probabilities: 50 = as set, 0 = all off, 100 = doubled.
	// A held Tame button silences all the dice. Backbeat weight biases per event.
	float scale = pThis->tamed ? 0.0f : ( pv[ kParamBreak ] / 50.0f ) * weight;
	r.rev     = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamReverse ] * scale;
	r.up      = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamPitchUp ] * scale;
	r.down    = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamPitchDown ] * scale;
	r.stut    = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamStutter ] * scale;
	r.stretch = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamStretch ] * scale;
	r.gatefx  = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamGate ] * scale;

	r.filt    = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamFilter ] * scale;
	r.serp    = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamSerpent ] * scale;

	int dv = pv[ kParamStutterDiv ];
	if ( dv >= 4 )
		r.stutterDiv = 4 << ( pThis->rng.next() % 3 );	// random: 4, 8 or 16
	else
		r.stutterDiv = 2 << dv;							// 2, 4, 8, 16

	if ( r.filt )
	{
		// random LP or HP with a random log-spaced cutoff sweep, 200Hz..8kHz
		r.filtType = pThis->rng.next() & 1;
		float f0 = 200.0f * expf( pThis->rng.uniform() * 3.6889f );
		float f1 = 200.0f * expf( pThis->rng.uniform() * 3.6889f );
		float k = 6.2831853f / (float)NT_globals.sampleRate;
		r.filtC0 = 2.0f * sinf( 0.5f * k * f0 );
		r.filtC1 = 2.0f * sinf( 0.5f * k * f1 );
	}
}

static void startVoice( _breakSlicer* pThis, Voice& voice, Voice& fadeSlot, Loop* lp, int idx, const FxRolls& rolls )
{
	const int16_t* pv = pThis->v;

	if ( !lp->sliced )
		return;

	int n = lp->numSlices;
	if ( idx < 0 ) idx = 0;
	if ( idx >= n ) idx = n - 1;

	uint32_t start = lp->sliceStart[ idx ];
	uint32_t end = lp->sliceStart[ idx + 1 ];
	if ( end <= start + 64 )
		return;
	uint32_t len = end - start;

	// choke: current voice moves to the fade slot
	if ( voice.active )
	{
		fadeSlot = voice;
		fadeSlot.envTarget = 0.0f;
	}

	Voice& v = voice;
	memset( &v, 0, sizeof(Voice) );
	int li = (int)( lp - pThis->loops );
	v.active = 1;
	v.sliceIdx = (int8_t)idx;
	v.loopIdx = (uint8_t)li;
	v.buf = lp->sample;
	v.bufFrames = lp->numFrames;
	v.start = start;
	v.end = end;
	v.env = 0.0f;
	v.envTarget = 1.0f;

	// locked slices always play straight
	bool locked = ( lp->lockMask >> idx ) & 1;
	bool rev     = rolls.rev && !locked;
	bool up      = rolls.up && !locked;
	bool down    = rolls.down && !locked;
	bool stut    = rolls.stut && !locked;
	bool stretch = rolls.stretch && !locked;
	bool gatefx  = rolls.gatefx && !locked;
	v.serpent = ( rolls.serp && !locked ) ? 1 : 0;

	float pitch = 1.0f;
	if ( up && !down )
		pitch = pThis->pitchUpFactor;
	else if ( down && !up )
		pitch = pThis->pitchDownFactor;

	v.dir = rev ? -1.0f : 1.0f;
	// per-loop rate and tune trims apply under the fx pitch dice
	v.rate = lp->srRatio * pitch * pThis->loopSpeed[ li ] * pThis->loopTune[ li ];
	v.pos = rev ? (float)( end - 1 ) : (float)start;

	// clock sync: conform slice duration to the measured clock
	float stretchFactor = 1.0f;
	int sync = pv[ kParamSync ];
	if ( sync && pThis->clockPeriod > 0.0f )
	{
		float f;
		if ( lp->fileBpm > 0.0f )
		{
			// known file tempo: uniform ratio, preserves groove with uneven slices
			int div = pv[ kParamClockDiv ];
			float clockBpm = 60.0f * NT_globals.sampleRate * clockDivQuarters[ div ] / pThis->clockPeriod;
			// rate trim shifts the loop's effective tempo (tune stays free)
			f = lp->fileBpm * pThis->loopSpeed[ li ] / clockBpm;
			if ( div == 0 )
			{
				// Auto: octave-normalise so any power-of-two clock division works
				while ( f < 0.5f ) f *= 2.0f;
				while ( f >= 2.0f ) f *= 0.5f;
			}
			else
			{
				if ( f < 0.25f ) f = 0.25f;
				if ( f > 4.0f ) f = 4.0f;
			}
		}
		else
		{
			// unknown tempo: each slice conforms to one clock tick
			f = pThis->clockPeriod / ( len / v.rate );
			if ( f < 0.25f ) f = 0.25f;
			if ( f > 4.0f ) f = 4.0f;
		}
		if ( sync == 2 )
			v.rate /= f;			// repitch: speed follows tempo, pitch drops/rises
		else
			stretchFactor = f;		// stretch: granular, pitch stays
	}

	if ( stut )
	{
		v.stutter = 1;
		uint32_t sub = len / rolls.stutterDiv;
		if ( sub < 64 )
			sub = 64;
		v.loopLen = sub;
		// stutter fills the sync-conformed slice duration (repitch is already in rate)
		v.framesLeft = ( len / v.rate ) * stretchFactor;
	}
	else
	{
		// stretch effect multiplies on top of any sync stretch
		if ( stretch )
			stretchFactor *= pv[ kParamStretchAmount ] / 100.0f;

		v.framesLeft = len / v.rate;

		if ( stretchFactor < 0.98f || stretchFactor > 1.02f )
		{
			// granular retrigger stretch; factor < 1 compresses (grains skip material)
			v.stretch = 1;
			float g = (float)len * 0.5f;
			if ( g > 4096.0f )
				g = 4096.0f;
			v.grainLen = g;
			v.grainAdv = g / stretchFactor;
			v.grainStart = v.pos;
			v.grainPhase = 0.0f;
			v.framesLeft = ( len / v.rate ) * stretchFactor;
		}
	}

	// gate fx: MPC-style tight chop to half length
	if ( gatefx )
		v.framesLeft *= 0.5f;

	// per-event filter sweep across the (final) slice duration
	if ( rolls.filt && !locked )
	{
		v.fType = rolls.filtType + 1;
		v.fCoef = rolls.filtC0;
		float fl = ( v.framesLeft > 1.0f ) ? v.framesLeft : 1.0f;
		v.fCoefStep = ( rolls.filtC1 - rolls.filtC0 ) / fl;
	}
	else if ( pThis->loopFType[ li ] )
	{
		// static per-loop tone filter (the fx sweep takes over when it fires)
		v.fType = pThis->loopFType[ li ];
		v.fCoef = pThis->loopFCoef[ li ];
		v.fCoefStep = 0.0f;
	}

	// low-pass gate: struck fresh on every slice event, always-on character
	// (not a die), so it ignores locks. Latch the globals so renderVoice stays
	// parameter-free; decay is per-strike because it follows the slice length.
	if ( pThis->lpgDepth > 0.0f )
	{
		v.lpgActive = 1;
		v.lpgEnv = 1.0f;
		v.lpgDepth = pThis->lpgDepth;
		v.lpgDamp = pThis->lpgDamp;
		v.lpgCoefMin = pThis->lpgCoefMin;
		v.lpgCoefMax = pThis->lpgCoefMax;

		// decay time: ~20ms up to the final slice duration, +/-10% per strike
		// (vactrol component drift, so runs of slices don't sound identical)
		float minF = 0.02f * (float)NT_globals.sampleRate;
		float slice = ( v.framesLeft > minF ) ? v.framesLeft : minF;
		float t = minF + pThis->lpgDecayFrac * ( slice - minF );
		t *= 0.9f + 0.2f * pThis->rng.uniform();
		if ( t < 1.0f )
			t = 1.0f;
		v.lpgDecayCoef = expf( -3.5f / t );		// env ~0.03 after t frames
	}
}

// Blend with the Quarrel wander applied (the heads fight over the fader)
static float effectiveBlend( _breakSlicer* pThis )
{
	float b = pThis->v[ kParamBlend ] + pThis->quarrelWalk;
	if ( b < 0.0f ) b = 0.0f;
	if ( b > 100.0f ) b = 100.0f;
	return b;
}

// idx is the slice to play; gridPos is the rhythmic position used for the
// Backbeat weighting (the step phase for clock-driven stepping, the slice's
// own index for CV / MIDI / Random triggers).
// beef: layer the role's one-shot on top, straight (no fx), and duck the
// original slice's voices while it plays
static void startBeefVoice( _breakSlicer* pThis, int role )
{
	if ( role <= kRoleNone || role >= kNumRoles )
		return;
	int bi = role - 1;
	OneShot& os = pThis->beefSample[ bi ];
	if ( !os.loaded || os.numFrames < 2 )
		return;
	float level = pThis->v[ beefLevelParam[bi] ] / 100.0f;
	if ( level <= 0.0f )
		return;

	// choke: current beef voice moves to the fade slot
	if ( pThis->beefVoice.active )
	{
		pThis->beefFadeVoice = pThis->beefVoice;
		pThis->beefFadeVoice.envTarget = 0.0f;
		pThis->beefFadeAmp = pThis->beefAmp;
	}

	Voice& v = pThis->beefVoice;
	memset( &v, 0, sizeof(Voice) );
	v.active = 1;
	v.loopIdx = (uint8_t)bi;
	v.buf = os.sample;
	v.bufFrames = os.numFrames;
	v.start = 0;
	v.end = os.numFrames;
	v.dir = 1.0f;
	v.rate = os.srRatio;
	v.pos = 0.0f;
	v.env = 0.0f;
	v.envTarget = 1.0f;
	v.framesLeft = (float)os.numFrames / v.rate;

	pThis->beefAmp = level;
	pThis->beefDuck = pThis->v[ beefDuckParam[bi] ] / 100.0f;
}

static void triggerSlice( _breakSlicer* pThis, int idx, int gridPos )
{
	const int16_t* pv = pThis->v;

	FxRolls rolls;
	rollFx( pThis, rolls, backbeatWeight( pThis, gridPos ) );

	bool twoLoops = pv[ kParamLoops ] && pThis->loops[1].sliced;

	if ( twoLoops && pv[ kParamBlendMode ] )
	{
		// Xfade: both loops play the slice, amplitudes crossfaded by Blend
		startVoice( pThis, pThis->cur, pThis->fade, &pThis->loops[0], idx, rolls );
		startVoice( pThis, pThis->curB, pThis->fadeB, &pThis->loops[1], idx, rolls );
	}
	else
	{
		// Chance: Blend = probability of drawing the event from loop B
		Loop* lp = &pThis->loops[0];
		if ( twoLoops )
		{
			if ( !pThis->loops[0].sliced )
				lp = &pThis->loops[1];
			else if ( ( pThis->rng.uniform() * 100.0f ) < effectiveBlend( pThis ) )
				lp = &pThis->loops[1];
		}
		startVoice( pThis, pThis->cur, pThis->fade, lp, idx, rolls );
	}

	// beef: role tag always comes from the canonical (master) loop's layout
	Loop* roleLp = pThis->loops[0].sliced ? &pThis->loops[0] : &pThis->loops[1];
	if ( idx >= 0 && idx < roleLp->numSlices )
		startBeefVoice( pThis, roleLp->sliceRole[ idx ] );

	pThis->lastSlice = idx;
}

// the loop whose slice layout drives sequencing / addressing (loop A is master)
static Loop* canonicalLoop( _breakSlicer* pThis )
{
	return pThis->loops[0].sliced ? &pThis->loops[0] : &pThis->loops[1];
}

// the role a boom-bap-ish groove template expects at a grid position: kicks on
// the beat downbeats, snares on the backbeats, hats on the offbeats
static int expectedRole( int gridPos, int n, int beats, int spb )
{
	if ( gridPos % spb != 0 )
		return kRoleHat;						// offbeat
	int beat = ( gridPos * beats ) / n;			// 0-based beat
	return ( beat & 1 ) ? kRoleSnare : kRoleKick;
}

// pick a random slice (0..n-1) carrying role `want` in loop rl; fall back to
// any slice when that tag pool is empty (sparse-pool policy for Role/Pattern)
static int pickSliceForRole( _breakSlicer* pThis, Loop* rl, int want, int n )
{
	int count = 0;
	for ( int i=0; i<n; ++i )
		if ( rl->sliceRole[i] == want )
			++count;
	if ( count == 0 )
		return pThis->rng.next() % n;
	int k = pThis->rng.next() % count;
	for ( int i=0; i<n; ++i )
		if ( rl->sliceRole[i] == want )
		{
			if ( k == 0 )
				return i;
			--k;
		}
	return pThis->rng.next() % n;				// unreachable, safety
}

// pick the slice a Random input trigger should play. gridPos receives the
// rhythmic position for Backbeat weighting: the slice's own index for Free /
// Beat, or the scanned groove slot for Role.
static int randomSlice( _breakSlicer* pThis, int& gridPos )
{
	int n = canonicalSlices( pThis );
	if ( n < 1 )
		n = 1;
	int mode = pThis->v[ kParamRandomMode ];
	if ( mode == 0 )
	{
		int idx = pThis->rng.next() % n;
		gridPos = idx;
		return idx;
	}

	int beats = ( pThis->v[ kParamBars ] + 1 ) * 4;		// 4/4 assumed
	int spb = n / beats;								// slices per beat
	if ( spb < 1 )
		spb = 1;

	if ( mode == 2 )
	{
		// Role mode: scan the groove template by grid position, advancing a
		// dedicated phase (kept separate from the clock/MIDI stepPhase so a
		// Random pulse never shifts the clocked sequence). Pick a random slice
		// carrying the expected tag; fall back to any when the pool is empty.
		Loop* rl = canonicalLoop( pThis );
		int slot = pThis->roleRandomPhase % n;
		pThis->roleRandomPhase = ( pThis->roleRandomPhase + 1 ) % n;
		gridPos = slot;								// weight Backbeat by the groove slot
		int want = expectedRole( slot, n, beats, spb );
		return pickSliceForRole( pThis, rl, want, n );
	}

	// Beat mode: jump to the same position within another beat,
	// so downbeats land on downbeats and the groove survives.
	int groups = n / spb;
	int phase = pThis->lastSlice % spb;
	int idx = ( pThis->rng.next() % groups ) * spb + phase;
	if ( idx >= n )
		idx = pThis->lastSlice;
	gridPos = idx;
	return idx;
}

// advance the clock-follow sequence, returning the slice to play
static int nextStep( _breakSlicer* pThis )
{
	int n = canonicalSlices( pThis );
	if ( n < 1 )
		n = 1;
	int idx;

	switch ( pThis->v[ kParamStepMode ] )
	{
	default:
	case 0:		// forward
		idx = pThis->seqStep % n;
		pThis->seqStep = ( idx + 1 ) % n;
		break;
	case 1:		// reverse
		idx = pThis->seqStep % n;
		pThis->seqStep = ( idx + n - 1 ) % n;
		break;
	case 2:		// ping-pong
	{
		idx = pThis->seqStep % n;
		int ns = idx + pThis->ppDir;
		if ( ns >= n )
		{
			pThis->ppDir = -1;
			ns = ( n >= 2 ) ? n - 2 : 0;
		}
		else if ( ns < 0 )
		{
			pThis->ppDir = 1;
			ns = ( n >= 2 ) ? 1 : 0;
		}
		pThis->seqStep = ns;
	}
		break;
	case 3:		// drunk walk
		idx = pThis->seqStep % n;
		pThis->seqStep = ( idx + ( ( pThis->rng.next() & 1 ) ? 1 : n - 1 ) ) % n;
		break;
	case 4:		// shuffle: random permutation, regenerated each full cycle
		if ( pThis->permN != n || pThis->shufflePos >= n )
		{
			for ( int i=0; i<n; ++i )
				pThis->perm[i] = i;
			for ( int i=n-1; i>0; --i )
			{
				int j = pThis->rng.next() % ( i + 1 );
				uint8_t t = pThis->perm[i];
				pThis->perm[i] = pThis->perm[j];
				pThis->perm[j] = t;
			}
			pThis->permN = n;
			pThis->shufflePos = 0;
		}
		idx = pThis->perm[ pThis->shufflePos++ ];
		break;
	case 5:		// pattern: a groove template maps the bar position to a role,
	{			// then a random slice of that role plays (fallback: any slice)
		int pos = pThis->seqStep % n;
		pThis->seqStep = ( pos + 1 ) % n;
		int bars = pThis->v[ kParamBars ] + 1;
		int stepsPerBar = n / bars;
		if ( stepsPerBar < 1 )
			stepsPerBar = 1;
		int patIdx = ( ( pos % stepsPerBar ) * 16 ) / stepsPerBar;	// -> 16-step grid
		if ( patIdx > 15 )
			patIdx = 15;
		int want = patternTable[ pThis->v[ kParamPattern ] ][ patIdx ];
		idx = pickSliceForRole( pThis, canonicalLoop( pThis ), want, n );
	}
		break;
	}

	return idx;
}

// clock-driven step: play the next sequenced slice, weighting the Backbeat by
// the rhythmic phase since Reset (so Walk/Shuffle still lean on beats 2 & 4).
static void triggerStep( _breakSlicer* pThis )
{
	int n = canonicalSlices( pThis );
	if ( n < 1 )
		n = 1;
	int gridPos = pThis->stepPhase % n;
	triggerSlice( pThis, nextStep( pThis ), gridPos );
	pThis->stepPhase = ( pThis->stepPhase + 1 ) % n;
}

// ---------------------------------------------------------------------------
// MIDI: notes from 36 (C1) upwards trigger slices directly

void	midiMessage( _NT_algorithm* self, uint8_t byte0, uint8_t byte1, uint8_t byte2 )
{
	_breakSlicer* pThis = (_breakSlicer*)self;

	if ( ( byte0 & 0xF0 ) != 0x90 || byte2 == 0 )
		return;
	int chParam = pThis->v[ kParamMidiChannel ];
	if ( chParam && ( byte0 & 0x0F ) != chParam - 1 )
		return;

	int idx = (int)byte1 - 36;
	if ( idx >= 0 && idx < canonicalSlices( pThis ) )
		triggerSlice( pThis, idx, idx );		// MIDI note: slice's own position
}

// MIDI clock (24 PPQN): drives tempo for Sync/ratchet/serpent and, while the
// transport is running, steps the sequence. The analog Clock input wins if
// patched (implicit precedence keeps the parameter count down). Realtime bytes
// are channelless, so the MIDI channel param does not apply here.
void	midiRealtime( _NT_algorithm* self, uint8_t byte )
{
	_breakSlicer* pThis = (_breakSlicer*)self;

	if ( pThis->v[ kParamClockInput ] )		// analog Clock patched: CV wins
		return;

	switch ( byte )
	{
	case 0xF8:		// timing clock tick
	{
		// tempo: measure output frames across 24 ticks -> quarter-note period
		if ( ++pThis->midiTickCount >= 24 )
		{
			uint32_t now = pThis->frameClock;
			uint32_t period = now - pThis->midiLastQuarterFrame;	// wraps cleanly
			pThis->midiLastQuarterFrame = now;
			pThis->midiTickCount = 0;

			// accept only a sane quarter note (30..300 bpm) and slew, since
			// MIDI clock jitters more than an analog edge
			uint32_t sr = NT_globals.sampleRate;
			if ( period >= sr / 5 && period <= sr * 2 )
			{
				int div = pThis->v[ kParamClockDiv ];
				float target = clockDivQuarters[ div ] * (float)period;
				if ( pThis->clockPeriod <= 0.0f )
					pThis->clockPeriod = target;
				else
					pThis->clockPeriod += ( target - pThis->clockPeriod ) * 0.25f;
			}
		}

		// stepping: fire on the first tick after Start (the downbeat), then
		// every N ticks derived from the Clock div
		if ( pThis->midiRunning )
		{
			int div = pThis->v[ kParamClockDiv ];
			int ticksPerStep = roundToInt( clockDivQuarters[ div ] * 24.0f );
			if ( ticksPerStep < 1 )
				ticksPerStep = 1;
			if ( pThis->midiStepTicks == 0 )
				triggerStep( pThis );
			if ( ++pThis->midiStepTicks >= (uint32_t)ticksPerStep )
				pThis->midiStepTicks = 0;
		}
	}
		break;
	case 0xFA:		// start: pull the sequence back to the top (like Reset)
		pThis->seqStep = 0;
		pThis->stepPhase = 0;
		pThis->roleRandomPhase = 0;
		pThis->ppDir = 1;
		pThis->shufflePos = 0;
		pThis->permN = 0;
		pThis->midiStepTicks = 0;
		pThis->midiTickCount = 0;
		pThis->midiLastQuarterFrame = pThis->frameClock;	// anchor tempo here
		pThis->midiRunning = true;
		break;
	case 0xFB:		// continue
		pThis->midiRunning = true;
		break;
	case 0xFC:		// stop: let the current slice ring out
		pThis->midiRunning = false;
		break;
	}
}

// ---------------------------------------------------------------------------
// rendering

static inline void readFrame( const float* buf, uint32_t numFrames, float pos, float& l, float& r )
{
	if ( pos < 0.0f )
		pos = 0.0f;
	uint32_t i = (uint32_t)pos;
	if ( i >= numFrames - 1 )
		i = numFrames - 2;
	float fr = pos - i;
	const float* p = buf + 2 * i;
	l = p[0] + fr * ( p[2] - p[0] );
	r = p[1] + fr * ( p[3] - p[1] );
}

static inline void renderVoice( Voice& v, float amp, float& outL, float& outR, float& sendL, float& sendR )
{
	if ( !v.active || v.bufFrames < 2 )
		return;

	float l, r;

	if ( v.stretch )
	{
		float pos = v.grainStart + v.dir * v.grainPhase;
		readFrame( v.buf, v.bufFrames, pos, l, r );

		v.grainPhase += v.rate;
		if ( v.grainPhase >= v.grainLen )
		{
			v.grainPhase = 0.0f;
			v.grainStart += v.dir * v.grainAdv;
			if ( v.dir > 0.0f )
			{
				if ( v.grainStart >= (float)v.end )
					v.active = 0;
			}
			else
			{
				if ( v.grainStart < (float)v.start )
					v.active = 0;
			}
		}
	}
	else
	{
		readFrame( v.buf, v.bufFrames, v.pos, l, r );

		v.pos += v.dir * v.rate;

		if ( v.stutter )
		{
			if ( v.dir > 0.0f )
			{
				float loopEnd = (float)( v.start + v.loopLen );
				if ( v.pos >= loopEnd )
					v.pos -= (float)v.loopLen;
			}
			else
			{
				float loopStart = (float)( v.end - v.loopLen );
				if ( v.pos < loopStart )
					v.pos += (float)v.loopLen;
			}
		}
		else
		{
			if ( v.pos >= (float)v.end || v.pos < (float)v.start )
				v.active = 0;
		}
	}

	// natural end + declick release
	v.framesLeft -= 1.0f;
	if ( v.framesLeft <= 0.0f )
		v.active = 0;
	else if ( v.framesLeft < (float)kReleaseFrames )
		v.envTarget = 0.0f;

	// declick envelope
	if ( v.env < v.envTarget )
	{
		v.env += 1.0f / kEnvRampFrames;
		if ( v.env > v.envTarget )
			v.env = v.envTarget;
	}
	else if ( v.env > v.envTarget )
	{
		v.env -= 1.0f / kEnvRampFrames;
		if ( v.env < 0.0f )
		{
			v.env = 0.0f;
			v.active = 0;
		}
	}

	// per-event filter sweep
	if ( v.fType )
	{
		float c = v.fCoef;
		v.fCoef = c + v.fCoefStep;
		if ( v.fCoef > 1.2f ) v.fCoef = 1.2f;
		if ( v.fCoef < 0.005f ) v.fCoef = 0.005f;

		v.svLowL += c * v.svBandL;
		float hiL = l - v.svLowL - kFilterDamp * v.svBandL;
		v.svBandL += c * hiL;
		l = ( v.fType == 1 ) ? v.svLowL : hiL;

		v.svLowR += c * v.svBandR;
		float hiR = r - v.svLowR - kFilterDamp * v.svBandR;
		v.svBandR += c * hiR;
		r = ( v.fType == 1 ) ? v.svLowR : hiR;
	}

	// low-pass gate: env drives cutoff, and amplitude follows it with a vactrol
	// skew (env^1.5), blended against the dry voice by depth
	if ( v.lpgActive )
	{
		v.lpgEnv *= v.lpgDecayCoef;
		float env = v.lpgEnv;
		float coef = v.lpgCoefMin + ( v.lpgCoefMax - v.lpgCoefMin ) * env;

		v.lpgLowL += coef * v.lpgBandL;
		float ghiL = l - v.lpgLowL - v.lpgDamp * v.lpgBandL;
		v.lpgBandL += coef * ghiL;

		v.lpgLowR += coef * v.lpgBandR;
		float ghiR = r - v.lpgLowR - v.lpgDamp * v.lpgBandR;
		v.lpgBandR += coef * ghiR;

		float ampEnv = env * sqrtf( env );			// env^1.5
		float d = v.lpgDepth;
		l = l * ( 1.0f - d ) + v.lpgLowL * ampEnv * d;
		r = r * ( 1.0f - d ) + v.lpgLowR * ampEnv * d;
	}

	l *= v.env * amp;
	r *= v.env * amp;
	outL += l;
	outR += r;
	if ( v.serpent )
	{
		sendL += l;
		sendR += r;
	}
}

// ---------------------------------------------------------------------------
// onset analysis, amortised

static void analyseChunk( _breakSlicer* pThis, Loop& L )
{
	uint32_t pos = L.analysisPos;
	uint32_t stop = pos + kAnalysisChunk;
	if ( stop > L.numFrames )
		stop = L.numFrames;

	const float* buf = L.sample;

	while ( pos + kAnalysisHop <= stop )
	{
		float energy = 0.0f;
		float peak = 0.0f;
		const float* p = buf + 2 * pos;
		for ( uint32_t j=0; j<kAnalysisHop; ++j, p += 2 )
		{
			float m = fabsf( p[0] ) + fabsf( p[1] );
			energy += m;
			if ( m > peak )
				peak = m;
		}

		uint32_t hop = pos / kAnalysisHop;
		if ( hop < L.numHops )
		{
			float diff = energy - L.prevHopEnergy;
			L.onset[ hop ] = ( diff > 0.0f ) ? diff : 0.0f;
			L.hopPeak[ hop ] = peak;
		}
		L.prevHopEnergy = energy;

		// waveform display bucket
		uint32_t b = muldivU32( pos, kWaveBuckets, L.numFrames );
		if ( b >= kWaveBuckets )
			b = kWaveBuckets - 1;
		if ( peak > L.wave[ b ] )
			L.wave[ b ] = peak;
		if ( peak > L.waveMax )
			L.waveMax = peak;

		pos += kAnalysisHop;
	}

	// handle the sub-hop tail so analysis always terminates
	if ( stop == L.numFrames && pos + kAnalysisHop > stop )
		pos = stop;

	L.analysisPos = pos;

	if ( pos >= L.numFrames )
	{
		L.analysed = true;
		if ( pThis->v[ kParamSliceMode ] && !L.manualSlices )
			computeSlices( pThis, L );
	}
}

// ---------------------------------------------------------------------------
// step

static inline bool risingEdge( float sample, bool& armed )
{
	if ( armed )
	{
		if ( sample >= kTrigHi )
		{
			armed = false;
			return true;
		}
	}
	else if ( sample < kTrigLo )
		armed = true;
	return false;
}

void 	step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
	_breakSlicer* pThis = (_breakSlicer*)self;
	const int16_t* pv = pThis->v;

	bool cardMounted = NT_isSdCardMounted();
	if ( pThis->cardMounted != cardMounted )
	{
		pThis->cardMounted = cardMounted;
		if ( cardMounted )
		{
			int folders = NT_getNumSampleFolders();
			pThis->params[ kParamFolder ].max = folders ? folders - 1 : 0;
			pThis->params[ kParamFolderB ].max = pThis->params[ kParamFolder ].max;
			NT_updateParameterDefinition( NT_algorithmIndex( self ), kParamFolder );
			NT_updateParameterDefinition( NT_algorithmIndex( self ), kParamFolderB );
			// presets may have loaded before the card mounted
			if ( !pThis->loops[0].loaded )
				requestLoad( pThis, 0 );
			if ( pv[ kParamLoops ] && !pThis->loops[1].loaded )
				requestLoad( pThis, 1 );
		}
	}

	// kick queued loads
	if ( !pThis->awaitingCallback )
	{
		for ( int slot=0; slot<kNumLoadSlots; ++slot )
		{
			if ( pThis->queuedLoad[ slot ] )
			{
				pThis->queuedLoad[ slot ] = false;
				startLoad( pThis, slot );
				break;
			}
		}
	}

	// amortised onset analysis, one loop at a time
	for ( int li=0; li<kNumLoops; ++li )
	{
		Loop& L = pThis->loops[li];
		if ( L.loaded && !L.analysed )
		{
			analyseChunk( pThis, L );
			break;
		}
	}

	int numFrames = numFramesBy4 * 4;

	float* outL = busFrames + ( pv[ kParamOutputL ] - 1 ) * numFrames;
	float* outR = busFrames + ( pv[ kParamOutputR ] - 1 ) * numFrames;
	bool replace = pv[ kParamOutputMode ];

	const float* selBus = pv[ kParamSelectInput ] ? busFrames + ( pv[ kParamSelectInput ] - 1 ) * numFrames : NULL;
	const float* trigBus = pv[ kParamTrigInput ] ? busFrames + ( pv[ kParamTrigInput ] - 1 ) * numFrames : NULL;
	const float* randBus = pv[ kParamRandomInput ] ? busFrames + ( pv[ kParamRandomInput ] - 1 ) * numFrames : NULL;
	const float* clockBus = pv[ kParamClockInput ] ? busFrames + ( pv[ kParamClockInput ] - 1 ) * numFrames : NULL;
	const float* resetBus = pv[ kParamResetInput ] ? busFrames + ( pv[ kParamResetInput ] - 1 ) * numFrames : NULL;
	const float* ratchetBus = pv[ kParamRatchetInput ] ? busFrames + ( pv[ kParamRatchetInput ] - 1 ) * numFrames : NULL;

	float gain = pThis->gain;
	float gainTarget = pThis->gainTarget;

	// quarrel: the heads fight over the blend fader (random walk, leaks to centre)
	if ( pv[ kParamQuarrel ] && pv[ kParamLoops ] )
	{
		float w = pThis->quarrelWalk;
		w += ( pThis->rng.uniform() * 2.0f - 1.0f ) * pv[ kParamQuarrel ] * 0.06f;
		w -= w * 0.005f;
		if ( w < -50.0f ) w = -50.0f;
		if ( w > 50.0f ) w = 50.0f;
		pThis->quarrelWalk = w;
	}
	else
		pThis->quarrelWalk = 0.0f;

	// crossfade amplitude targets (equal power in Xfade mode, unity otherwise)
	float xfTargetA = 1.0f, xfTargetB = 1.0f;
	if ( pv[ kParamLoops ] && pv[ kParamBlendMode ] )
	{
		float b = effectiveBlend( pThis ) * ( 3.14159265f / 200.0f );	// 0..pi/2
		xfTargetA = cosf( b );
		xfTargetB = sinf( b );
	}
	float xfA = pThis->xfA;
	float xfB = pThis->xfB;

	float lgA = pThis->loopGain[0], lgTargetA = pThis->loopGainTarget[0];
	float lgB = pThis->loopGain[1], lgTargetB = pThis->loopGainTarget[1];

	// serpent delay time chases the clock (dotted-eighth feel: 3/4 of a tick)
	float echoTarget = ( pThis->clockPeriod > 0.0f ) ? pThis->clockPeriod * 0.75f : 18000.0f;
	if ( echoTarget < 480.0f ) echoTarget = 480.0f;
	if ( echoTarget > (float)( kEchoFrames - 1000 ) ) echoTarget = (float)( kEchoFrames - 1000 );

	for ( int i=0; i<numFrames; ++i )
	{
		if ( resetBus && risingEdge( resetBus[i], pThis->resetArmed ) )
		{
			pThis->seqStep = 0;
			pThis->stepPhase = 0;
			pThis->roleRandomPhase = 0;
			pThis->ppDir = 1;
			pThis->shufflePos = 0;
			pThis->permN = 0;		// force a fresh shuffle
		}
		if ( trigBus && risingEdge( trigBus[i], pThis->trigArmed ) )
		{
			if ( selBus )
			{
				float cv = selBus[i];
				int idx;
				if ( pv[ kParamSelectMode ] )
				{
					// 1V/oct: quantize to the nearest semitone (0V = MIDI
					// note 12, C0 -- standard Eurorack 1V/oct reference),
					// then offset so the chosen root note lands on slice 0
					// -- e.g. set offset to C2 to make C2 slice 1
					int note = 12 + roundToInt( cv * 12.0f );
					idx = note - pv[ kParamSelectOffset ];
				}
				else
					idx = (int)( cv * ( canonicalSlices( pThis ) / 5.0f ) );
				triggerSlice( pThis, idx, idx );	// CV-addressed: own position
			}
			else
			{
				// no Select CV: triggers follow the step mode, like Clock
				triggerStep( pThis );
			}
		}
		if ( randBus && risingEdge( randBus[i], pThis->randArmed ) )
		{
			int gp;
			int idx = randomSlice( pThis, gp );
			triggerSlice( pThis, idx, gp );
		}
		pThis->framesSinceClock++;
		pThis->frameClock++;		// free-running, for MIDI clock tempo
		if ( clockBus && risingEdge( clockBus[i], pThis->clockArmed ) )
		{
			// measure clock period for Sync (sane range 50ms - 4s)
			uint32_t elapsed = pThis->framesSinceClock;
			pThis->framesSinceClock = 0;
			if ( elapsed >= NT_globals.sampleRate / 20 && elapsed <= NT_globals.sampleRate * 4 )
				pThis->clockPeriod = (float)elapsed;

			triggerStep( pThis );
		}
		if ( ratchetBus )
		{
			// gate high: retrig the current slice at a clock subdivision
			bool high = ratchetBus[i] >= kTrigHi;
			if ( high )
			{
				float interval = ( pThis->clockPeriod > 0.0f )
					? pThis->clockPeriod / ratchetDivValues[ pv[ kParamRatchetDiv ] ]
					: NT_globals.sampleRate / 8.0f;
				if ( !pThis->ratchetHigh )
				{
					triggerSlice( pThis, pThis->lastSlice, pThis->lastSlice );
					pThis->ratchetTimer = interval;
				}
				else
				{
					pThis->ratchetTimer -= 1.0f;
					if ( pThis->ratchetTimer <= 0.0f )
					{
						triggerSlice( pThis, pThis->lastSlice, pThis->lastSlice );
						pThis->ratchetTimer += interval;
					}
				}
			}
			pThis->ratchetHigh = high;
		}

		xfA += ( xfTargetA - xfA ) * 0.002f;
		xfB += ( xfTargetB - xfB ) * 0.002f;
		lgA += ( lgTargetA - lgA ) * 0.002f;
		lgB += ( lgTargetB - lgB ) * 0.002f;
		float ampA = xfA * lgA;
		float ampB = xfB * lgB;

		// beef duck: the current one-shot's own envelope drives how hard the
		// original slice ducks, so it attacks/releases in step with the hit
		float duckMul = 1.0f - pThis->beefDuck * pThis->beefVoice.env;

		float l = 0.0f, r = 0.0f;
		float sendL = 0.0f, sendR = 0.0f;
		renderVoice( pThis->cur, ( pThis->cur.loopIdx ? ampB : ampA ) * duckMul, l, r, sendL, sendR );
		renderVoice( pThis->fade, ( pThis->fade.loopIdx ? ampB : ampA ) * duckMul, l, r, sendL, sendR );
		renderVoice( pThis->curB, ampB * duckMul, l, r, sendL, sendR );
		renderVoice( pThis->fadeB, ampB * duckMul, l, r, sendL, sendR );
		renderVoice( pThis->beefVoice, pThis->beefAmp, l, r, sendL, sendR );
		renderVoice( pThis->beefFadeVoice, pThis->beefFadeAmp, l, r, sendL, sendR );

		// serpent: dub delay tail (darkened feedback, clock-chasing time)
		{
			pThis->echoTime += ( echoTarget - pThis->echoTime ) * 0.0002f;
			float rp = (float)pThis->echoPos - pThis->echoTime;
			if ( rp < 0.0f )
				rp += (float)kEchoFrames;
			uint32_t i0 = (uint32_t)rp;
			uint32_t i1 = ( i0 + 1 ) % kEchoFrames;
			float fr = rp - i0;
			float* e = pThis->echo;
			float eL = e[ 2*i0 ] + fr * ( e[ 2*i1 ] - e[ 2*i0 ] );
			float eR = e[ 2*i0+1 ] + fr * ( e[ 2*i1+1 ] - e[ 2*i0+1 ] );

			// darken the repeats
			pThis->echoLpL += ( eL - pThis->echoLpL ) * 0.35f;
			pThis->echoLpR += ( eR - pThis->echoLpR ) * 0.35f;

			e[ 2*pThis->echoPos ] = sendL + pThis->echoLpL * 0.5f;
			e[ 2*pThis->echoPos+1 ] = sendR + pThis->echoLpR * 0.5f;
			pThis->echoPos = ( pThis->echoPos + 1 ) % kEchoFrames;

			l += eL;
			r += eR;
		}

		// drive: soft-clip saturation
		if ( pv[ kParamDrive ] )
		{
			l = softClip( l * pThis->drivePre ) * pThis->driveMakeup;
			r = softClip( r * pThis->drivePre ) * pThis->driveMakeup;
		}

		// crush: sample-hold decimation + bit quantisation
		if ( pv[ kParamCrush ] )
		{
			pThis->crushPhase += 1.0f;
			if ( pThis->crushPhase >= pThis->crushDiv )
			{
				pThis->crushPhase -= pThis->crushDiv;
				float q = pThis->crushQ;
				pThis->crushL = (int32_t)( l * q ) / q;
				pThis->crushR = (int32_t)( r * q ) / q;
			}
			l = pThis->crushL;
			r = pThis->crushR;
		}

		gain += ( gainTarget - gain ) * 0.002f;
		l *= gain;
		r *= gain;

		if ( replace )
		{
			outL[i] = l;
			outR[i] = r;
		}
		else
		{
			outL[i] += l;
			outR[i] += r;
		}
	}

	pThis->gain = gain;
	pThis->xfA = xfA;
	pThis->xfB = xfB;
	pThis->loopGain[0] = lgA;
	pThis->loopGain[1] = lgB;
}

// ---------------------------------------------------------------------------
// slice editor (Octatrack style: zoom + nudge slice points)

// visible frame window at the current zoom, centred on the selected point
static void editorView( _breakSlicer* pThis, Loop& L, uint32_t& visStart, uint32_t& visFrames )
{
	float zf = exp2f( pThis->zoomPot * 8.0f );			// 1x .. 256x
	uint32_t vis = (uint32_t)( L.numFrames / zf );
	if ( vis < 512 )
		vis = 512;
	if ( vis > L.numFrames )
		vis = L.numFrames;

	uint32_t centre = L.sliceStart[ pThis->selPoint ];
	uint32_t start = ( centre > vis / 2 ) ? centre - vis / 2 : 0;
	if ( start + vis > L.numFrames )
		start = L.numFrames - vis;

	visStart = start;
	visFrames = vis;
}

static void nudgeSelected( _breakSlicer* pThis, Loop& L, int delta )
{
	int s = pThis->selPoint;
	uint32_t visStart, visFrames;
	editorView( pThis, L, visStart, visFrames );

	int step = (int)( visFrames / 256 );				// one pixel at current zoom
	if ( step < 1 )
		step = 1;

	int64_t pt = (int64_t)L.sliceStart[ s ] + (int64_t)delta * step;
	int64_t lo = (int64_t)L.sliceStart[ s-1 ] + kMinSliceFrames;
	int64_t hi = (int64_t)L.sliceStart[ s+1 ] - kMinSliceFrames;
	if ( hi < lo )
		return;			// neighbours too close to move anything
	if ( pt < lo ) pt = lo;
	if ( pt > hi ) pt = hi;

	L.sliceStart[ s ] = (uint32_t)pt;
	L.manualSlices = true;
}

// snap the selected point to the strongest onset within ~0.2s
static void snapSelected( _breakSlicer* pThis, Loop& L )
{
	if ( !L.analysed )
		return;

	int s = pThis->selPoint;
	uint32_t pt = L.sliceStart[ s ];
	uint32_t hop = pt / kAnalysisHop;
	uint32_t window = (uint32_t)( 0.2f * L.srRatio * NT_globals.sampleRate ) / kAnalysisHop;
	uint32_t lo = ( hop > window ) ? hop - window : 0;
	uint32_t hi = hop + window;
	if ( hi >= L.numHops )
		hi = L.numHops ? L.numHops - 1 : 0;

	uint32_t best = hop;
	float bestV = -1.0f;
	for ( uint32_t k=lo; k<=hi; ++k )
	{
		if ( L.onset[k] > bestV )
		{
			bestV = L.onset[k];
			best = k;
		}
	}

	uint32_t snapped = best * kAnalysisHop;
	uint32_t loF = L.sliceStart[ s-1 ] + kMinSliceFrames;
	uint32_t hiF = L.sliceStart[ s+1 ] - kMinSliceFrames;
	if ( hiF < loF )
		return;
	if ( snapped < loF ) snapped = loF;
	if ( snapped > hiF ) snapped = hiF;

	L.sliceStart[ s ] = snapped;
	L.manualSlices = true;
}

// snap the selected point to the nearest zero crossing (mono sum sign flip)
static void snapZero( _breakSlicer* pThis, Loop& L )
{
	int s = pThis->selPoint;
	uint32_t pt = L.sliceStart[ s ];
	if ( L.numFrames < 2 )
		return;

	uint32_t loF = L.sliceStart[ s-1 ] + kMinSliceFrames;
	uint32_t hiF = L.sliceStart[ s+1 ] - kMinSliceFrames;
	if ( hiF < loF )
		return;

	const float* buf = L.sample;
	// mono value at frame f
	#define MONO( f ) ( buf[ 2*(f) ] + buf[ 2*(f) + 1 ] )

	uint32_t best = pt;
	bool found = false;
	for ( uint32_t d=0; d<2048 && !found; ++d )
	{
		// nearest first: check below then above
		if ( pt >= d + 1 && pt - d >= loF )
		{
			uint32_t f = pt - d;
			if ( MONO( f - 1 ) * MONO( f ) <= 0.0f )
			{
				best = f;
				found = true;
				break;
			}
		}
		if ( pt + d < L.numFrames && pt + d <= hiF && pt + d >= 1 )
		{
			uint32_t f = pt + d;
			if ( MONO( f - 1 ) * MONO( f ) <= 0.0f )
			{
				best = f;
				found = true;
			}
		}
	}
	#undef MONO

	if ( !found )
		return;
	if ( best < loF ) best = loF;
	if ( best > hiF ) best = hiF;

	L.sliceStart[ s ] = best;
	L.manualSlices = true;
}

// editor controls tracked for the legend's "last used" highlight
enum
{
	kCtrlNone,
	kCtrlEncLTurn,
	kCtrlEncLPush,
	kCtrlEncRTurn,
	kCtrlEncRPush,
	kCtrlB2,
	kCtrlB4,
	kCtrlPotL,
	kCtrlPotC,
	kCtrlPotR,
};

uint32_t	hasCustomUi( _NT_algorithm* self )
{
	_breakSlicer* pThis = (_breakSlicer*)self;
	uint32_t mask = kNT_button1 | kNT_button3;	// button 1 held = tame
	if ( pThis->editMode )
		mask |= kNT_button2 | kNT_button4 | kNT_encoderL | kNT_encoderR | kNT_encoderButtonL | kNT_encoderButtonR | kNT_potButtonL | kNT_potButtonC | kNT_potR;
	return mask;
}

void	customUi( _NT_algorithm* self, const _NT_uiData& data )
{
	_breakSlicer* pThis = (_breakSlicer*)self;

	uint16_t pressed = data.controls & ~data.lastButtons;

	// tame: while button 1 is held, the beast behaves (all fx suppressed)
	pThis->tamed = ( data.controls & kNT_button1 ) != 0;

	if ( pressed & kNT_button3 )
	{
		Loop& L = pThis->loops[ pThis->editLoop ];
		if ( L.sliced && L.numSlices >= 2 )
			pThis->editMode = !pThis->editMode;
		else
			pThis->editMode = false;
	}

	if ( !pThis->editMode )
		return;

	Loop* L = &pThis->loops[ pThis->editLoop ];

	if ( pressed & kNT_button2 && pThis->v[ kParamLoops ] )
	{
		// switch between loop A and B in the editor
		Loop& other = pThis->loops[ pThis->editLoop ^ 1 ];
		if ( other.sliced && other.numSlices >= 2 )
		{
			pThis->editLoop ^= 1;
			L = &pThis->loops[ pThis->editLoop ];
			if ( pThis->selPoint >= L->numSlices )
				pThis->selPoint = L->numSlices - 1;
			pThis->lastControl = kCtrlB2;
		}
	}

	if ( !L->sliced || L->numSlices < 2 )
		return;

	if ( data.encoders[0] )
	{
		// select point 0..N-1; point 0 is fixed but selectable for locking slice 1
		int s = pThis->selPoint + data.encoders[0];
		int last = L->numSlices - 1;
		if ( s < 0 ) s = last;
		if ( s > last ) s = 0;
		pThis->selPoint = s;
		pThis->lastControl = kCtrlEncLTurn;
	}

	if ( data.encoders[1] && pThis->selPoint >= 1 )
	{
		nudgeSelected( pThis, *L, data.encoders[1] );
		pThis->lastControl = kCtrlEncRTurn;
	}

	if ( pressed & kNT_encoderButtonR && pThis->selPoint >= 1 )
	{
		snapSelected( pThis, *L );
		pThis->lastControl = kCtrlEncRPush;
	}

	if ( pressed & kNT_button4 && pThis->selPoint >= 1 )
	{
		snapZero( pThis, *L );
		pThis->lastControl = kCtrlB4;
	}

	if ( pressed & kNT_encoderButtonL )
	{
		// preview the selected slice straight, no FX rolled
		FxRolls straight = {};
		startVoice( pThis, pThis->cur, pThis->fade, L, pThis->selPoint, straight );
		pThis->lastControl = kCtrlEncLPush;
	}

	if ( pressed & kNT_potButtonL )
	{
		L->lockMask ^= 1u << pThis->selPoint;	// lock slice starting at this point
		pThis->lastControl = kCtrlPotL;
	}

	if ( pressed & kNT_potButtonC )
	{
		// cycle the selected slice's drum-role tag (None -> K -> S -> P -> H -> C)
		uint8_t& role = L->sliceRole[ pThis->selPoint ];
		role = (uint8_t)( ( role + 1 ) % kNumRoles );
		pThis->lastControl = kCtrlPotC;
	}
	else if ( data.controls & kNT_potC )
	{
		// sweep the pot to set the role directly from its position
		int role = (int)( data.pots[1] * kNumRoles );
		if ( role >= kNumRoles ) role = kNumRoles - 1;
		L->sliceRole[ pThis->selPoint ] = (uint8_t)role;
		pThis->lastControl = kCtrlPotC;
	}

	if ( data.controls & kNT_potR )
	{
		pThis->zoomPot = data.pots[2];
		pThis->lastControl = kCtrlPotR;
	}
}

void	setupUi( _NT_algorithm* self, _NT_float3& pots )
{
	_breakSlicer* pThis = (_breakSlicer*)self;
	pots[0] = 0.5f;
	pots[1] = 0.5f;
	pots[2] = pThis->zoomPot;
}

// format a 1-based slice label with musical position, e.g. "S5 b2.1"
// (4/4 assumed; position shown only when slices divide evenly into the beats)
static int formatSliceLabel( char* buf, int idx, int numSlices, int bars )
{
	int n = 0;
	buf[n++] = 'S';
	n += NT_intToString( buf + n, idx + 1 );
	int beats = bars * 4;
	if ( numSlices >= beats && ( numSlices % beats ) == 0 )
	{
		int spb = numSlices / beats;
		int beat = idx / spb;			// 0-based beat across all bars
		buf[n++] = ' ';
		if ( bars > 1 )
		{
			n += NT_intToString( buf + n, beat / 4 + 1 );
			buf[n++] = ':';
		}
		buf[n++] = 'b';
		n += NT_intToString( buf + n, beat % 4 + 1 );
		buf[n++] = '.';
		n += NT_intToString( buf + n, idx % spb + 1 );
	}
	buf[n] = 0;
	return n;
}

// peak level over a frame range, using the hop mipmap when zoomed out
static float rangePeak( Loop& L, uint32_t f0, uint32_t f1 )
{
	if ( f1 > L.numFrames )
		f1 = L.numFrames;
	if ( f0 >= f1 )
		return 0.0f;

	if ( f1 - f0 >= kAnalysisHop && L.analysisPos >= f1 )
	{
		uint32_t h0 = f0 / kAnalysisHop;
		uint32_t h1 = f1 / kAnalysisHop;
		if ( h1 >= L.numHops )
			h1 = L.numHops ? L.numHops - 1 : 0;
		float peak = 0.0f;
		for ( uint32_t h=h0; h<=h1; ++h )
			if ( L.hopPeak[h] > peak )
				peak = L.hopPeak[h];
		return peak;
	}

	const float* p = L.sample + 2 * f0;
	float peak = 0.0f;
	for ( uint32_t f=f0; f<f1; ++f, p += 2 )
	{
		float m = fabsf( p[0] ) + fabsf( p[1] );
		if ( m > peak )
			peak = m;
	}
	return peak;
}

// one legend entry: its label and the lastControl id that highlights it
struct LegendToken { const char* text; uint8_t ctrl; };

// lays out a row of legend tokens, brightening whichever one matches
// lastControl. tiny font is a fixed 3x5 glyph + 1px gap => 4px advance
static void drawTokenRow( int anchorX, int y, _NT_textAlignment groupAlign, uint8_t lastControl, const LegendToken* tokens, int count )
{
	const int kCharW = 4;
	int totalChars = 0;
	for ( int i=0; i<count; ++i )
	{
		totalChars += (int)strlen( tokens[i].text );
		if ( i ) totalChars += 1;	// inter-token space
	}
	int totalW = totalChars * kCharW;
	int x = anchorX;
	if ( groupAlign == kNT_textCentre ) x -= totalW / 2;
	else if ( groupAlign == kNT_textRight ) x -= totalW;
	for ( int i=0; i<count; ++i )
	{
		int colour = ( tokens[i].ctrl != kCtrlNone && tokens[i].ctrl == lastControl ) ? 15 : 8;
		NT_drawText( x, y, tokens[i].text, colour, kNT_textLeft, kNT_textTiny );
		x += ( (int)strlen( tokens[i].text ) + 1 ) * kCharW;
	}
}

static bool drawEditor( _breakSlicer* pThis )
{
	Loop& L = pThis->loops[ pThis->editLoop ];

	uint32_t visStart, visFrames;
	editorView( pThis, L, visStart, visFrames );

	// bottom ~19px reserved: our own control legend, plus clearance below it
	// for the host's own footer bar (algorithm name + control hint), which
	// draws over the very bottom of the screen independent of our content
	const int top = 14, bottom = 45, mid = ( top + bottom ) / 2;
	float scale = ( L.waveMax > 0.001f ) ? ( ( bottom - top ) * 0.5f ) / L.waveMax : 0.0f;

	// waveform at current zoom
	for ( int x=0; x<256; ++x )
	{
		uint32_t f0 = visStart + (uint32_t)( ( (uint64_t)x * visFrames ) >> 8 );
		uint32_t f1 = visStart + (uint32_t)( ( (uint64_t)( x + 1 ) * visFrames ) >> 8 );
		int h = (int)( rangePeak( L, f0, f1 ) * scale );
		if ( h > ( bottom - top ) / 2 )
			h = ( bottom - top ) / 2;
		NT_drawShapeI( kNT_line, x, mid - h, x, mid + h, 4 );
	}

	// slice points in view (point 0 included: selectable for locking slice 1)
	for ( int s=0; s<L.numSlices; ++s )
	{
		uint32_t pt = L.sliceStart[ s ];
		if ( pt < visStart || pt >= visStart + visFrames )
			continue;
		int x = (int)muldivU32( pt - visStart, 256, visFrames );
		bool sel = ( s == pThis->selPoint );
		NT_drawShapeI( kNT_line, x, top, x, bottom, sel ? 15 : 8 );
		if ( sel )
			NT_drawShapeI( kNT_rectangle, x-2, top, x+2, top+2, 15 );
		if ( ( L.lockMask >> s ) & 1 )
			NT_drawText( x+2, top+7, "L", 12, kNT_textLeft, kNT_textTiny );
		if ( L.sliceRole[ s ] )
		{
			char rc[2] = { kRoleInitials[ L.sliceRole[ s ] ], 0 };
			NT_drawText( x+2, top+13, rc, sel ? 15 : 10, kNT_textLeft, kNT_textTiny );
		}
	}

	// playhead (only when playing from the loop being edited)
	Voice* pv = NULL;
	if ( pThis->cur.active && pThis->cur.loopIdx == pThis->editLoop )
		pv = &pThis->cur;
	else if ( pThis->curB.active && pThis->curB.loopIdx == pThis->editLoop )
		pv = &pThis->curB;
	if ( pv )
	{
		float p = pv->stretch ? ( pv->grainStart + pv->dir * pv->grainPhase ) : pv->pos;
		if ( p >= (float)visStart && p < (float)( visStart + visFrames ) )
		{
			int x = (int)( ( p - visStart ) * 256.0f / visFrames );
			NT_drawShapeI( kNT_line, x, mid - 6, x, mid + 6, 12 );
		}
	}

	// header: loop, selected slice (1-based, with beat position), time, zoom
	{
		char buf[48];
		int n = 0;
		if ( pThis->v[ kParamLoops ] )
		{
			const char* head = pThis->editLoop ? "Goat " : "Lion ";
			while ( *head )
				buf[n++] = *head++;
		}
		n += formatSliceLabel( buf + n, pThis->selPoint, L.numSlices, pThis->v[ kParamBars ] + 1 );
		buf[n++] = ' ';
		float fileRate = L.srRatio * NT_globals.sampleRate;
		n += NT_floatToString( buf + n, L.sliceStart[ pThis->selPoint ] / fileRate, 3 );
		buf[n++] = 's';
		buf[n++] = ' ';
		{
			const char* z = "PotR x";
			while ( *z )
				buf[n++] = *z++;
		}
		n += NT_floatToString( buf + n, (float)L.numFrames / visFrames, 1 );
		buf[n] = 0;
		NT_drawText( 0, 10, buf, 15, kNT_textLeft, kNT_textTiny );
		NT_drawText( 254, 10, L.manualSlices ? "edited" : "edit", 8, kNT_textRight, kNT_textTiny );
	}

	// control legend: EncL (always) / centre taps (context) / EncR (needs a movable point).
	// whichever token matches pThis->lastControl draws brighter
	{
		bool pointEditable = pThis->selPoint >= 1;
		const int encY = 62;	// encoders back at the original bottom row
		const int potY = 53;	// pots/buttons staggered above, clear of the host's footer bar

		LegendToken left[] = { { "EncL:Sel", kCtrlEncLTurn }, { "Push:Prev", kCtrlEncLPush } };
		drawTokenRow( 0, encY, kNT_textLeft, pThis->lastControl, left, 2 );

		LegendToken centre[4] = { { "PotL:Lock", kCtrlPotL }, { "PotC:Role", kCtrlPotC }, { "PotR:Zoom", kCtrlPotR } };
		int nc = 3;
		if ( pThis->v[ kParamLoops ] )
			centre[nc++] = { "B2:Swap", kCtrlB2 };
		drawTokenRow( 128, potY, kNT_textCentre, pThis->lastControl, centre, nc );

		LegendToken exitTok[] = { { "B3:Exit", kCtrlNone } };
		drawTokenRow( 256, potY, kNT_textRight, pThis->lastControl, exitTok, 1 );

		if ( pointEditable )
		{
			LegendToken right[] = { { "EncR:Nudge", kCtrlEncRTurn }, { "Push:Snap", kCtrlEncRPush } };
			drawTokenRow( 256, encY, kNT_textRight, pThis->lastControl, right, 2 );
		}
	}

	return true;	// suppress the standard parameter line
}

// ---------------------------------------------------------------------------
// serialisation: manual slice points, locks and role tags persist in the preset

static const char* const kJsonSliceNames[ kNumLoops ] = { "slicePoints", "slicePointsB" };
static const char* const kJsonLockNames[ kNumLoops ] = { "locks", "locksB" };
static const char* const kJsonRoleNames[ kNumLoops ] = { "roles", "rolesB" };

void	serialise( _NT_algorithm* self, _NT_jsonStream& stream )
{
	_breakSlicer* pThis = (_breakSlicer*)self;

	for ( int li=0; li<kNumLoops; ++li )
	{
		Loop& L = pThis->loops[li];

		if ( L.lockMask )
		{
			stream.addMemberName( kJsonLockNames[li] );
			stream.addNumber( (int)L.lockMask );
		}

		if ( L.manualSlices && L.sliced )
		{
			stream.addMemberName( kJsonSliceNames[li] );
			stream.openArray();
			for ( int s=1; s<L.numSlices; ++s )
				stream.addNumber( (int)L.sliceStart[ s ] );
			stream.closeArray();
		}

		// role tags: only written if any slice is tagged. Indexed by slice, so
		// they restore independently of the sample load order.
		int lastTagged = -1;
		for ( int s=0; s<kMaxSlices; ++s )
			if ( L.sliceRole[ s ] )
				lastTagged = s;
		if ( lastTagged >= 0 )
		{
			stream.addMemberName( kJsonRoleNames[li] );
			stream.openArray();
			for ( int s=0; s<=lastTagged; ++s )
				stream.addNumber( (int)L.sliceRole[ s ] );
			stream.closeArray();
		}
	}
}

bool	deserialise( _NT_algorithm* self, _NT_jsonParse& parse )
{
	_breakSlicer* pThis = (_breakSlicer*)self;

	int members;
	if ( !parse.numberOfObjectMembers( members ) )
		return false;

	for ( int j=0; j<members; ++j )
	{
		bool matched = false;
		for ( int li=0; li<kNumLoops && !matched; ++li )
		{
			Loop& L = pThis->loops[li];

			if ( parse.matchName( kJsonLockNames[li] ) )
			{
				int v;
				if ( !parse.number( v ) )
					return false;
				L.lockMask = (uint32_t)v;
				matched = true;
			}
			else if ( parse.matchName( kJsonSliceNames[li] ) )
			{
				int n;
				if ( !parse.numberOfArrayElements( n ) )
					return false;
				int stored = 0;
				for ( int i=0; i<n; ++i )
				{
					int v;
					if ( !parse.number( v ) )
						return false;
					if ( i < kMaxSlices && v > 0 )
						L.pendingPoints[ stored++ ] = (uint32_t)v;
				}
				L.pendingNumSlices = stored + 1;
				L.havePending = ( stored > 0 );
				applyPendingPoints( pThis, L );	// applies now if the sample beat us here
				matched = true;
			}
			else if ( parse.matchName( kJsonRoleNames[li] ) )
			{
				int n;
				if ( !parse.numberOfArrayElements( n ) )
					return false;
				for ( int i=0; i<n; ++i )
				{
					int v;
					if ( !parse.number( v ) )
						return false;
					if ( i < kMaxSlices )
						L.sliceRole[ i ] = ( v > 0 && v < kNumRoles ) ? (uint8_t)v : 0;
				}
				matched = true;
			}
		}
		if ( !matched && !parse.skipMember() )
			return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// display

// one waveform strip in the performance view. ampMul (0..1) dims the
// waveform to reflect this loop's live crossfade level in Xfade blend
// mode (1.0 elsewhere, i.e. no dimming)
static void drawStrip( _breakSlicer* pThis, int li, int top, int bottom, float ampMul )
{
	Loop& L = pThis->loops[li];
	int mid = ( top + bottom ) / 2;

	if ( !L.loaded )
	{
		NT_drawText( 128, mid + 2, pThis->awaitingCallback ? "Loading..." : "No sample", 8, kNT_textCentre, kNT_textTiny );
		return;
	}

	float scale = ( L.waveMax > 0.001f ) ? ( ( bottom - top ) * 0.5f ) / L.waveMax : 0.0f;

	// waveform: brightness (2..6) tracks ampMul so a faded-out loop in
	// an xfade dims without disappearing (slice layout stays visible)
	int waveColour = 2 + (int)( ampMul * 4.0f + 0.5f );
	if ( waveColour > 6 ) waveColour = 6;
	if ( waveColour < 2 ) waveColour = 2;
	for ( int b=0; b<kWaveBuckets; ++b )
	{
		int h = (int)( L.wave[ b ] * scale );
		if ( h > ( bottom - top ) / 2 )
			h = ( bottom - top ) / 2;
		int x = b * 2;
		NT_drawShapeI( kNT_line, x, mid - h, x, mid + h, waveColour );
	}

	// slice markers + lock flags
	uint32_t total = L.numFrames;
	for ( int s=0; s<L.numSlices; ++s )
	{
		int x = (int)muldivU32( L.sliceStart[ s ], 256, total );
		if ( s )
			NT_drawShapeI( kNT_line, x, top, x, bottom, 10 );
		if ( ( L.lockMask >> s ) & 1 )
			NT_drawText( x+2, top+6, "L", 12, kNT_textLeft, kNT_textTiny );
		if ( L.sliceRole[ s ] )
		{
			char rc[2] = { kRoleInitials[ L.sliceRole[ s ] ], 0 };
			NT_drawText( x+2, top+12, rc, 9, kNT_textLeft, kNT_textTiny );
		}
	}

	// current slice highlight + playhead (cur covers chance mode; curB the xfade B layer)
	Voice* v = NULL;
	if ( pThis->cur.active && pThis->cur.loopIdx == li )
		v = &pThis->cur;
	else if ( pThis->curB.active && pThis->curB.loopIdx == li )
		v = &pThis->curB;
	if ( v )
	{
		int s = v->sliceIdx;
		int x0 = (int)muldivU32( L.sliceStart[ s ], 256, total );
		int x1 = (int)muldivU32( L.sliceStart[ s + 1 ], 256, total );
		NT_drawShapeI( kNT_rectangle, x0, top, x1, top + 1, 15 );

		float p = v->stretch ? ( v->grainStart + v->dir * v->grainPhase ) : v->pos;
		int x = (int)( p * 256.0f / total );
		if ( x >= 0 && x < 256 )
			NT_drawShapeI( kNT_line, x, top, x, bottom, 15 );
	}
}

// text with a 1px black halo, so it stays legible sitting on top of a waveform
static void drawOutlinedText( int x, int y, const char* str, int colour, _NT_textAlignment align )
{
	static const int dx[] = { -1, 0, 1, -1, 1, -1, 0, 1 };
	static const int dy[] = { -1, -1, -1, 0, 0, 1, 1, 1 };
	for ( int i=0; i<8; ++i )
		NT_drawText( x + dx[i], y + dy[i], str, 0, align, kNT_textTiny );
	NT_drawText( x, y, str, colour, align, kNT_textTiny );
}

bool	draw( _NT_algorithm* self )
{
	_breakSlicer* pThis = (_breakSlicer*)self;

	bool twoLoops = pThis->v[ kParamLoops ];

	if ( !pThis->loops[0].loaded && !( twoLoops && pThis->loops[1].loaded ) )
	{
		const char* msg = pThis->awaitingCallback ? "Loading..." : "No sample";
		NT_drawText( 128, 38, msg, 15, kNT_textCentre );
		return false;
	}

	if ( pThis->editMode && pThis->loops[ pThis->editLoop ].sliced )
		return drawEditor( pThis );

	if ( twoLoops )
	{
		drawStrip( pThis, 0, 15, 37, pThis->xfA );
		drawStrip( pThis, 1, 40, 62, pThis->xfB );
		drawOutlinedText( 128, 26, "LION", 12, kNT_textCentre );
		drawOutlinedText( 128, 51, "GOAT", 12, kNT_textCentre );
	}
	else
		drawStrip( pThis, 0, 18, 62, 1.0f );

	bool analysing = ( pThis->loops[0].loaded && !pThis->loops[0].analysed )
		|| ( twoLoops && pThis->loops[1].loaded && !pThis->loops[1].analysed );

	// y=30: the host draws its own icon in the top-right corner above here
	if ( pThis->tamed )
		NT_drawText( 254, 30, "tamed", 15, kNT_textRight, kNT_textTiny );
	else if ( analysing && pThis->v[ kParamSliceMode ] )
		NT_drawText( 254, 30, "analysing", 8, kNT_textRight, kNT_textTiny );

	if ( pThis->v[ kParamSync ] && pThis->clockPeriod > 0.0f )
	{
		char buf[32];
		int n = 0;
		if ( pThis->loops[0].fileBpm > 0.0f )
		{
			n += NT_floatToString( buf + n, pThis->loops[0].fileBpm, 0 );
			buf[n++] = '>';
		}
		float bpm = 60.0f * NT_globals.sampleRate * clockDivQuarters[ pThis->v[ kParamClockDiv ] ] / pThis->clockPeriod;
		n += NT_floatToString( buf + n, bpm, 1 );
		buf[n] = 0;
		NT_drawText( 0, 12, buf, 8, kNT_textLeft, kNT_textTiny );
	}

	return false;
}

// ---------------------------------------------------------------------------
// factory

static const _NT_factory factory =
{
	.guid = NT_MULTICHAR( 'C', 'h', 'i', 'm' ),		// guid
	.name = "Chimera",
	.description = "Two-headed breakbeat slicer",
	.numSpecifications = ARRAY_SIZE(specifications),
	.specifications = specifications,
	.calculateStaticRequirements = NULL,
	.initialise = NULL,
	.calculateRequirements = calculateRequirements,
	.construct = construct,
	.parameterChanged = parameterChanged,
	.step = step,
	.draw = draw,
	.midiRealtime = midiRealtime,
	.midiMessage = midiMessage,
	.tags = kNT_tagInstrument,
	.hasCustomUi = hasCustomUi,
	.customUi = customUi,
	.setupUi = setupUi,
	.serialise = serialise,
	.deserialise = deserialise,
	.midiSysEx = NULL,
	.parameterUiPrefix = NULL,
	.parameterString = parameterString,
};

uintptr_t pluginEntry( _NT_selector selector, uint32_t data )
{
	switch ( selector )
	{
	case kNT_selector_version:
		return kNT_apiVersionCurrent;
	case kNT_selector_numFactories:
		return 1;
	case kNT_selector_factoryInfo:
		return (uintptr_t)( ( data == 0 ) ? &factory : NULL );
	}
	return 0;
}
