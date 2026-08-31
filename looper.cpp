/*
 * Chimera Looper
 * Clock-synced loop player for the Expert Sleepers Disting NT.
 *
 * A repitch-only stand-in for the firmware's Sample Player (Clocked) that
 * plays entirely from RAM instead of streaming from the MicroSD card, so
 * other algorithms' bulk sample loads (chimera's WAV swaps in particular)
 * can no longer starve its playback. The card is only touched when a new
 * sample is chosen, and that read lands in a back buffer while the current
 * loop keeps playing; the swap waits for the next loop boundary.
 *
 * Sync:
 *   The loop length is chosen as the number of bars (power of two, Auto or
 *   forced) that best fits the sample at the current tempo, and playback is
 *   repitched so the sample spans exactly that many bars. Tempo comes from
 *   the Clock input (interpretation set by Clock div) or from MIDI clock
 *   (24 PPQN; Start/Continue/Stop observed). With Auto trigger on, the loop
 *   retriggers itself exactly on the bar count, locked to clock edges/ticks
 *   rather than a frame counter, so it cannot drift. Without a usable clock
 *   the sample plays at its natural rate.
 *
 * The Folder/Sample parameters carry the exact names chimera's players deck
 * scans for, so this algorithm slots into the deck in place of the factory
 * clocked player (the deck matches either name).
 */

#include <math.h>
#include <string.h>
#include <new>
#include <distingnt/api.h>
#include <distingnt/wav.h>

// ---------------------------------------------------------------------------
// constants

static const float kTrigHi = 1.0f;		// volts, rising edge threshold
static const float kTrigLo = 0.5f;		// volts, re-arm threshold

static const float kInt16Scale = 1.0f / 32768.0f;
static const float kEnvAttack = 1.0f / 32.0f;	// declick ramps, per output frame
static const float kEnvRelease = 1.0f / 128.0f;

// bounded append, for lines assembled from catalogue strings
static int appendStr( char* buf, int n, const char* s, int cap )
{
	while ( *s && n < cap - 1 )
		buf[ n++ ] = *s++;
	buf[ n ] = 0;
	return n;
}

// ---------------------------------------------------------------------------
// parameters

enum
{
	kParamOutputL,
	kParamOutputMode,
	kParamOutputR,
	kParamGain,

	kParamFolder,
	kParamSample,
	kParamFit,
	kParamSpeedTune,

	kParamClockSource,
	kParamClockInput,
	kParamClockDiv,
	kParamBeats,
	kParamAutoTrig,
	kParamPlay,

	kParamPlayInput,
	kParamMidiDiv,

	kNumParams,
};

enum { kSrcOff, kSrcCV, kSrcMidi };

