/*
 * APM decoder / player.
 *
 * Decodes IMA ADPCM files created by wav2apm.py (see its header for the
 * file layout) block by block.
 *
 * Linux:  the file is decoded with Jack Jansen's adpcm_decoder() and written
 *         to <input>_decoded.wav.
 * Atari:  the file is played through a double buffer in ST-RAM which is
 *         refilled whenever the DMA has finished playing one half of it:
 *         - CPU: decoded with adpcm_decoder() into 16-bit samples, played
 *           by the DMA,
 *         - DSP (-d): the file itself is played by the DMA into the DSP
 *           (handshake mode), which decodes it and sends the samples to
 *           the DAC (see dspdec.asm).
 *         Statistics about the CPU load are printed at the end. Any key
 *         stops the playback.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adpcm.h"

#ifdef __MINT__
#include <mint/osbind.h>
#include <mint/falcon.h>
#include <time.h>
#include <unistd.h>
#include <usound.h>

#include "dspdec.p56.h"
#endif

#define APM_HEADER_SIZE		32
#define APM_VERSION			1
#define APM_MAX_INDEX		88

#define DEFAULT_BLOCKS_PER_CHUNK	1

typedef struct {
	int				channels;
	unsigned long	rate;
	long			blockSize;		/* bytes */
	unsigned long	samples;		/* per channel */
	long			codeBytes;		/* per channel per block */
	long			blockFrames;	/* per channel per block */
} ApmInfo;

typedef struct {
	FILE*			file;
	ApmInfo			info;
	unsigned char*	block;			/* one encoded block */
	char*			codes;			/* codes of one channel of a block */
	short*			mono;			/* decoded samples of one channel of a block */
	unsigned long	framesLeft;
} Decoder;

static unsigned long readBe16(const unsigned char* p)
{
	return ((unsigned long)p[0] << 8) | p[1];
}

static unsigned long readBe32(const unsigned char* p)
{
	return (readBe16(p) << 16) | readBe16(p + 2);
}

static int decoderOpen(Decoder* d, const char* fileName)
{
	unsigned char header[APM_HEADER_SIZE];
	ApmInfo* info = &d->info;

	memset(d, 0, sizeof(*d));

	d->file = fopen(fileName, "rb");
	if (d->file == NULL) {
		fprintf(stderr, "%s: cannot open\n", fileName);
		return 0;
	}

	if (fread(header, 1, sizeof(header), d->file) != sizeof(header)
		|| memcmp(header, "APCM", 4) != 0
		|| readBe16(header + 4) != APM_VERSION) {
		fprintf(stderr, "%s: not an APM version %d file\n", fileName, APM_VERSION);
		return 0;
	}

	info->channels = readBe16(header + 6);
	info->rate = readBe32(header + 8);
	info->blockSize = readBe16(header + 12);
	info->samples = readBe32(header + 16);

	if (info->channels < 1 || info->channels > 2
		|| info->blockSize < 32 || (info->blockSize & (info->blockSize - 1)) != 0) {
		fprintf(stderr, "%s: unsupported channels (%d) or block size (%ld)\n",
			fileName, info->channels, info->blockSize);
		return 0;
	}

	info->codeBytes = (info->blockSize - 4 * info->channels) / info->channels;
	info->blockFrames = 1 + 2 * info->codeBytes;

	d->block = malloc(info->blockSize);
	d->codes = malloc(info->codeBytes);
	d->mono = malloc(info->blockFrames * sizeof(short));
	if (d->block == NULL || d->codes == NULL || d->mono == NULL) {
		fprintf(stderr, "out of memory\n");
		return 0;
	}

	d->framesLeft = info->samples;
	return 1;
}

static void decoderClose(Decoder* d)
{
	free(d->mono);
	free(d->codes);
	free(d->block);
	if (d->file != NULL)
		fclose(d->file);
}

/*
 * Decode one block into interleaved samples with outChannels channels
 * (a mono file is duplicated into both channels when outChannels is 2).
 * Returns the number of frames written, 0 at the end of the file, -1 on error.
 */
