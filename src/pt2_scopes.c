#include <stdint.h>
#include <stdbool.h>
#include <math.h> // modf()
#ifndef _WIN32
#include <unistd.h> // usleep()
#endif
#include "pt2_header.h"
#include "pt2_helpers.h"
#include "pt2_visuals.h"
#include "pt2_scopes.h"
#include "pt2_tables.h"
#include "pt2_structs.h"
#include "pt2_config.h"
#include "pt2_hpc.h"

// this uses code that is not entirely thread safe, but I have never had any issues so far...

static volatile bool scopesUpdatingFlag, scopesDisplayingFlag;
static hpc_t scopeHpc;
static SDL_Thread *scopeThread;

scope_t scope[AMIGA_VOICES]; // global

void resetCachedScopePeriod(void)
{
	scope_t *s = scope;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, s++)
	{
		s->oldPeriod = -1;
		s->dOldScopeDelta = 0.0;
	}
}

// this is quite hackish, but fixes sample swapping issues
static int32_t getSampleSlotFromReadAddress(const int8_t *sampleReadAddress)
{
	assert(song != NULL);
	const int8_t *sampleData = song->sampleData;
	const int32_t sampleSlotSize = config.maxSampleLength;

	if (sampleData == NULL) // shouldn't really happen, but just in case
		return -1;

	int32_t sampleSlot = 30; // start at last slot

	const int8_t *sampleBaseAddress = &sampleData[sampleSlot * sampleSlotSize];
	if (sampleReadAddress == NULL || sampleReadAddress >= sampleBaseAddress+sampleSlotSize)
		return -1; // out of range

	for (; sampleSlot >= 0; sampleSlot--)
	{
		if (sampleReadAddress >= sampleBaseAddress)
			break;

		sampleBaseAddress -= sampleSlotSize;
	}

	return sampleSlot; // 0..30, or -1 if out of range
}

int32_t getSampleReadPos(int32_t ch) // used for the sampler screen
{
	// cache some stuff
	const scope_t *sc = &scope[ch];
	const bool active = sc->active;
	const int8_t *data = sc->data;
	const int32_t pos = sc->pos;
	const int32_t len = sc->length;

	if (song == NULL || !active || data == NULL)
		return -1;

	/* Because the scopes work like the Paula emulation, we have a DATA
	** and LENGTH variable, which are not static. This means that we have
	** to get creative to get the absolute sampling position.
	*/

	int32_t sample = getSampleSlotFromReadAddress(data);
	if (sample != editor.currSample)
		return -1; // sample is not the one we're seeing in the sampler screen

	const moduleSample_t *s = &song->samples[sample];
	const int8_t *sampleReadAddress = &data[pos];
	const int8_t *sampleBaseAddress = &song->sampleData[s->offset];
	const int32_t realPos = (int32_t)(sampleReadAddress - sampleBaseAddress);

	// return -1 if sample has no loop and read length is 2 (playing sample "loop" area)
	const bool loopEnabled = (s->loopStart + s->loopLength) > 2;
	if (!loopEnabled && len == 2)
		return -1;

	if (realPos < 0 || realPos >= s->length)
		return -1;

	return realPos;
}

void scopeSetPeriod(int32_t ch, int32_t period)
{
	volatile scope_t *s = &scope[ch];

	// if the new period was the same as the previous period, use cached delta
	if (period != s->oldPeriod)
	{
		s->oldPeriod = period;

		const double dPeriodToScopeDeltaDiv = PAULA_PAL_CLK / (double)SCOPE_HZ;
		s->dOldScopeDelta = dPeriodToScopeDeltaDiv / period;
	}

	s->dDelta = s->dOldScopeDelta;
}

void scopeTrigger(int32_t ch)
{
	volatile scope_t *sc = &scope[ch];
	scope_t tempState = *sc; // cache it

	const int8_t *newData = tempState.newData;
	if (newData == NULL)
		newData = &song->sampleData[config.reservedSampleOffset]; // 128K reserved sample

	int32_t newLength = tempState.newLength; // in bytes, not words
	if (newLength < 2)
		newLength = 2; // for safety

	tempState.dPhase = 0.0;
	tempState.pos = 0;
	tempState.data = newData;
	tempState.length = newLength;
	tempState.active = true;

	/* Update live scope now.
	** In theory it -can- be written to in the middle of a cached read,
	** then the read thread writes its own non-updated cached copy back and
	** the trigger never happens. So far I have never seen it happen,
	** so it's probably very rare. Yes, this is not good coding...
	*/
	*sc = tempState;
}

void updateScopes(void)
{
	scope_t tempState;

	if (editor.isWAVRendering)
		return;

	volatile scope_t *sc = scope;

	scopesUpdatingFlag = true;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, sc++)
	{
		tempState = *sc; // cache it
		if (!tempState.active)
			continue; // scope is not active

		tempState.dPhase += tempState.dDelta;

		const int32_t wholeSamples = (int32_t)tempState.dPhase;
		tempState.dPhase -= wholeSamples;
		tempState.pos += wholeSamples;

		if (tempState.pos >= tempState.length)
		{
			// sample reached end, simulate Paula register update (sample swapping)

			/* Wrap pos around one time with current length, then set new length
			** and wrap around it (handles one-shot loops and sample swapping).
			*/
			tempState.pos -= tempState.length;

			tempState.length = tempState.newLength;
			if (tempState.pos >= tempState.length && tempState.length > 0)
				tempState.pos %= tempState.length;

			tempState.data = tempState.newData;
		}

		*sc = tempState; // update scope state
	}
	scopesUpdatingFlag = false;
}