static const char* const clockSourceStrings[] = { "Off", "CV", "MIDI" };
static const char* const clockDivStrings[] = { "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar" };
static const float clockDivQuarters[] = { 0.125f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
static const char* const fitStrings[] =
	{ "Auto", "1/4 bar", "1/2 bar", "1 bar", "2 bars", "4 bars", "8 bars", "16 bars", "32 bars" };
static const float fitBars[] = { 0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f };
static const char* const autoTrigStrings[] = { "Off", "Loop" };
// effective incoming ticks per quarter note. 24 is the MIDI standard; the
// other entries absorb sources that run double/half clock (or the same clock
// arriving on two module inputs at once, which reads as 48)
static const char* const midiDivStrings[] = { "12 ppqn", "24 ppqn", "48 ppqn" };
static const uint32_t midiDivTicks[] = { 12, 24, 48 };
enum { kMidiDiv24 = 1 };

static const _NT_parameter parameters[] = {
	NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output L", 1, 13 )
	NT_PARAMETER_AUDIO_OUTPUT( "Output R", 1, 14 )
	{ .name = "Gain", .min = -40, .max = 24, .def = 0, .unit = kNT_unitDb, .scaling = 0, .enumStrings = NULL },

	// Named exactly "Folder"/"Sample": chimera's players deck resolves a
	// player slot by finding parameters with these names.
	{ .name = "Folder", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitHasStrings, .scaling = 0, .enumStrings = NULL },
	{ .name = "Sample", .min = 0, .max = 32767, .def = 0, .unit = kNT_unitConfirm, .scaling = 0, .enumStrings = NULL },
	{ .name = "Fit", .min = 0, .max = 8, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = fitStrings },
	{ .name = "Speed tune", .min = 500, .max = 2000, .def = 1000, .unit = kNT_unitPercent, .scaling = kNT_scaling10, .enumStrings = NULL },

	{ .name = "Clock source", .min = 0, .max = 2, .def = kSrcCV, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = clockSourceStrings },
	NT_PARAMETER_CV_INPUT( "Clock input", 0, 0 )
	{ .name = "Clock div", .min = 0, .max = 5, .def = 3, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = clockDivStrings },
	{ .name = "Beats/bar", .min = 1, .max = 16, .def = 4, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL },
	{ .name = "Auto trigger", .min = 0, .max = 1, .def = 1, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = autoTrigStrings },
	{ .name = "Play", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL },

	NT_PARAMETER_CV_INPUT( "Play input", 0, 0 )
	{ .name = "MIDI div", .min = 0, .max = 2, .def = kMidiDiv24, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = midiDivStrings },
};

static const uint8_t pageSample[] = { kParamFolder, kParamSample, kParamFit, kParamSpeedTune };
static const uint8_t pageSync[] = { kParamClockSource, kParamClockInput, kParamClockDiv, kParamMidiDiv, kParamBeats, kParamAutoTrig, kParamPlay };
static const uint8_t pageRouting[] = { kParamOutputL, kParamOutputR, kParamOutputMode, kParamGain, kParamPlayInput };

static const _NT_parameterPage pages[] = {
	{ .name = "Sample", .numParams = ARRAY_SIZE(pageSample), .params = pageSample },
	{ .name = "Sync", .numParams = ARRAY_SIZE(pageSync), .params = pageSync },
	{ .name = "Routing", .numParams = ARRAY_SIZE(pageRouting), .params = pageRouting },
};

static const _NT_parameterPages parameterPages = {
	.numPages = ARRAY_SIZE(pages),
	.pages = pages,
};

// ---------------------------------------------------------------------------
// specifications

static const _NT_specification specifications[] = {
	// per buffer; two buffers are reserved so a new sample loads behind the
	// playing one, which is the whole point of this algorithm
	{ .name = "Max length", .min = 1, .max = 30, .def = 12, .type = kNT_typeSeconds },
};

// ---------------------------------------------------------------------------
// state

struct Voice
{
	uint8_t			active;
	const int16_t*	buf;
	uint32_t		numFrames;
	float			pos;		// in sample frames (file rate)
	float			rate;		// sample frames per output frame
	float			env;
	float			envTarget;
};

struct _looper : public _NT_algorithm
{
	_looper() {}
	~_looper() {}

	_NT_parameter	params[ kNumParams ];	// mutable copy (Folder/Sample maxima)

	uint32_t		capFrames;
	int16_t*		buf[ 2 ];
	int				active;			// buffer holding the playing sample
	bool			loaded;
	uint32_t		numFrames;
	float			srRatio;		// file rate / system rate
	int32_t			loadedFolder, loadedSample;

	_NT_wavRequest	request;
	bool			awaitingCallback;
	bool			queuedLoad;
	bool			pendingReady;	// back buffer holds a complete new sample
	uint32_t		pendingFrames;
	float			pendingSrRatio;
	int32_t			pendingFolder, pendingSample;

	bool			cardMounted;

	Voice			cur, fade;

	bool			clockArmed, playArmed;
	uint32_t		framesSinceClock;
	uint32_t		frameClock;			// free-running, for MIDI tempo
	float			quarterPeriod;		// output frames per quarter note, 0 = unknown
	float			quartersAcc;		// quarters since loop start (CV clock)
	uint32_t		midiTicks;			// ticks since loop start (MIDI clock)
	uint32_t		midiTickCount;		// tempo measurement window
	uint32_t		midiLastQuarterFrame;
	bool			midiRunning;
	bool			startPending;		// Start seen; the next tick is the downbeat

	float			loopQuarters;		// loop length, cached at retrigger
	float			gain, gainTarget;
	int16_t			lastPlay;
};

// ---------------------------------------------------------------------------
// loop length / speed

// loop length in quarters for the current sample and tempo. Fit forces a bar
// count; Auto picks the geometrically nearest power of two (incl. 1/4, 1/2)
// to the sample's natural length.
static float chooseLoopQuarters( _looper* pThis )
{
	int beats = pThis->v[ kParamBeats ];
	int fit = pThis->v[ kParamFit ];
	if ( fit > 0 )
		return fitBars[ fit ] * beats;

	if ( pThis->quarterPeriod <= 0.0f || !pThis->numFrames || pThis->srRatio <= 0.0f )
		return (float)beats;	// 1 bar until a clock teaches us better

	float natQuarters = ( (float)pThis->numFrames / pThis->srRatio ) / pThis->quarterPeriod;
	float natBars = natQuarters / (float)beats;
	// smallest power-of-two bar count within sqrt(2) of the natural length =
	// the nearest in log2, without needing log2f (not in the firmware's libm)
	float c = 0.25f;
	while ( c < 32.0f && natBars > c * 1.41421356f )
		c *= 2.0f;
	return c * beats;
}

// sample frames per output frame: squeeze the file into the loop window
static float currentRate( _looper* pThis, uint32_t numFrames )
{
	float tune = pThis->v[ kParamSpeedTune ] * 0.001f;
	if ( pThis->v[ kParamClockSource ] != kSrcOff
		&& pThis->quarterPeriod > 0.0f && pThis->loopQuarters > 0.0f )
		return (float)numFrames * tune / ( pThis->loopQuarters * pThis->quarterPeriod );
	return pThis->srRatio * tune;	// unclocked: natural speed
}

// ---------------------------------------------------------------------------
// loading

static void doSwap( _looper* pThis )
{
	pThis->active ^= 1;
	pThis->numFrames = pThis->pendingFrames;
	pThis->srRatio = pThis->pendingSrRatio;
	pThis->loadedFolder = pThis->pendingFolder;
	pThis->loadedSample = pThis->pendingSample;
	pThis->loaded = true;
	pThis->pendingReady = false;
}

// start the loop from the top. Any newly loaded sample swaps in here, so a
// sample change always lands on a musical boundary. `align` restarts the
// bar counting too (manual triggers); clock-driven retriggers keep theirs.
static void retrigger( _looper* pThis, bool align )
{
	if ( pThis->pendingReady )
		doSwap( pThis );
	if ( align )
	{
		pThis->quartersAcc = 0.0f;
		pThis->midiTicks = 0;
	}
	if ( !pThis->loaded || !pThis->numFrames )
		return;

	if ( pThis->cur.active )
	{
		pThis->fade = pThis->cur;
		pThis->fade.envTarget = 0.0f;
	}

	pThis->loopQuarters = chooseLoopQuarters( pThis );

	Voice& v = pThis->cur;
	v.active = 1;
	v.buf = pThis->buf[ pThis->active ];
	v.numFrames = pThis->numFrames;
	v.pos = 0.0f;
	v.env = 0.0f;
	v.envTarget = 1.0f;
	v.rate = currentRate( pThis, v.numFrames );
}

static void wavCallback( void* callbackData, bool success )
{
	_looper* pThis = (_looper*)callbackData;
	pThis->awaitingCallback = false;
	if ( !success )
		return;
	pThis->pendingReady = true;
	// nothing playing: no boundary to wait for
	if ( !pThis->cur.active )
		doSwap( pThis );
}

static void startLoad( _looper* pThis )
{
	int folder = pThis->v[ kParamFolder ];
	int sample = pThis->v[ kParamSample ];

	// already holding (or already fetching) this file: preset load fires
	// parameterChanged for both the folder and the sample, so without this
	// every selection is read twice
	if ( pThis->loaded && !pThis->pendingReady
		&& pThis->loadedFolder == folder && pThis->loadedSample == sample )
		return;
	if ( pThis->pendingReady
		&& pThis->pendingFolder == folder && pThis->pendingSample == sample )
		return;

	_NT_wavInfo info;
	NT_getSampleFileInfo( folder, sample, info );
	if ( !info.name || !info.numFrames )
		return;

	uint32_t frames = ( info.numFrames < pThis->capFrames ) ? info.numFrames : pThis->capFrames;
	int tgt = pThis->active ^ 1;

	// an unswapped previous load is abandoned; the fade voice may still be
	// ringing out of the target buffer after a recent swap -- cut it rather
	// than read under it (it is milliseconds from silent anyway)
	pThis->pendingReady = false;
	if ( pThis->fade.active && pThis->fade.buf == pThis->buf[ tgt ] )
		pThis->fade.active = 0;

	pThis->pendingFrames = frames;
	pThis->pendingSrRatio = info.sampleRate / (float)NT_globals.sampleRate;
	pThis->pendingFolder = folder;
	pThis->pendingSample = sample;

	pThis->request.folder = folder;
	pThis->request.sample = sample;
	pThis->request.startOffset = 0;
	pThis->request.numFrames = frames;
	pThis->request.dst = pThis->buf[ tgt ];

	if ( NT_readSampleFrames( pThis->request ) )
		pThis->awaitingCallback = true;
}

static void requestLoad( _looper* pThis )
{
	if ( pThis->awaitingCallback )
		pThis->queuedLoad = true;
	else
		startLoad( pThis );
}

// ---------------------------------------------------------------------------
// lifecycle

void	calculateRequirements( _NT_algorithmRequirements& req, const int32_t* specifications )
{
	uint32_t capFrames = (uint32_t)specifications[0] * 48000;
	req.numParameters = kNumParams;
	req.sram = sizeof(_looper);
	req.dram = 2 * capFrames * 2 * sizeof(int16_t);		// two stereo 16-bit buffers
	req.dtc = 0;
	req.itc = 0;
}

_NT_algorithm*	construct( const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements& req, const int32_t* specifications )
{
	static_assert( kNumParams == ARRAY_SIZE(parameters), "parameter count mismatch" );
	(void)req;

	memset( ptrs.sram, 0, sizeof(_looper) );
	_looper* alg = new (ptrs.sram) _looper();

	alg->capFrames = (uint32_t)specifications[0] * 48000;
	alg->buf[0] = (int16_t*)ptrs.dram;
	alg->buf[1] = alg->buf[0] + alg->capFrames * 2;

	memcpy( alg->params, parameters, sizeof parameters );
	alg->parameters = alg->params;
	alg->parameterPages = &parameterPages;

	alg->request.callback = wavCallback;
	alg->request.callbackData = alg;
	alg->request.bits = kNT_WavBits16;
	alg->request.channels = kNT_WavStereo;
	alg->request.progress = kNT_WavProgress;
	alg->request.startOffset = 0;

	alg->loadedFolder = alg->loadedSample = -1;
	alg->srRatio = 1.0f;
	alg->gain = alg->gainTarget = 1.0f;
	alg->clockArmed = alg->playArmed = true;

	return alg;
}

// ---------------------------------------------------------------------------
// parameters

static void refreshSampleMax( _looper* pThis, int algIdx )
{
	_NT_wavFolderInfo folderInfo;
	NT_getSampleFolderInfo( pThis->v[ kParamFolder ], folderInfo );
	pThis->params[ kParamSample ].max =
		folderInfo.numSampleFiles ? folderInfo.numSampleFiles - 1 : 0;
	NT_updateParameterDefinition( algIdx, kParamSample );
}

void	parameterChanged( _NT_algorithm* self, int p )
{
	_looper* pThis = (_looper*)self;

	switch ( p )
	{
	case kParamGain:
		// 10^(dB/20), as 2^(dB * log2(10)/20)
		pThis->gainTarget = exp2f( pThis->v[ kParamGain ] * 0.16609640474f );
		break;
	case kParamFolder:
	{
		int algIdx = NT_algorithmIndex( pThis );
		if ( algIdx >= 0 )
			refreshSampleMax( pThis, algIdx );
		requestLoad( pThis );
	}
		break;
	case kParamSample:
		requestLoad( pThis );
		break;
	case kParamPlay:
		if ( pThis->v[ kParamPlay ] && !pThis->lastPlay )
			retrigger( pThis, true );
		pThis->lastPlay = pThis->v[ kParamPlay ];
		break;
	}
}

int 	parameterString( _NT_algorithm* self, int p, int v, char* buff )
{
	_looper* pThis = (_looper*)self;
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

// ---------------------------------------------------------------------------
// rendering

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

static inline void renderVoice( Voice& v, float& l, float& r )
{
	if ( !v.active )
		return;

	uint32_t i0 = (uint32_t)v.pos;
	float sl, sr;
	if ( i0 >= v.numFrames - 1 )
	{
		sl = v.buf[ 2*(v.numFrames-1) ] * kInt16Scale;
		sr = v.buf[ 2*(v.numFrames-1)+1 ] * kInt16Scale;
	}
	else
	{
		float fr = v.pos - (float)i0;
		const int16_t* s = v.buf + 2*i0;
		sl = ( s[0] + fr * ( s[2] - s[0] ) ) * kInt16Scale;
		sr = ( s[1] + fr * ( s[3] - s[1] ) ) * kInt16Scale;
	}

	if ( v.envTarget > 0.5f )
	{
		v.env += kEnvAttack;
		if ( v.env > 1.0f )
			v.env = 1.0f;
	}
	else
	{
		v.env -= kEnvRelease;
		if ( v.env <= 0.0f )
		{
			v.env = 0.0f;
			v.active = 0;
		}
	}

	l += sl * v.env;
	r += sr * v.env;

	v.pos += v.rate;
	// ride out the tail: if the file ends before the loop boundary (Speed
	// tune > 100%, or a fit shorter than the sample would like), fade rather
	// than stop dead
	if ( (float)v.numFrames - v.pos < v.rate * 64.0f )
		v.envTarget = 0.0f;
	if ( v.pos >= (float)v.numFrames )
		v.active = 0;
}

void 	step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
	_looper* pThis = (_looper*)self;
	const int16_t* pv = pThis->v;

	bool cardMounted = NT_isSdCardMounted();
	if ( pThis->cardMounted != cardMounted )
	{
		pThis->cardMounted = cardMounted;
		if ( cardMounted )
		{
			int algIdx = NT_algorithmIndex( self );
			int folders = NT_getNumSampleFolders();
			pThis->params[ kParamFolder ].max = folders ? folders - 1 : 0;
			NT_updateParameterDefinition( algIdx, kParamFolder );
			refreshSampleMax( pThis, algIdx );
			// presets may have restored before the card mounted
			if ( !pThis->loaded )
				requestLoad( pThis );
		}
	}

	if ( pThis->queuedLoad && !pThis->awaitingCallback )
	{
		pThis->queuedLoad = false;
		startLoad( pThis );
	}

	int numFrames = numFramesBy4 * 4;

	float* outL = busFrames + ( pv[ kParamOutputL ] - 1 ) * numFrames;
	float* outR = busFrames + ( pv[ kParamOutputR ] - 1 ) * numFrames;
	bool replace = pv[ kParamOutputMode ];

	const float* clockBus = pv[ kParamClockInput ] ? busFrames + ( pv[ kParamClockInput ] - 1 ) * numFrames : NULL;
	const float* playBus = pv[ kParamPlayInput ] ? busFrames + ( pv[ kParamPlayInput ] - 1 ) * numFrames : NULL;

	int src = pv[ kParamClockSource ];
	bool autoTrig = pv[ kParamAutoTrig ] != 0;
	bool clocked = ( src != kSrcOff ) && pThis->quarterPeriod > 0.0f;
	float quartersPerEdge = clockDivQuarters[ pv[ kParamClockDiv ] ];

	// the tempo (and Speed tune) may move under a playing loop; follow it
	if ( pThis->cur.active )
		pThis->cur.rate = currentRate( pThis, pThis->cur.numFrames );

	// unclocked auto-loop free-runs: restart whenever the voice ends
	if ( autoTrig && !clocked && !pThis->cur.active && pThis->loaded )
		retrigger( pThis, true );

	float gain = pThis->gain;
	float gainTarget = pThis->gainTarget;

	for ( int i=0; i<numFrames; ++i )
	{
		pThis->frameClock++;
		pThis->framesSinceClock++;

		if ( playBus && risingEdge( playBus[i], pThis->playArmed ) )
			retrigger( pThis, true );

		if ( clockBus && risingEdge( clockBus[i], pThis->clockArmed ) )
		{
			// measure the quarter-note period (sane range 50ms - 4s per edge).
			// CV provides the tempo unless MIDI owns it.
			uint32_t elapsed = pThis->framesSinceClock;
			pThis->framesSinceClock = 0;
			if ( src == kSrcCV
				&& elapsed >= NT_globals.sampleRate / 20 && elapsed <= NT_globals.sampleRate * 4 )
				pThis->quarterPeriod = (float)elapsed / quartersPerEdge;

			// bar position advances on every edge; only the boundary (or the
			// very first start) retriggers. A voice that ended early (Speed
			// tune > 100%) stays silent until the true boundary rather than
			// restarting on the next fine subdivision.
			if ( src == kSrcCV && autoTrig && pThis->loaded )
			{
				pThis->quartersAcc += quartersPerEdge;
				if ( pThis->loopQuarters <= 0.0f )
					retrigger( pThis, true );	// never started: this edge is the downbeat
				else if ( pThis->quartersAcc >= pThis->loopQuarters - 0.0001f )
				{
					pThis->quartersAcc -= pThis->loopQuarters;
					if ( pThis->quartersAcc < 0.0f || pThis->quartersAcc >= pThis->loopQuarters )
						pThis->quartersAcc = 0.0f;
					retrigger( pThis, false );
				}
			}
		}

		float l = 0.0f, r = 0.0f;
		renderVoice( pThis->cur, l, r );
		renderVoice( pThis->fade, l, r );

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
// MIDI clock

void	midiRealtime( _NT_algorithm* self, uint8_t byte )
{
	_looper* pThis = (_looper*)self;

	if ( pThis->v[ kParamClockSource ] != kSrcMidi )
		return;

	switch ( byte )
	{
	case 0xF8:		// timing clock tick
	{
		// strict transport: ticks arriving while stopped neither measure
		// tempo nor advance the bar. Some sources free-run their clock while
		// stopped, and those ticks would otherwise slew the tempo around
		// under a held loop.
		if ( !pThis->midiRunning )
			break;

		bool downbeat = pThis->startPending;
		if ( downbeat )
		{
			// the first tick after Start is the downbeat (MIDI convention),
			// and the clean anchor for the tempo window
			pThis->startPending = false;
			pThis->midiTickCount = 0;
			pThis->midiLastQuarterFrame = pThis->frameClock;
		}
		// tempo: measure output frames across one quarter's worth of ticks
		// (MIDI div) -> quarter-note period, accepted only in a sane range
		// (30..300 bpm) and slewed, since MIDI clock jitters more than an
		// analog edge
		else if ( ++pThis->midiTickCount >= midiDivTicks[ pThis->v[ kParamMidiDiv ] ] )
		{
			uint32_t now = pThis->frameClock;
			uint32_t period = now - pThis->midiLastQuarterFrame;	// wraps cleanly
			pThis->midiLastQuarterFrame = now;
			pThis->midiTickCount = 0;

			uint32_t sr = NT_globals.sampleRate;
			if ( period >= sr / 5 && period <= sr * 2 )
			{
				if ( pThis->quarterPeriod <= 0.0f )
					pThis->quarterPeriod = (float)period;
				else
					pThis->quarterPeriod += ( (float)period - pThis->quarterPeriod ) * 0.25f;
			}
		}

		// same boundary discipline as the CV path: ticks advance the bar
		// position, and only the downbeat/boundary retriggers
		if ( pThis->v[ kParamAutoTrig ] && pThis->loaded )
		{
			if ( downbeat || pThis->loopQuarters <= 0.0f )
				retrigger( pThis, true );	// zeroes midiTicks: this tick is the downbeat
			else
			{
				++pThis->midiTicks;
				uint32_t loopTicks = (uint32_t)( pThis->loopQuarters
					* (float)midiDivTicks[ pThis->v[ kParamMidiDiv ] ] + 0.5f );
				if ( loopTicks && pThis->midiTicks >= loopTicks )
				{
					pThis->midiTicks = 0;
					retrigger( pThis, false );
				}
			}
		}
	}
		break;
	case 0xFA:		// start: the next tick is the downbeat
		pThis->startPending = true;
		pThis->midiRunning = true;
		break;
	case 0xFB:		// continue: bar counting resumes where Stop left it, but
					// the tempo window restarts so it cannot span the silence
		pThis->midiTickCount = 0;
		pThis->midiLastQuarterFrame = pThis->frameClock;
		pThis->midiRunning = true;
		break;
	case 0xFC:		// stop: let the current pass ring out, stop retriggering
		pThis->midiRunning = false;
		break;
	}
}

// ---------------------------------------------------------------------------
// display

bool	draw( _NT_algorithm* self )
{
	_looper* pThis = (_looper*)self;
	char line[ 64 ];
	int n = 0;

	// what is loaded (or arriving)
	if ( pThis->loadedFolder >= 0 )
	{
		_NT_wavInfo info;
		NT_getSampleFileInfo( pThis->loadedFolder, pThis->loadedSample, info );
		if ( info.name )
			n = appendStr( line, n, info.name, sizeof line );
	}
	if ( !n )
		n = appendStr( line, n, "no sample", sizeof line );
	NT_drawText( 8, 28, line, 15 );

	n = 0;
	if ( pThis->awaitingCallback )
		n = appendStr( line, n, "loading...", sizeof line );
	else if ( pThis->pendingReady )
		n = appendStr( line, n, "swaps at loop start", sizeof line );
	else if ( pThis->cur.active && pThis->loopQuarters > 0.0f && pThis->quarterPeriod > 0.0f
		&& pThis->v[ kParamClockSource ] != kSrcOff )
	{
		int beats = pThis->v[ kParamBeats ];
		n = appendStr( line, n, "fit ", sizeof line );
		n += NT_floatToString( line + n, pThis->loopQuarters / (float)beats, 2 );
		n = appendStr( line, n, " bars  ", sizeof line );
		float bpm = 60.0f * (float)NT_globals.sampleRate / pThis->quarterPeriod;
		n += NT_floatToString( line + n, bpm, 1 );
		n = appendStr( line, n, " bpm  ", sizeof line );
		float spd = ( pThis->srRatio > 0.0f )
			? pThis->cur.rate / pThis->srRatio * 100.0f : 100.0f;
		n += NT_floatToString( line + n, spd, 1 );
		n = appendStr( line, n, "%", sizeof line );
	}
	else if ( pThis->cur.active )
		n = appendStr( line, n, "free running", sizeof line );
	else
		n = appendStr( line, n, "stopped", sizeof line );
	NT_drawText( 8, 42, line, 10 );

	return false;
}

// ---------------------------------------------------------------------------
// factory

static const _NT_factory factory =
{
	.guid = NT_MULTICHAR( 'C', 'h', 'L', 'p' ),
	.name = "Chimera Looper",
	.description = "RAM-resident clocked loop player",
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
	.midiMessage = NULL,
	.tags = kNT_tagInstrument,
	.hasCustomUi = NULL,
	.customUi = NULL,
	.setupUi = NULL,
	.serialise = NULL,
	.deserialise = NULL,
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