static long decodeBlock(Decoder* d, short* out, int outChannels)
{
	const ApmInfo* info = &d->info;
	long frames;
	int ch;

	if (d->framesLeft == 0)
		return 0;

	if (fread(d->block, 1, info->blockSize, d->file) != (size_t)info->blockSize) {
		fprintf(stderr, "unexpected end of file\n");
		return -1;
	}

	frames = info->blockFrames;
	if ((unsigned long)frames > d->framesLeft)
		frames = d->framesLeft;

	for (ch = 0; ch < info->channels; ch++) {
		const unsigned char* header = d->block + 4 * ch;
		const unsigned char* data = d->block + 4 * info->channels + ch;
		struct adpcm_state state;
		long i;
		int oc;

		if (header[2] > APM_MAX_INDEX) {
			fprintf(stderr, "corrupted block (step index %d)\n", header[2]);
			return -1;
		}

		state.valprev = (short)readBe16(header);
		state.index = header[2];

		for (i = 0; i < info->codeBytes; i++)
			d->codes[i] = data[i * info->channels];

		d->mono[0] = state.valprev;
		adpcm_decoder(d->codes, d->mono + 1, 2 * info->codeBytes, &state);

		for (oc = ch; oc < outChannels; oc += info->channels) {
			for (i = 0; i < frames; i++)
				out[i * outChannels + oc] = d->mono[i];
		}
	}

	d->framesLeft -= frames;
	return frames;
}

#ifndef __MINT__

static void writeLe16(FILE* f, unsigned long x)
{
	fputc(x & 0xff, f);
	fputc((x >> 8) & 0xff, f);
}

static void writeLe32(FILE* f, unsigned long x)
{
	writeLe16(f, x & 0xffff);
	writeLe16(f, x >> 16);
}

static void writeWavHeader(FILE* f, int channels, unsigned long rate, unsigned long frames)
{
	unsigned long dataSize = frames * channels * 2;

	fwrite("RIFF", 1, 4, f);
	writeLe32(f, 36 + dataSize);
	fwrite("WAVEfmt ", 1, 8, f);
	writeLe32(f, 16);
	writeLe16(f, 1);					/* PCM */
	writeLe16(f, channels);
	writeLe32(f, rate);
	writeLe32(f, rate * channels * 2);
	writeLe16(f, channels * 2);
	writeLe16(f, 16);
	fwrite("data", 1, 4, f);
	writeLe32(f, dataSize);
}

static int decodeToWav(Decoder* d, const char* inputName)
{
	const ApmInfo* info = &d->info;
	char* outputName;
	const char* dot;
	short* pcm;
	FILE* f;
	unsigned long frames = 0;
	long n;

	dot = strrchr(inputName, '.');
	if (dot == NULL || strchr(dot, '/') != NULL)
		dot = inputName + strlen(inputName);

	outputName = malloc((dot - inputName) + sizeof("_decoded.wav"));
	pcm = malloc(info->blockFrames * info->channels * sizeof(short));
	if (outputName == NULL || pcm == NULL) {
		fprintf(stderr, "out of memory\n");
		return 0;
	}
	sprintf(outputName, "%.*s_decoded.wav", (int)(dot - inputName), inputName);

	f = fopen(outputName, "wb");
	if (f == NULL) {
		fprintf(stderr, "%s: cannot create\n", outputName);
		return 0;
	}

	writeWavHeader(f, info->channels, info->rate, 0);

	while ((n = decodeBlock(d, pcm, info->channels)) > 0) {
		long i;

		for (i = 0; i < n * info->channels; i++)
			writeLe16(f, (unsigned short)pcm[i]);
		frames += n;
	}

	fseek(f, 0, SEEK_SET);
	writeWavHeader(f, info->channels, info->rate, frames);
	fclose(f);

	printf("%s: %lu frames\n", outputName, frames);

	free(pcm);
	free(outputName);
	return n == 0;
}

#else	/* __MINT__ */

/* DSP playback: zero-filled bytes after the end of the file to flush the DSP */
#define DSP_FLUSH_BYTES		2048

typedef struct {
	clock_t	fillTicks;
	clock_t	maxFillTicks;
	long	fills;
	long	underruns;
} Stats;

/*
 * Fill one half of the DMA buffer; whatever is not covered by the file
 * (once it has ended) must be filled with silence.
 * Returns the number of bytes taken from the file (0 = silence only),
 * -1 on error.
 */
typedef long (*FillFunc)(Decoder* d, char* dst, long bytes);

/* CPU playback: decode whole blocks into 16-bit stereo samples. */
static long fillDecoded(Decoder* d, char* dst, long bytes)
{
	const long frameBytes = 2 * sizeof(short);
	long frames = 0;

	while ((frames + d->info.blockFrames) * frameBytes <= bytes) {
		long n = decodeBlock(d, (short*)dst + frames * 2, 2);
		if (n < 0)
			return -1;
		if (n == 0)
			break;
		frames += n;
	}

	memset(dst + frames * frameBytes, 0, bytes - frames * frameBytes);
	return frames * frameBytes;
}

/* DSP playback: copy the file as it is (decoded by the DSP). */
static long fillRaw(Decoder* d, char* dst, long bytes)
{
	long n = fread(dst, 1, bytes, d->file);

	if (n < bytes) {
		if (ferror(d->file)) {
			fprintf(stderr, "read error\n");
			return -1;
		}
		memset(dst + n, 0, bytes - n);	/* decoded as silent blocks */
	}
	return n;
}