/* This routine gets the average sample amplitude through the running scope voices.
** This gives a somewhat more stable result than getting the peak from the mixer,
** and we don't care about including filters/BLEP in the peak calculation.
*/
static void updateRealVuMeters(void) 
{
	scope_t tmpScope, *sc;

	// sink VU-meters first
	for (int32_t i = 0; i < AMIGA_VOICES; i++)
	{
		editor.realVuMeterVolumes[i] -= 4;
		if (editor.realVuMeterVolumes[i] < 0)
			editor.realVuMeterVolumes[i] = 0;
	}

	// get peak sample data from running scope voices
	sc = scope;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, sc++)
	{
		tmpScope = *sc; // cache it

		if (!tmpScope.active || tmpScope.data == NULL || tmpScope.volume == 0 || tmpScope.length == 0)
			continue;

		// amount of integer samples getting skipped every frame
		const int32_t samplesToScan = (const int32_t)tmpScope.dDelta;
		if (samplesToScan <= 0)
			continue;

		int32_t pos = tmpScope.pos;
		int32_t length = tmpScope.length;
		const int8_t *data = tmpScope.data;

		int32_t runningAmplitude = 0;
		for (int32_t x = 0; x < samplesToScan; x++)
		{
			int32_t amplitude = 0;
			if (data != NULL)
				amplitude = data[pos] * tmpScope.volume;

			runningAmplitude += ABS(amplitude);

			if (++pos >= length)
			{
				pos = 0;

				/* Read cycle done, temporarily update the display data/length variables
				** before the scope thread does it.
				*/
				data = tmpScope.newData;
				length = tmpScope.newLength;
			}
		}

		double dAvgAmplitude = runningAmplitude / (double)samplesToScan;

		dAvgAmplitude *= 96.0 / (128.0 * 64.0); // normalize

		int32_t vuHeight = (int32_t)(dAvgAmplitude + 0.5); // rounded
		if (vuHeight > 48) // max VU-meter height
			vuHeight = 48;

		if ((int8_t)vuHeight > editor.realVuMeterVolumes[i])
			editor.realVuMeterVolumes[i] = (int8_t)vuHeight;
	}
}

void drawScopes(void)
{
	volatile scope_t *sc = scope; // cache it
	int32_t scopeX = 128;

	const uint32_t bgColor = video.palette[PAL_BACKGRD];
	const uint32_t fgColor = video.palette[PAL_QADSCP];

	scopesDisplayingFlag = true;
	for (int32_t i = 0; i < AMIGA_VOICES; i++, sc++)
	{
		scope_t tmpScope = *sc; // cache it

		// render scope
		if (tmpScope.active && tmpScope.data != NULL && tmpScope.volume != 0 && tmpScope.length > 0)
		{
			sc->emptyScopeDrawn = false;

			// fill scope background
			fillRect(scopeX, 55, SCOPE_WIDTH, SCOPE_HEIGHT, bgColor);

			// render scope data
			int16_t scopeData;
			int32_t pos = tmpScope.pos;
			int32_t length = tmpScope.length;
			const int16_t volume = -(tmpScope.volume << 7);
			const int8_t *data = tmpScope.data;
			uint32_t *scopeDrawPtr = &video.frameBuffer[(71 * SCREEN_W) + scopeX];

			for (int32_t x = 0; x < SCOPE_WIDTH; x++)
			{
				scopeData = 0;
				if (data != NULL)
					scopeData = (data[pos] * volume) >> 16;

				scopeDrawPtr[(scopeData * SCREEN_W) + x] = fgColor;

				if (++pos >= length)
				{
					pos = 0;

					// read cycle done, update the drawing data/length variables
					length = tmpScope.newLength;
					data = tmpScope.newData;
				}
			}
		}
		else if (!sc->emptyScopeDrawn)
		{
			// scope is inactive (or vol=0), draw empty scope once until it gets active again

			// fill scope background
			fillRect(scopeX, 55, SCOPE_WIDTH, SCOPE_HEIGHT, bgColor);

			// draw scope line
			hLine(scopeX, 71, SCOPE_WIDTH, fgColor);

			sc->emptyScopeDrawn = true;
		}

		scopeX += SCOPE_WIDTH+8;
	}
	scopesDisplayingFlag = false;
}

static int32_t SDLCALL scopeThreadFunc(void *ptr)
{
	// this is needed for scope stability (confirmed)
	SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);

	hpc_SetDurationInHz(&scopeHpc, SCOPE_HZ);
	hpc_ResetEndTime(&scopeHpc);

	while (editor.programRunning)
	{
		if (config.realVuMeters)
			updateRealVuMeters();

		updateScopes();

		hpc_Wait(&scopeHpc);
	}

	(void)ptr;
	return true;
}

bool initScopes(void)
{
	resetCachedScopePeriod();

	scopeThread = SDL_CreateThread(scopeThreadFunc, NULL, NULL);
	if (scopeThread == NULL)
	{
		showErrorMsgBox("Couldn't create scope thread!");
		return false;
	}

	SDL_DetachThread(scopeThread);
	return true;
}

void stopScope(int32_t ch)
{
	// wait for scopes to finish updating
	while (scopesUpdatingFlag);

	scope[ch].active = false;

	// wait for scope displaying to be done (safety)
	while (scopesDisplayingFlag);
}

void stopAllScopes(void)
{
	// wait for scopes to finish updating
	while (scopesUpdatingFlag);

	for (int32_t i = 0; i < AMIGA_VOICES; i++)
		scope[i].active = false;

	// wait for scope displaying to be done (safety)
	while (scopesDisplayingFlag);
}
