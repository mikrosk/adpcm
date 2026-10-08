/*
 * APM decoder / player.
 *
 * Decodes IMA ADPCM files created by wav2apm.py (see its header for the
 * file layout) with Jack Jansen's adpcm_decoder(), one block at a time.
 *
 * Linux:  the decoded audio is written to <input>_decoded.wav.
 * Atari:  the decoded audio is played by the DMA sound from a double buffer
 *         in ST-RAM; the CPU decodes the next chunk into the half which has
 *         just finished playing. Statistics about the CPU load are printed
 *         at the end. Any key stops the playback.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adpcm.h"

#ifdef __MINT__
#include <mint/osbind.h>
#include <mint/falcon.h>
#include <time.h>
#include <usound.h>
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

typedef struct {
	clock_t	decodeTicks;
	clock_t	maxChunkTicks;
	long	chunks;
	long	underruns;
} Stats;

/*
 * Decode up to blocksPerChunk blocks into dst; the rest of the chunk
 * (if the file has ended) is filled with silence.
 * Returns the number of frames decoded, -1 on error.
 */
static long fillChunk(Decoder* d, short* dst, long chunkFrames, int outChannels, Stats* stats)
{
	long frames = 0;
	clock_t start = clock();

	while (frames < chunkFrames) {
		long n = decodeBlock(d, dst + frames * outChannels, outChannels);
		if (n < 0)
			return -1;
		if (n == 0)
			break;
		frames += n;
	}

	memset(dst + frames * outChannels, 0, (chunkFrames - frames) * outChannels * sizeof(short));

	start = clock() - start;
	stats->decodeTicks += start;
	if (start > stats->maxChunkTicks)
		stats->maxChunkTicks = start;
	stats->chunks++;

	return frames;
}

static int play(Decoder* d, int blocksPerChunk)
{
	const ApmInfo* info = &d->info;
	USoundSpec desired;
	USoundSpec obtained;
	USoundContext context;
	SndBufPtr pointers;
	Stats stats;
	char* buffer;
	short* half[2];
	int silent[2];
	long chunkFrames;
	long chunkBytes;
	unsigned long playedFrames = 0;
	clock_t startTicks;
	clock_t playTicks;
	int playing = 0;
	int ok = 1;
	int i;

	memset(&obtained, 0, sizeof(obtained));
	desired.frequency = info->rate;
	desired.channels = info->channels;
	desired.format = USoundFormatSigned16MSB;
	desired.samples = 1024;

	if (!USoundInitXbios(&desired, &obtained, &context)) {
		fprintf(stderr, "cannot initialise the sound system\n");
		return 0;
	}

	if (obtained.format != USoundFormatSigned16MSB) {
		fprintf(stderr, "signed 16-bit big endian samples are not supported\n");
		USoundDeinitXbios(&context);
		return 0;
	}
	if (obtained.frequency != info->rate)
		printf("warning: playing at %u Hz instead of %lu Hz\n", obtained.frequency, info->rate);

	chunkFrames = info->blockFrames * blocksPerChunk;
	chunkBytes = chunkFrames * obtained.channels * sizeof(short);

	buffer = (char*)Mxalloc(2 * chunkBytes, MX_STRAM);
	if ((long)buffer <= 0) {
		fprintf(stderr, "not enough ST-RAM for %ld bytes\n", 2 * chunkBytes);
		USoundDeinitXbios(&context);
		return 0;
	}
	half[0] = (short*)buffer;
	half[1] = (short*)(buffer + chunkBytes);

	printf("%d ch, %lu Hz, %lu frames (%lu s), block %ld B (%ld frames), "
		"chunk %d block(s) = %ld B in ST-RAM x 2\n",
		info->channels, info->rate, info->samples, info->samples / info->rate,
		info->blockSize, info->blockFrames, blocksPerChunk, chunkBytes);
	fflush(stdout);

	memset(&stats, 0, sizeof(stats));

	for (i = 0; i < 2; i++) {
		long n = fillChunk(d, half[i], chunkFrames, obtained.channels, &stats);
		if (n < 0) {
			ok = 0;
			goto done;
		}
		silent[i] = n == 0;
		playedFrames += n;
	}

	Setbuffer(SR_PLAY, buffer, buffer + 2 * chunkBytes);
	Buffoper(SB_PLA_ENA | SB_PLA_RPT);
	startTicks = clock();

	while (!silent[playing]) {
		int current;

		if (Bconstat(2)) {
			Bconin(2);
			printf("stopped by key press\n");
			break;
		}

		Buffptr(&pointers);
		current = (char*)pointers.play < (char*)half[1] ? 0 : 1;

		if (current != playing) {
			/* the half which has just finished playing gets the next chunk */
			long n = fillChunk(d, half[playing], chunkFrames, obtained.channels, &stats);
			if (n < 0) {
				ok = 0;
				break;
			}
			silent[playing] = n == 0;
			playedFrames += n;

			Buffptr(&pointers);
			if (((char*)pointers.play < (char*)half[1] ? 0 : 1) != current)
				stats.underruns++;

			playing = current;
		}
	}

	playTicks = clock() - startTicks;
	Buffoper(0);

	printf("decoded %lu frames in %ld chunks, played %lu ms\n",
		playedFrames, stats.chunks, (unsigned long)(playTicks * 1000 / CLOCKS_PER_SEC));
	printf("decoding+reading: %lu ms total, %lu%% of the playback time\n",
		(unsigned long)(stats.decodeTicks * 1000 / CLOCKS_PER_SEC),
		playTicks > 0 ? (unsigned long)(stats.decodeTicks * 100 / playTicks) : 0);
	printf("slowest chunk: %lu ms (one chunk plays %ld ms), underruns: %ld\n",
		(unsigned long)(stats.maxChunkTicks * 1000 / CLOCKS_PER_SEC),
		chunkFrames * 1000 / info->rate, stats.underruns);

done:
	Mfree(buffer);
	USoundDeinitXbios(&context);
	return ok;
}

#endif	/* __MINT__ */

int main(int argc, char* argv[])
{
	Decoder decoder;
	int blocksPerChunk = DEFAULT_BLOCKS_PER_CHUNK;
	const char* fileName = NULL;
	int ok;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
			blocksPerChunk = atoi(argv[++i]);
		else if (fileName == NULL)
			fileName = argv[i];
		else
			fileName = NULL, i = argc;
	}

	if (fileName == NULL || blocksPerChunk < 1) {
		fprintf(stderr, "usage: %s [-c blocks per chunk] file.apm\n", argv[0]);
		return 1;
	}

	if (!decoderOpen(&decoder, fileName)) {
		decoderClose(&decoder);
		return 1;
	}

#ifndef __MINT__
	(void)blocksPerChunk;
	ok = decodeToWav(&decoder, fileName);
#else
	ok = play(&decoder, blocksPerChunk);
#endif

	decoderClose(&decoder);
	return ok ? 0 : 1;
}