static long timedFill(FillFunc fill, Decoder* d, char* dst, long bytes, Stats* stats)
{
	clock_t start = clock();
	long n = fill(d, dst, bytes);

	start = clock() - start;
	stats->fillTicks += start;
	if (start > stats->maxFillTicks)
		stats->maxFillTicks = start;
	stats->fills++;

	return n;
}

/*
 * Current DMA play position. Buffptr() reads the position registers byte
 * by byte, so a carry between the reads can make it return a value off by
 * 256 bytes or more; read it until two consecutive reads are consistent.
 */
static char* playPosition(void)
{
	SndBufPtr first;
	SndBufPtr second;

	Buffptr(&first);
	for (;;) {
		Buffptr(&second);
		if ((char*)second.play >= (char*)first.play
			&& (char*)second.play - (char*)first.play < 64)
			return (char*)second.play;
		first = second;
	}
}

/*
 * Play a double buffer of 2 * halfBytes in ST-RAM, refilling each half by
 * fill() once the DMA has left it. The playback stops when the DMA has
 * played tailBytes of the first half which contains only silence.
 */
static int playBuffer(Decoder* d, FillFunc fill, long halfBytes, long tailBytes, Stats* stats)
{
	char* buffer;
	char* half[2];
	int silent[2];
	int playing = 0;
	int ok = 1;
	int i;

	buffer = (char*)Mxalloc(2 * halfBytes, MX_STRAM);
	if ((long)buffer <= 0) {
		fprintf(stderr, "not enough ST-RAM for %ld bytes\n", 2 * halfBytes);
		return 0;
	}
	half[0] = buffer;
	half[1] = buffer + halfBytes;

	for (i = 0; i < 2; i++) {
		long n = timedFill(fill, d, half[i], halfBytes, stats);
		if (n < 0) {
			Mfree(buffer);
			return 0;
		}
		silent[i] = n == 0;
	}

	Setbuffer(SR_PLAY, buffer, buffer + 2 * halfBytes);
	Buffoper(SB_PLA_ENA | SB_PLA_RPT);

	for (;;) {
		char* position;
		int current;

		if (Bconstat(2)) {
			Bconin(2);
			printf("stopped by key press\n");
			break;
		}

		position = playPosition();
		current = position < half[1] ? 0 : 1;

		if (silent[current] && position - half[current] >= tailBytes)
			break;

		if (current != playing) {
			/* the half which has just finished playing gets the next chunk */
			long n = timedFill(fill, d, half[playing], halfBytes, stats);
			if (n < 0) {
				ok = 0;
				break;
			}
			silent[playing] = n == 0;

			if ((playPosition() < half[1] ? 0 : 1) != current)
				stats->underruns++;

			playing = current;
		}
	}

	Buffoper(0);
	Mfree(buffer);
	return ok;
}

static void printStats(const Stats* stats, clock_t playTicks, long halfBytes, long halfMs, const char* what)
{
	printf("played %lu ms, %s: %lu ms total (%lu%% of the playback time)\n",
		(unsigned long)(playTicks * 1000 / CLOCKS_PER_SEC), what,
		(unsigned long)(stats->fillTicks * 1000 / CLOCKS_PER_SEC),
		playTicks > 0 ? (unsigned long)(stats->fillTicks * 100 / playTicks) : 0);
	printf("buffer: 2 x %ld B (%ld ms each), slowest refill: %lu ms, underruns: %ld\n",
		halfBytes, halfMs, (unsigned long)(stats->maxFillTicks * 1000 / CLOCKS_PER_SEC),
		stats->underruns);
}

static int playCpu(Decoder* d, int blocksPerChunk)
{
	const ApmInfo* info = &d->info;
	USoundSpec desired;
	USoundSpec obtained;
	USoundContext context;
	Stats stats;
	long halfBytes;
	clock_t start;
	int ok;

	memset(&obtained, 0, sizeof(obtained));
	desired.frequency = info->rate;
	desired.channels = 2;
	desired.format = USoundFormatSigned16MSB;
	desired.samples = 1024;

	if (!USoundInitXbios(&desired, &obtained, &context)) {
		fprintf(stderr, "cannot initialise the sound system\n");
		return 0;
	}

	if (obtained.format != USoundFormatSigned16MSB || obtained.channels != 2) {
		fprintf(stderr, "16-bit stereo playback is not supported\n");
		USoundDeinitXbios(&context);
		return 0;
	}
	if (obtained.frequency != info->rate)
		printf("warning: playing at %u Hz instead of %lu Hz\n", obtained.frequency, info->rate);

	halfBytes = info->blockFrames * blocksPerChunk * 2 * sizeof(short);
	memset(&stats, 0, sizeof(stats));

	start = clock();
	ok = playBuffer(d, fillDecoded, halfBytes, 0, &stats);
	start = clock() - start;

	printStats(&stats, start, halfBytes, info->blockFrames * blocksPerChunk * 1000 / info->rate,
		"decoding+reading");

	USoundDeinitXbios(&context);
	return ok;
}

