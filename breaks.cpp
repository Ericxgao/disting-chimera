/*
 * Break Slicer
 * CV-addressable breakbeat slicer for the Expert Sleepers Disting NT.
 * Inspired by amen (norns) and zeptocore/ectocore.
 *
 * Architecture:
 *   WAV sample -> DRAM buffer -> sliced into 4/8/16/32 slices
 *   (equal grid, or transient mode: grid points snap to nearest onset)
 *
 * Triggering:
 *   Select CV in (0-5V -> slice index) sampled on Trigger in rising edge
 *   Random in     -> plays a random slice
 *   Clock in      -> steps through slices sequentially
 *
 * Effects (rolled per slice event, amen style):
 *   each has a 0-100% probability parameter; map it to a fader for
 *   controlled chaos, or map it to a gate (0/100) for manual punch-in.
 *   Reverse, Pitch up, Pitch down, Stutter (sub-loop retrig),
 *   Stretch (grain retrigger timestretch, classic S1000 jungle artefact).
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
	kMaxSlices		= 32,
	kWaveBuckets	= 128,		// waveform display resolution
	kAnalysisHop	= 128,		// frames per onset-envelope hop
	kAnalysisChunk	= 8192,		// frames analysed per step() call
	kMinSliceFrames	= 256,
	kEnvRampFrames	= 32,		// declick attack
	kReleaseFrames	= 48,		// declick release, in output frames
};

static const float kTrigHi = 1.0f;		// volts, rising edge threshold
static const float kTrigLo = 0.5f;		// volts, re-arm threshold

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

	kParamFolder,
	kParamSample,
	kParamSlices,
	kParamSliceMode,

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
	kParamBreak,

	kParamPitchAmount,
	kParamStutterDiv,
	kParamStretchAmount,
	kParamCrush,

	kNumParams,
};

static const char* const sliceCountStrings[] = { "4", "8", "16", "32" };
static const char* const sliceModeStrings[] = { "Equal", "Transient" };
static const char* const stutterDivStrings[] = { "1/2", "1/4", "1/8", "1/16", "Random" };
static const char* const syncModeStrings[] = { "Off", "Stretch", "Repitch" };
static const char* const clockDivStrings[] = { "Auto", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar" };

// quarter notes per clock tick, indexed by kParamClockDiv (entry 0 unused: Auto)
static const float clockDivQuarters[] = { 1.0f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };

static const char* const stepModeStrings[] = { "Forward", "Reverse", "PingPong", "Walk", "Shuffle" };
static const char* const randomModeStrings[] = { "Free", "Beat" };
static const char* const ratchetDivStrings[] = { "1/1", "1/2", "1/3", "1/4", "1/8" };
static const float ratchetDivValues[] = { 1.0f, 2.0f, 3.0f, 4.0f, 8.0f };
static const char* const midiChannelStrings[] = {
	"Omni", "1", "2", "3", "4", "5", "6", "7", "8",
	"9", "10", "11", "12", "13", "14", "15", "16" };

static const _NT_parameter parameters[] = {
	NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output L", 1, 13 )
	NT_PARAMETER_AUDIO_OUTPUT( "Output R", 1, 14 )
	{ .name = "Level", .min = -40, .max = 6, .def = 0, .unit = kNT_unitDb, .scaling = 0, .enumStrings = NULL },

	{ .name = "Folder", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitHasStrings, .scaling = 0, .enumStrings = NULL },
	{ .name = "Sample", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitConfirm, .scaling = 0, .enumStrings = NULL },
	{ .name = "Slices", .min = 0, .max = 3, .def = 2, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = sliceCountStrings },
	{ .name = "Slice mode", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = sliceModeStrings },

	NT_PARAMETER_CV_INPUT( "Select input", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Trig input", 0, 1 )
	NT_PARAMETER_CV_INPUT( "Random input", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Clock input", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Reset input", 0, 0 )
	NT_PARAMETER_CV_INPUT( "Ratchet input", 0, 0 )

	{ .name = "Sync", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = syncModeStrings },
	{ .name = "Clock div", .min = 0, .max = 6, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = clockDivStrings },
	{ .name = "Step mode", .min = 0, .max = 4, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = stepModeStrings },
	{ .name = "Random mode", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = randomModeStrings },
	{ .name = "Ratchet div", .min = 0, .max = 4, .def = 3, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = ratchetDivStrings },
	{ .name = "MIDI channel", .min = 0, .max = 16, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = midiChannelStrings },

	{ .name = "Reverse", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Pitch up", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Pitch down", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Stutter", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Stretch", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Gate", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Break", .min = 0, .max = 100, .def = 50, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },

	{ .name = "Pitch amount", .min = 1, .max = 24, .def = 12, .unit = kNT_unitSemitones, .scaling = 0, .enumStrings = NULL },
	{ .name = "Stutter div", .min = 0, .max = 4, .def = 4, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = stutterDivStrings },
	{ .name = "Stretch amount", .min = 110, .max = 400, .def = 200, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
	{ .name = "Crush", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL },
};

static const uint8_t pageSample[] = { kParamFolder, kParamSample, kParamSlices, kParamSliceMode };
static const uint8_t pageTriggers[] = { kParamSelectInput, kParamTrigInput, kParamRandomInput, kParamClockInput, kParamResetInput, kParamRatchetInput };
static const uint8_t pageSeq[] = { kParamSync, kParamClockDiv, kParamStepMode, kParamRandomMode, kParamRatchetDiv, kParamMidiChannel };
static const uint8_t pageFx[] = { kParamReverse, kParamPitchUp, kParamPitchDown, kParamStutter, kParamStretch, kParamGate, kParamBreak };
static const uint8_t pageFxSetup[] = { kParamPitchAmount, kParamStutterDiv, kParamStretchAmount, kParamCrush };
static const uint8_t pageRouting[] = { kParamOutputL, kParamOutputR, kParamOutputMode, kParamLevel };

static const _NT_parameterPage pages[] = {
	{ .name = "Sample", .numParams = ARRAY_SIZE(pageSample), .params = pageSample },
	{ .name = "Triggers", .numParams = ARRAY_SIZE(pageTriggers), .params = pageTriggers },
	{ .name = "Sequence", .numParams = ARRAY_SIZE(pageSeq), .params = pageSeq },
	{ .name = "FX", .numParams = ARRAY_SIZE(pageFx), .params = pageFx },
	{ .name = "FX setup", .numParams = ARRAY_SIZE(pageFxSetup), .params = pageFxSetup },
	{ .name = "Routing", .numParams = ARRAY_SIZE(pageRouting), .params = pageRouting },
};

static const _NT_parameterPages parameterPages = {
	.numPages = ARRAY_SIZE(pages),
	.pages = pages,
};

// ---------------------------------------------------------------------------
// specifications

static const _NT_specification specifications[] = {
	{ .name = "Max length", .min = 1, .max = 32, .def = 8, .type = kNT_typeSeconds },
};

// ---------------------------------------------------------------------------
// voice

struct Voice
{
	uint8_t		active;
	uint8_t		stutter;
	uint8_t		stretch;
	int8_t		sliceIdx;

	float		dir;			// +1 / -1
	float		rate;			// source frames per output frame

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
};

// ---------------------------------------------------------------------------
// algorithm

struct _breakSlicer : public _NT_algorithm
{
	_breakSlicer() {}
	~_breakSlicer() {}

	_NT_parameter	params[ kNumParams ];

	// sample loading
	_NT_wavRequest	request;
	bool			cardMounted;
	bool			awaitingCallback;
	bool			loaded;
	bool			sliced;

	float*			sample;			// DRAM: interleaved stereo floats
	uint32_t		capFrames;		// buffer capacity in frames
	uint32_t		numFrames;		// valid frames after load
	float			srRatio;		// file rate / host rate
	float			fileBpm;		// parsed from filename "_<bpm>.wav", 0 = unknown

	// onset analysis (amortised over step() calls)
	float*			onset;			// DRAM: onset strength per hop
	float*			hopPeak;		// DRAM: peak level per hop (zoomed waveform mipmap)
	uint32_t		numHops;
	uint32_t		analysisPos;	// frames analysed so far
	float			prevHopEnergy;
	bool			analysed;

	// slices
	uint32_t		sliceStart[ kMaxSlices + 1 ];
	int				numSlices;

	// display
	float			wave[ kWaveBuckets ];
	float			waveMax;

	// triggering
	bool			trigArmed, randArmed, clockArmed, resetArmed;
	int				seqStep;
	int				lastSlice;			// most recently triggered slice
	int				ppDir;				// ping-pong direction
	int				shufflePos;
	int				permN;				// slice count the permutation was built for
	uint8_t			perm[ kMaxSlices ];

	// ratchet
	bool			ratchetHigh;
	float			ratchetTimer;		// output frames until next retrig

	// slice locks (locked slice always plays straight, no fx rolls)
	uint32_t		lockMask;

	// crush (SP-1200 style decimator)
	float			crushQ;				// quantisation levels
	float			crushDiv;			// output frames per held sample
	float			crushPhase;
	float			crushL, crushR;

	// clock measurement (for Sync)
	float			clockPeriod;		// output frames per clock tick, 0 = unknown
	uint32_t		framesSinceClock;

	// slice editor
	bool			editMode;
	bool			manualSlices;	// user-moved points; auto re-slice won't clobber
	int				selPoint;		// selected slice point, 1..numSlices-1
	float			zoomPot;		// 0..1 -> 1x..256x

	// manual points restored from a preset, waiting for the sample to load
	bool			havePending;
	int				pendingNumSlices;
	uint32_t		pendingPoints[ kMaxSlices ];

	// cached parameter values
	float			gain, gainTarget;
	float			pitchUpFactor, pitchDownFactor;

	Voice			cur, fade;
	Rng				rng;
};

// ---------------------------------------------------------------------------
// slicing

static void computeSlices( _breakSlicer* pThis )
{
	int n = 4 << pThis->v[ kParamSlices ];
	pThis->numSlices = n;

	uint32_t total = pThis->numFrames;
	if ( total < (uint32_t)( n * kMinSliceFrames ) )
	{
		// sample too short for this many slices - fall back to what fits
		while ( n > 1 && total < (uint32_t)( n * kMinSliceFrames ) )
			n >>= 1;
		pThis->numSlices = n;
	}

	bool transient = pThis->v[ kParamSliceMode ] && pThis->analysed;

	pThis->sliceStart[0] = 0;
	pThis->sliceStart[n] = total;

	for ( int i=1; i<n; ++i )
	{
		uint32_t grid = (uint32_t)( (uint64_t)i * total / n );

		if ( transient )
		{
			// snap grid point to the strongest onset within +/- 40% of a slice
			uint32_t gridHop = grid / kAnalysisHop;
			uint32_t range = ( total / n ) * 2 / 5 / kAnalysisHop;
			uint32_t lo = ( gridHop > range ) ? gridHop - range : 0;
			uint32_t hi = gridHop + range;
			if ( hi >= pThis->numHops )
				hi = pThis->numHops - 1;
			uint32_t best = gridHop;
			float bestV = -1.0f;
			for ( uint32_t k=lo; k<=hi; ++k )
			{
				if ( pThis->onset[k] > bestV )
				{
					bestV = pThis->onset[k];
					best = k;
				}
			}
			grid = best * kAnalysisHop;
		}

		// enforce monotonic, minimum slice length
		uint32_t minStart = pThis->sliceStart[i-1] + kMinSliceFrames;
		if ( grid < minStart )
			grid = minStart;
		if ( grid > total )
			grid = total;
		pThis->sliceStart[i] = grid;
	}

	pThis->sliced = ( total > 0 );

	if ( pThis->selPoint >= pThis->numSlices )
		pThis->selPoint = pThis->numSlices - 1;
	if ( pThis->selPoint < 0 )
		pThis->selPoint = 0;
}

// apply slice points restored from a preset, once the sample is loaded
static void applyPendingPoints( _breakSlicer* pThis )
{
	if ( !pThis->havePending || !pThis->sliced )
		return;
	if ( pThis->pendingNumSlices != pThis->numSlices )
	{
		pThis->havePending = false;
		return;
	}

	uint32_t total = pThis->numFrames;
	uint32_t prev = 0;
	for ( int i=1; i<pThis->numSlices; ++i )
	{
		uint32_t pt = pThis->pendingPoints[ i-1 ];
		uint32_t minStart = prev + kMinSliceFrames;
		if ( pt < minStart )
			pt = minStart;
		if ( pt > total )
			pt = total;
		pThis->sliceStart[ i ] = pt;
		prev = pt;
	}
	pThis->manualSlices = true;
	pThis->havePending = false;
}

// ---------------------------------------------------------------------------
// construction

void	calculateRequirements( _NT_algorithmRequirements& req, const int32_t* specifications )
{
	uint32_t capFrames = (uint32_t)specifications[0] * 48000;
	uint32_t numHops = capFrames / kAnalysisHop + 2;

	req.numParameters = kNumParams;
	req.sram = sizeof(_breakSlicer);
	req.dram = capFrames * 2 * sizeof(float) + 2 * numHops * sizeof(float);
	req.dtc = 0;
	req.itc = 0;
}

static void wavCallback( void* callbackData, bool success )
{
	_breakSlicer* pThis = (_breakSlicer*)callbackData;
	pThis->awaitingCallback = false;
	if ( success )
	{
		pThis->loaded = true;
		pThis->analysisPos = 0;
		pThis->prevHopEnergy = 0.0f;
		pThis->analysed = false;
		pThis->waveMax = 0.0f;
		memset( pThis->wave, 0, sizeof(pThis->wave) );
		computeSlices( pThis );	// equal grid immediately; transient re-snaps after analysis
		applyPendingPoints( pThis );
	}
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
	alg->sample = (float*)ptrs.dram;
	alg->onset = alg->sample + capFrames * 2;
	alg->hopPeak = alg->onset + maxHops;
	alg->numHops = 0;

	memcpy( alg->params, parameters, sizeof parameters );
	alg->parameters = alg->params;
	alg->parameterPages = &parameterPages;

	alg->request.callback = wavCallback;
	alg->request.callbackData = alg;
	alg->request.bits = kNT_WavBits32;
	alg->request.channels = kNT_WavStereo;
	alg->request.progress = kNT_WavProgress;
	alg->request.startOffset = 0;
	alg->request.dst = alg->sample;

	alg->srRatio = 1.0f;
	alg->gain = alg->gainTarget = 1.0f;
	alg->pitchUpFactor = 2.0f;
	alg->pitchDownFactor = 0.5f;
	alg->trigArmed = alg->randArmed = alg->clockArmed = alg->resetArmed = true;
	alg->ppDir = 1;
	alg->rng.seed( 0xBEA7BEA7u );

	return alg;
}

// ---------------------------------------------------------------------------
// parameters

int 	parameterString( _NT_algorithm* self, int p, int v, char* buff )
{
	_breakSlicer* pThis = (_breakSlicer*)self;
	int len = 0;

	switch ( p )
	{
	case kParamFolder:
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
	{
		_NT_wavInfo info;
		NT_getSampleFileInfo( pThis->v[ kParamFolder ], v, info );
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

static void startLoad( _breakSlicer* pThis )
{
	if ( pThis->awaitingCallback )
		return;

	_NT_wavInfo info;
	NT_getSampleFileInfo( pThis->v[ kParamFolder ], pThis->v[ kParamSample ], info );
	if ( !info.name || !info.numFrames )
		return;

	pThis->loaded = false;
	pThis->sliced = false;
	pThis->analysed = false;
	pThis->cur.active = 0;
	pThis->fade.active = 0;

	pThis->numFrames = ( info.numFrames < pThis->capFrames ) ? info.numFrames : pThis->capFrames;
	pThis->numHops = pThis->numFrames / kAnalysisHop;
	pThis->srRatio = info.sampleRate / (float)NT_globals.sampleRate;
	pThis->fileBpm = parseBpmFromName( info.name );

	pThis->request.folder = pThis->v[ kParamFolder ];
	pThis->request.sample = pThis->v[ kParamSample ];
	pThis->request.numFrames = pThis->numFrames;

	if ( NT_readSampleFrames( pThis->request ) )
		pThis->awaitingCallback = true;
}

void	parameterChanged( _NT_algorithm* self, int p )
{
	_breakSlicer* pThis = (_breakSlicer*)self;

	switch ( p )
	{
	case kParamFolder:
	{
		_NT_wavFolderInfo folderInfo;
		NT_getSampleFolderInfo( pThis->v[ kParamFolder ], folderInfo );
		pThis->params[ kParamSample ].max = folderInfo.numSampleFiles ? folderInfo.numSampleFiles - 1 : 0;
		NT_updateParameterDefinition( NT_algorithmIndex( self ), kParamSample );
	}
		break;
	case kParamSample:
		startLoad( pThis );
		break;
	case kParamSlices:
	case kParamSliceMode:
		pThis->manualSlices = false;	// explicit re-slice discards manual edits
		if ( pThis->loaded )
			computeSlices( pThis );
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
		float c = (float)pThis->v[ kParamCrush ];
		pThis->crushQ = exp2f( 16.0f - c * 0.08f );		// 16 -> 8 bits
		pThis->crushDiv = 1.0f + c * 0.05f;				// 48k -> ~8kHz
	}
		break;
	}
}

// ---------------------------------------------------------------------------
// triggering

static void triggerSlice( _breakSlicer* pThis, int idx )
{
	if ( !pThis->sliced )
		return;

	int n = pThis->numSlices;
	if ( idx < 0 ) idx = 0;
	if ( idx >= n ) idx = n - 1;

	uint32_t start = pThis->sliceStart[ idx ];
	uint32_t end = pThis->sliceStart[ idx + 1 ];
	if ( end <= start + 64 )
		return;
	uint32_t len = end - start;

	// choke: current voice moves to the fade slot
	if ( pThis->cur.active )
	{
		pThis->fade = pThis->cur;
		pThis->fade.envTarget = 0.0f;
	}

	Voice& v = pThis->cur;
	memset( &v, 0, sizeof(Voice) );
	v.active = 1;
	v.sliceIdx = (int8_t)idx;
	v.start = start;
	v.end = end;
	v.env = 0.0f;
	v.envTarget = 1.0f;

	pThis->lastSlice = idx;

	// roll the effects (amen style: probability per event)
	// Break macro scales all probabilities: 50 = as set, 0 = all off, 100 = doubled.
	// Locked slices always play straight.
	const int16_t* pv = pThis->v;
	bool locked = ( pThis->lockMask >> idx ) & 1;
	float scale = locked ? 0.0f : pv[ kParamBreak ] / 50.0f;
	bool rev     = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamReverse ] * scale;
	bool up      = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamPitchUp ] * scale;
	bool down    = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamPitchDown ] * scale;
	bool stut    = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamStutter ] * scale;
	bool stretch = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamStretch ] * scale;
	bool gatefx  = ( pThis->rng.uniform() * 100.0f ) < pv[ kParamGate ] * scale;

	float pitch = 1.0f;
	if ( up && !down )
		pitch = pThis->pitchUpFactor;
	else if ( down && !up )
		pitch = pThis->pitchDownFactor;

	v.dir = rev ? -1.0f : 1.0f;
	v.rate = pThis->srRatio * pitch;
	v.pos = rev ? (float)( end - 1 ) : (float)start;

	// clock sync: conform slice duration to the measured clock
	float stretchFactor = 1.0f;
	int sync = pv[ kParamSync ];
	if ( sync && pThis->clockPeriod > 0.0f )
	{
		float f;
		if ( pThis->fileBpm > 0.0f )
		{
			// known file tempo: uniform ratio, preserves groove with uneven slices
			int div = pv[ kParamClockDiv ];
			float clockBpm = 60.0f * NT_globals.sampleRate * clockDivQuarters[ div ] / pThis->clockPeriod;
			f = pThis->fileBpm / clockBpm;
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
		int div;
		int dv = pv[ kParamStutterDiv ];
		if ( dv >= 4 )
			div = 4 << ( pThis->rng.next() % 3 );		// random: 4, 8 or 16
		else
			div = 2 << dv;								// 2, 4, 8, 16
		uint32_t sub = len / div;
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
}

// pick the slice a Random input trigger should play
static int randomSlice( _breakSlicer* pThis )
{
	int n = pThis->numSlices ? pThis->numSlices : 1;
	if ( pThis->v[ kParamRandomMode ] == 0 )
		return pThis->rng.next() % n;

	// Beat mode: jump to the same position within another beat,
	// so downbeats land on downbeats and the groove survives.
	int spb = n / 4;			// slices per beat, assuming a 1-bar 4/4 loop
	if ( spb < 1 )
		spb = 1;
	int groups = n / spb;
	int phase = pThis->lastSlice % spb;
	int idx = ( pThis->rng.next() % groups ) * spb + phase;
	return ( idx < n ) ? idx : pThis->lastSlice;
}

// advance the clock-follow sequence, returning the slice to play
static int nextStep( _breakSlicer* pThis )
{
	int n = pThis->numSlices ? pThis->numSlices : 1;
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
	}

	return idx;
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

static inline void renderVoice( _breakSlicer* pThis, Voice& v, float& outL, float& outR )
{
	if ( !v.active )
		return;

	float l, r;

	if ( v.stretch )
	{
		float pos = v.grainStart + v.dir * v.grainPhase;
		readFrame( pThis->sample, pThis->numFrames, pos, l, r );

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
		readFrame( pThis->sample, pThis->numFrames, v.pos, l, r );

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

	outL += l * v.env;
	outR += r * v.env;
}

// ---------------------------------------------------------------------------
// onset analysis, amortised

static void analyseChunk( _breakSlicer* pThis )
{
	uint32_t pos = pThis->analysisPos;
	uint32_t stop = pos + kAnalysisChunk;
	if ( stop > pThis->numFrames )
		stop = pThis->numFrames;

	const float* buf = pThis->sample;

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
		if ( hop < pThis->numHops )
		{
			float diff = energy - pThis->prevHopEnergy;
			pThis->onset[ hop ] = ( diff > 0.0f ) ? diff : 0.0f;
			pThis->hopPeak[ hop ] = peak;
		}
		pThis->prevHopEnergy = energy;

		// waveform display bucket
		uint32_t b = (uint32_t)( (uint64_t)pos * kWaveBuckets / pThis->numFrames );
		if ( b >= kWaveBuckets )
			b = kWaveBuckets - 1;
		if ( peak > pThis->wave[ b ] )
			pThis->wave[ b ] = peak;
		if ( peak > pThis->waveMax )
			pThis->waveMax = peak;

		pos += kAnalysisHop;
	}

	// handle the sub-hop tail so analysis always terminates
	if ( stop == pThis->numFrames && pos + kAnalysisHop > stop )
		pos = stop;

	pThis->analysisPos = pos;

	if ( pos >= pThis->numFrames )
	{
		pThis->analysed = true;
		if ( pThis->v[ kParamSliceMode ] && !pThis->manualSlices )
			computeSlices( pThis );
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

	bool cardMounted = NT_isSdCardMounted();
	if ( pThis->cardMounted != cardMounted )
	{
		pThis->cardMounted = cardMounted;
		if ( cardMounted )
		{
			pThis->params[ kParamFolder ].max = NT_getNumSampleFolders() - 1;
			NT_updateParameterDefinition( NT_algorithmIndex( self ), kParamFolder );
			if ( !pThis->loaded )
				startLoad( pThis );		// preset may have loaded before the card mounted
		}
	}

	if ( pThis->loaded && !pThis->analysed )
		analyseChunk( pThis );

	int numFrames = numFramesBy4 * 4;
	const int16_t* pv = pThis->v;

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

	for ( int i=0; i<numFrames; ++i )
	{
		if ( resetBus && risingEdge( resetBus[i], pThis->resetArmed ) )
		{
			pThis->seqStep = 0;
			pThis->ppDir = 1;
			pThis->shufflePos = 0;
			pThis->permN = 0;		// force a fresh shuffle
		}
		if ( trigBus && risingEdge( trigBus[i], pThis->trigArmed ) )
		{
			float cv = selBus ? selBus[i] : 0.0f;
			int idx = (int)( cv * ( pThis->numSlices / 5.0f ) );
			triggerSlice( pThis, idx );
		}
		if ( randBus && risingEdge( randBus[i], pThis->randArmed ) )
			triggerSlice( pThis, randomSlice( pThis ) );
		pThis->framesSinceClock++;
		if ( clockBus && risingEdge( clockBus[i], pThis->clockArmed ) )
		{
			// measure clock period for Sync (sane range 50ms - 4s)
			uint32_t elapsed = pThis->framesSinceClock;
			pThis->framesSinceClock = 0;
			if ( elapsed >= NT_globals.sampleRate / 20 && elapsed <= NT_globals.sampleRate * 4 )
				pThis->clockPeriod = (float)elapsed;

			triggerSlice( pThis, nextStep( pThis ) );
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
					triggerSlice( pThis, pThis->lastSlice );
					pThis->ratchetTimer = interval;
				}
				else
				{
					pThis->ratchetTimer -= 1.0f;
					if ( pThis->ratchetTimer <= 0.0f )
					{
						triggerSlice( pThis, pThis->lastSlice );
						pThis->ratchetTimer += interval;
					}
				}
			}
			pThis->ratchetHigh = high;
		}

		float l = 0.0f, r = 0.0f;
		if ( pThis->loaded && pThis->numFrames >= 2 )
		{
			renderVoice( pThis, pThis->cur, l, r );
			renderVoice( pThis, pThis->fade, l, r );
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
	if ( idx >= 0 && idx < pThis->numSlices )
		triggerSlice( pThis, idx );
}

// ---------------------------------------------------------------------------
// slice editor (Octatrack style: zoom + nudge slice points)

// visible frame window at the current zoom, centred on the selected point
static void editorView( _breakSlicer* pThis, uint32_t& visStart, uint32_t& visFrames )
{
	float zf = exp2f( pThis->zoomPot * 8.0f );			// 1x .. 256x
	uint32_t vis = (uint32_t)( pThis->numFrames / zf );
	if ( vis < 512 )
		vis = 512;
	if ( vis > pThis->numFrames )
		vis = pThis->numFrames;

	uint32_t centre = pThis->sliceStart[ pThis->selPoint ];
	uint32_t start = ( centre > vis / 2 ) ? centre - vis / 2 : 0;
	if ( start + vis > pThis->numFrames )
		start = pThis->numFrames - vis;

	visStart = start;
	visFrames = vis;
}

static void nudgeSelected( _breakSlicer* pThis, int delta )
{
	int s = pThis->selPoint;
	uint32_t visStart, visFrames;
	editorView( pThis, visStart, visFrames );

	int step = (int)( visFrames / 256 );				// one pixel at current zoom
	if ( step < 1 )
		step = 1;

	int64_t pt = (int64_t)pThis->sliceStart[ s ] + (int64_t)delta * step;
	int64_t lo = (int64_t)pThis->sliceStart[ s-1 ] + kMinSliceFrames;
	int64_t hi = (int64_t)pThis->sliceStart[ s+1 ] - kMinSliceFrames;
	if ( pt < lo ) pt = lo;
	if ( pt > hi ) pt = hi;
	if ( hi < lo )
		return;			// neighbours too close to move anything

	pThis->sliceStart[ s ] = (uint32_t)pt;
	pThis->manualSlices = true;
}

// snap the selected point to the strongest onset within ~0.2s
static void snapSelected( _breakSlicer* pThis )
{
	if ( !pThis->analysed )
		return;

	int s = pThis->selPoint;
	uint32_t pt = pThis->sliceStart[ s ];
	uint32_t hop = pt / kAnalysisHop;
	uint32_t window = (uint32_t)( 0.2f * pThis->srRatio * NT_globals.sampleRate ) / kAnalysisHop;
	uint32_t lo = ( hop > window ) ? hop - window : 0;
	uint32_t hi = hop + window;
	if ( hi >= pThis->numHops )
		hi = pThis->numHops ? pThis->numHops - 1 : 0;

	uint32_t best = hop;
	float bestV = -1.0f;
	for ( uint32_t k=lo; k<=hi; ++k )
	{
		if ( pThis->onset[k] > bestV )
		{
			bestV = pThis->onset[k];
			best = k;
		}
	}

	uint32_t snapped = best * kAnalysisHop;
	uint32_t loF = pThis->sliceStart[ s-1 ] + kMinSliceFrames;
	uint32_t hiF = pThis->sliceStart[ s+1 ] - kMinSliceFrames;
	if ( snapped < loF ) snapped = loF;
	if ( snapped > hiF ) snapped = hiF;
	if ( hiF < loF )
		return;

	pThis->sliceStart[ s ] = snapped;
	pThis->manualSlices = true;
}

uint32_t	hasCustomUi( _NT_algorithm* self )
{
	_breakSlicer* pThis = (_breakSlicer*)self;
	uint32_t mask = kNT_button3;
	if ( pThis->editMode )
		mask |= kNT_encoderL | kNT_encoderR | kNT_encoderButtonL | kNT_encoderButtonR | kNT_potR;
	return mask;
}

void	customUi( _NT_algorithm* self, const _NT_uiData& data )
{
	_breakSlicer* pThis = (_breakSlicer*)self;

	uint16_t pressed = data.controls & ~data.lastButtons;

	if ( pressed & kNT_button3 )
	{
		if ( pThis->sliced && pThis->numSlices >= 2 )
			pThis->editMode = !pThis->editMode;
		else
			pThis->editMode = false;
	}

	if ( !pThis->editMode || !pThis->sliced || pThis->numSlices < 2 )
		return;

	if ( data.encoders[0] )
	{
		// select point 0..N-1; point 0 is fixed but selectable for locking slice 0
		int s = pThis->selPoint + data.encoders[0];
		int last = pThis->numSlices - 1;
		if ( s < 0 ) s = last;
		if ( s > last ) s = 0;
		pThis->selPoint = s;
	}

	if ( data.encoders[1] && pThis->selPoint >= 1 )
		nudgeSelected( pThis, data.encoders[1] );

	if ( pressed & kNT_encoderButtonR && pThis->selPoint >= 1 )
		snapSelected( pThis );

	if ( pressed & kNT_encoderButtonL )
		pThis->lockMask ^= 1u << pThis->selPoint;	// lock slice starting at this point

	if ( data.controls & kNT_potR )
		pThis->zoomPot = data.pots[2];
}

void	setupUi( _NT_algorithm* self, _NT_float3& pots )
{
	_breakSlicer* pThis = (_breakSlicer*)self;
	pots[0] = 0.5f;
	pots[1] = 0.5f;
	pots[2] = pThis->zoomPot;
}

// format a 1-based slice label with musical position, e.g. "S5 b2.1"
// (assumes the sample is one 4/4 bar; position shown only when slices divide by 4)
static int formatSliceLabel( char* buf, int idx, int numSlices )
{
	int n = 0;
	buf[n++] = 'S';
	n += NT_intToString( buf + n, idx + 1 );
	if ( numSlices >= 4 && ( numSlices % 4 ) == 0 )
	{
		int spb = numSlices / 4;
		buf[n++] = ' ';
		buf[n++] = 'b';
		n += NT_intToString( buf + n, idx / spb + 1 );
		buf[n++] = '.';
		n += NT_intToString( buf + n, idx % spb + 1 );
	}
	buf[n] = 0;
	return n;
}

// peak level over a frame range, using the hop mipmap when zoomed out
static float rangePeak( _breakSlicer* pThis, uint32_t f0, uint32_t f1 )
{
	if ( f1 > pThis->numFrames )
		f1 = pThis->numFrames;
	if ( f0 >= f1 )
		return 0.0f;

	if ( f1 - f0 >= kAnalysisHop && pThis->analysisPos >= f1 )
	{
		uint32_t h0 = f0 / kAnalysisHop;
		uint32_t h1 = f1 / kAnalysisHop;
		if ( h1 >= pThis->numHops )
			h1 = pThis->numHops ? pThis->numHops - 1 : 0;
		float peak = 0.0f;
		for ( uint32_t h=h0; h<=h1; ++h )
			if ( pThis->hopPeak[h] > peak )
				peak = pThis->hopPeak[h];
		return peak;
	}

	const float* p = pThis->sample + 2 * f0;
	float peak = 0.0f;
	for ( uint32_t f=f0; f<f1; ++f, p += 2 )
	{
		float m = fabsf( p[0] ) + fabsf( p[1] );
		if ( m > peak )
			peak = m;
	}
	return peak;
}

static bool drawEditor( _breakSlicer* pThis )
{
	uint32_t visStart, visFrames;
	editorView( pThis, visStart, visFrames );

	const int top = 14, bottom = 62, mid = ( top + bottom ) / 2;
	float scale = ( pThis->waveMax > 0.001f ) ? ( ( bottom - top ) * 0.5f ) / pThis->waveMax : 0.0f;

	// waveform at current zoom
	for ( int x=0; x<256; ++x )
	{
		uint32_t f0 = visStart + (uint32_t)( (uint64_t)x * visFrames / 256 );
		uint32_t f1 = visStart + (uint32_t)( (uint64_t)( x + 1 ) * visFrames / 256 );
		int h = (int)( rangePeak( pThis, f0, f1 ) * scale );
		if ( h > ( bottom - top ) / 2 )
			h = ( bottom - top ) / 2;
		NT_drawShapeI( kNT_line, x, mid - h, x, mid + h, 4 );
	}

	// slice points in view (point 0 included: selectable for locking slice 0)
	for ( int s=0; s<pThis->numSlices; ++s )
	{
		uint32_t pt = pThis->sliceStart[ s ];
		if ( pt < visStart || pt >= visStart + visFrames )
			continue;
		int x = (int)( (uint64_t)( pt - visStart ) * 256 / visFrames );
		bool sel = ( s == pThis->selPoint );
		NT_drawShapeI( kNT_line, x, top, x, bottom, sel ? 15 : 8 );
		if ( sel )
			NT_drawShapeI( kNT_rectangle, x-2, top, x+2, top+2, 15 );
		if ( ( pThis->lockMask >> s ) & 1 )
			NT_drawText( x+2, top+7, "L", 12, kNT_textLeft, kNT_textTiny );
	}

	// playhead
	if ( pThis->cur.active )
	{
		float p = pThis->cur.stretch ? ( pThis->cur.grainStart + pThis->cur.dir * pThis->cur.grainPhase ) : pThis->cur.pos;
		if ( p >= (float)visStart && p < (float)( visStart + visFrames ) )
		{
			int x = (int)( ( p - visStart ) * 256.0f / visFrames );
			NT_drawShapeI( kNT_line, x, mid - 6, x, mid + 6, 12 );
		}
	}

	// header: selected slice (1-based, with beat position), time, zoom
	{
		char buf[48];
		int n = formatSliceLabel( buf, pThis->selPoint, pThis->numSlices );
		buf[n++] = ' ';
		float fileRate = pThis->srRatio * NT_globals.sampleRate;
		n += NT_floatToString( buf + n, pThis->sliceStart[ pThis->selPoint ] / fileRate, 3 );
		buf[n++] = 's';
		buf[n++] = ' ';
		buf[n++] = 'x';
		n += NT_floatToString( buf + n, (float)pThis->numFrames / visFrames, 1 );
		buf[n] = 0;
		NT_drawText( 0, 10, buf, 15, kNT_textLeft, kNT_textTiny );
		NT_drawText( 254, 10, pThis->manualSlices ? "edited" : "edit", 8, kNT_textRight, kNT_textTiny );
	}

	return true;	// suppress the standard parameter line
}

// ---------------------------------------------------------------------------
// serialisation: manual slice points persist in the preset

void	serialise( _NT_algorithm* self, _NT_jsonStream& stream )
{
	_breakSlicer* pThis = (_breakSlicer*)self;

	if ( pThis->lockMask )
	{
		stream.addMemberName( "locks" );
		stream.addNumber( (int)pThis->lockMask );
	}

	if ( pThis->manualSlices && pThis->sliced )
	{
		stream.addMemberName( "slicePoints" );
		stream.openArray();
		for ( int s=1; s<pThis->numSlices; ++s )
			stream.addNumber( (int)pThis->sliceStart[ s ] );
		stream.closeArray();
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
		if ( parse.matchName( "locks" ) )
		{
			int v;
			if ( !parse.number( v ) )
				return false;
			pThis->lockMask = (uint32_t)v;
		}
		else if ( parse.matchName( "slicePoints" ) )
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
					pThis->pendingPoints[ stored++ ] = (uint32_t)v;
			}
			pThis->pendingNumSlices = stored + 1;
			pThis->havePending = ( stored > 0 );
		}
		else if ( !parse.skipMember() )
			return false;
	}

	applyPendingPoints( pThis );	// applies now if the sample beat us here
	return true;
}

// ---------------------------------------------------------------------------
// display

bool	draw( _NT_algorithm* self )
{
	_breakSlicer* pThis = (_breakSlicer*)self;

	if ( !pThis->loaded )
	{
		const char* msg = pThis->awaitingCallback ? "Loading..." : "No sample";
		NT_drawText( 128, 38, msg, 15, kNT_textCentre );
		return false;
	}

	if ( pThis->editMode && pThis->sliced )
		return drawEditor( pThis );

	const int top = 18, bottom = 62, mid = ( top + bottom ) / 2;
	float scale = ( pThis->waveMax > 0.001f ) ? ( ( bottom - top ) * 0.5f ) / pThis->waveMax : 0.0f;

	// waveform
	for ( int b=0; b<kWaveBuckets; ++b )
	{
		int h = (int)( pThis->wave[ b ] * scale );
		if ( h > ( bottom - top ) / 2 )
			h = ( bottom - top ) / 2;
		int x = b * 2;
		NT_drawShapeI( kNT_line, x, mid - h, x, mid + h, 6 );
	}

	// slice markers + lock flags
	uint32_t total = pThis->numFrames;
	for ( int s=0; s<pThis->numSlices; ++s )
	{
		int x = (int)( (uint64_t)pThis->sliceStart[ s ] * 256 / total );
		if ( s )
			NT_drawShapeI( kNT_line, x, top - 3, x, bottom, 10 );
		if ( ( pThis->lockMask >> s ) & 1 )
			NT_drawText( x+2, top+4, "L", 12, kNT_textLeft, kNT_textTiny );
	}

	// current slice highlight + playhead
	if ( pThis->cur.active )
	{
		int s = pThis->cur.sliceIdx;
		int x0 = (int)( (uint64_t)pThis->sliceStart[ s ] * 256 / total );
		int x1 = (int)( (uint64_t)pThis->sliceStart[ s + 1 ] * 256 / total );
		NT_drawShapeI( kNT_rectangle, x0, top - 3, x1, top - 2, 15 );

		float p = pThis->cur.stretch ? ( pThis->cur.grainStart + pThis->cur.dir * pThis->cur.grainPhase ) : pThis->cur.pos;
		int x = (int)( p * 256.0f / total );
		if ( x >= 0 && x < 256 )
			NT_drawShapeI( kNT_line, x, top, x, bottom, 15 );
	}

	if ( !pThis->analysed && pThis->v[ kParamSliceMode ] )
		NT_drawText( 254, 12, "analysing", 8, kNT_textRight, kNT_textTiny );
	else if ( pThis->cur.active )
	{
		char buf[24];
		formatSliceLabel( buf, pThis->cur.sliceIdx, pThis->numSlices );
		NT_drawText( 254, 12, buf, 8, kNT_textRight, kNT_textTiny );
	}

	if ( pThis->v[ kParamSync ] && pThis->clockPeriod > 0.0f )
	{
		char buf[32];
		int n = 0;
		if ( pThis->fileBpm > 0.0f )
		{
			n += NT_floatToString( buf + n, pThis->fileBpm, 0 );
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
	.guid = NT_MULTICHAR( 'B', 'r', 'k', 's' ),
	.name = "Break Slicer",
	.description = "CV-addressable breakbeat slicer",
	.numSpecifications = ARRAY_SIZE(specifications),
	.specifications = specifications,
	.calculateStaticRequirements = NULL,
	.initialise = NULL,
	.calculateRequirements = calculateRequirements,
	.construct = construct,
	.parameterChanged = parameterChanged,
	.step = step,
	.draw = draw,
	.midiRealtime = NULL,
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