/* Devconnect() prescaler of the 25.175 MHz clock for the given rate, -1 if none */
static int prescaleForRate(unsigned long rate)
{
	static const struct {
		unsigned long	rate;
		int				prescale;
	} rates[] = {
		{ 49170, CLK50K }, { 32780, CLK33K }, { 24585, CLK25K }, { 19668, CLK20K },
		{ 16390, CLK16K }, { 12292, CLK12K }, {  9834, CLK10K }, {  8195, CLK8K }
	};
	size_t i;

	for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		if (rates[i].rate == rate)
			return rates[i].prescale;
	}
	return -1;
}

static int playDsp(Decoder* d, int blocksPerChunk)
{
	const ApmInfo* info = &d->info;
	const int prescale = prescaleForRate(info->rate);
	Stats stats;
	long halfBytes;
	long halfMs;
	clock_t start;
	int ok;

	if (prescale < 0) {
		fprintf(stderr, "%lu Hz is not a Falcon frequency\n", info->rate);
		return 0;
	}

	if (Locksnd() != 1) {
		fprintf(stderr, "the sound system is locked\n");
		return 0;
	}
	if (Dsp_Lock() != 0) {
		fprintf(stderr, "the DSP is locked\n");
		Unlocksnd();
		return 0;
	}

	Sndstatus(SND_RESET);
	Soundcmd(ADDERIN, MATIN);
	Setmode(MODE_STEREO16);
	Dsptristate(DSP_ENABLE, DSP_ENABLE);
	/* the DSP reads the words at its own pace (handshake) */
	Devconnect(DMAPLAY, DSPRECV, CLK32M, prescale, HANDSHAKE);
	Devconnect(DSPXMIT, DAC, CLK25M, prescale, NO_SHAKE);

	/*
	 * Load the DSP program only after the matrix has been set up: the DSP
	 * requests the first word right away and changing the connection
	 * afterwards lets the DMA run without the handshake (at least in
	 * Hatari).
	 */
	Dsp_ExecProg((char*)dspdec_p56, dspdec_p56_len / 3, Dsp_RequestUniqueAbility());

	/* the DSP finds the stream start by the file header */
	fseek(d->file, 0, SEEK_SET);

	halfBytes = info->blockSize * blocksPerChunk;
	halfMs = (info->blockFrames * blocksPerChunk) * 1000 / info->rate;
	memset(&stats, 0, sizeof(stats));

	start = clock();
	ok = playBuffer(d, fillRaw, halfBytes,
		halfBytes < DSP_FLUSH_BYTES ? halfBytes : DSP_FLUSH_BYTES, &stats);
	start = clock() - start;

	printStats(&stats, start, halfBytes, halfMs, "reading");

	Sndstatus(SND_RESET);
	Dsptristate(DSP_TRISTATE, DSP_TRISTATE);
	Dsp_Unlock();
	Unlocksnd();
	return ok;
}

/* Keep the output on the screen when started from the desktop. */
static void waitForKey(void)
{
	printf("press any key\n");
	fflush(stdout);
	Bconin(2);
}

#endif	/* __MINT__ */

int main(int argc, char* argv[])
{
	Decoder decoder;
	int blocksPerChunk = DEFAULT_BLOCKS_PER_CHUNK;
	int useDsp = 0;
	const char* fileName = NULL;
	int ok;
	int i;

#ifdef __MINT__
	if (isatty(STDOUT_FILENO))
		atexit(waitForKey);
#endif

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
			blocksPerChunk = atoi(argv[++i]);
		else if (strcmp(argv[i], "-d") == 0)
			useDsp = 1;
		else if (fileName == NULL)
			fileName = argv[i];
		else
			fileName = NULL, i = argc;
	}

	if (fileName == NULL || blocksPerChunk < 1) {
		fprintf(stderr, "usage: %s [-c blocks per chunk] [-d] file.apm\n", argv[0]);
		return 1;
	}

	if (!decoderOpen(&decoder, fileName)) {
		decoderClose(&decoder);
		return 1;
	}

#ifndef __MINT__
	(void)blocksPerChunk;
	(void)useDsp;
	ok = decodeToWav(&decoder, fileName);
#else
	ok = useDsp ? playDsp(&decoder, blocksPerChunk) : playCpu(&decoder, blocksPerChunk);
#endif

	decoderClose(&decoder);
	return ok ? 0 : 1;
}
