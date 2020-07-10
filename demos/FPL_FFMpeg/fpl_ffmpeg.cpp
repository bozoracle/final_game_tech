/*
-------------------------------------------------------------------------------
Name:
	FPL-Demo | FFmpeg

Description:
	Custom FFmpeg Media Player Demo based on FPL and ffplay.c

Requirements:
	- C++/11 Compiler
	- Platform x64 / Win32
	- Final Platform Layer
	- FFmpeg-4.2.2-win64-shared (http://ffmpeg.zeranoe.com/builds/)
	- FFmpeg-4.2.2-win64-dev (http://ffmpeg.zeranoe.com/builds/)

Author:
	Torsten Spaete

Changelog:
	## 2020-04-22
	- Relative Seeking Support
	- OSD for displaying media/stream informations
	- Check for versions for FFMPEG libraries

	## 2020-03-02
	- Upgraded to FFmpeg 4.2.2

	## 2019-04-27
	- Small changes (Comments, Constants)

	## 2018-10-22
	- Reflect api changes in FPL 0.9.3

	## 2018-09-24
	- Changed all inline functions to static
	- Reflect api changes in FPL 0.9.2

	## 2018-06-29
	- Changed to use new keyboard/mouse button state

	## 2018-06-06
	- Refactored files
	- Upgraded to FFMPEG 4.0

	## 2018-05-15:
	- Fixed compile errors
	- Added USE_FLIP_V_PICTURE_IN_SOFTWARE

	## 2018-04-25:
	- Fixed compile errors after FGL has changed to C99

	## 2018-04-23:
	- Initial creation of this description block
	- Forced Visual-Studio-Project to compile in C++ always

Issues:
	- Black flickering in software rendering mode (FPL has not double buffering support for software rendering)
	- Frame drops for a couple of seconds when doing seeking (Need to flush all the queues or something)
	- Defines in mix everywhere, making it hard to implement other systems or switch from software <-> hardware (Need a rewrite)

Features/Planned:
	[x] Reads packets from stream and queues them up
	[x] Decodes video and audio packets and queues them up as well
	[x] FFmpeg functions are loaded dynamically
	[x] Linked list for packet queue
	[x] Handle PTS/DTS to schedule video frame
	[x] Syncronize video to audio
	[x] Fix memory leak (There was no leak at all)
	[x] Support for FFmpeg static linking
	[x] Rewrite Frame Queue to support Peek in Previous, Current and Next frame
	[x] Introduce serials
	[x] Introduce null and flush packet
	[x] Restart
	[x] Frame dropping using prev/next frame
	[x] Pause/Resume
	[x] OpenGL Video Rendering
	[x] Syncronize audio to video
	[x] Use same FFmpeg name from every dynamic function
	[x] Fix bug for WMV always dropping nearly every frame (TimeBase was wrong all the time)
	[x] Aspect ratio calculation
	[x] Fullscreen toggling
	[x] Modern OpenGL 3.3
	[x] OSD
	[x] Seeking (+/- 5 secs)
	[ ] Frame by frame stepping
	[ ] Support for audio format change while playing
	[ ] Support for video format change while playing
	[x] Image format conversion (YUY2, YUV > RGB24 etc.)
		[x] GLSL (YUV420P for now)
		[x] Slow CPU implementation (YUV420P for now)
		[ ] SIMD YUV420P implementation
	[ ] Audio format conversion (Downsampling, Upsampling, S16 > F32 etc.)
		[ ] Slow CPU implementation
		[ ] SIMD implementation
		[ ] Planar (Non-interleaved) samples to (Interleaved) Non-planar samples conversion
	[ ] Composite video rendering
		[ ] Bitmap rect blitting
		[ ] Subtitle Decoding and Compositing

Resources:
	- http://dranger.com/ffmpeg/tutorial01.html
	- https://blogs.gentoo.org/lu_zero/2015/10/15/deprecating-avpicture/
	- https://blogs.gentoo.org/lu_zero/2016/03/29/new-avcodec-api/
	- https://www.codeproject.com/tips/489450/creating-custom-ffmpeg-io-context

License:
	Copyright (c) 2017-2020 Torsten Spaete
	MIT License (See LICENSE file)
-------------------------------------------------------------------------------
*/

#define FPL_IMPLEMENTATION
#define FPL_LOGGING
#define FPL_LOG_TO_DEBUGOUT
#include <final_platform_layer.h>

#include <assert.h> // assert

#include "defines.h"
#include "utils.h"
#include "maths.h"
#include "ffmpeg.h"

#include "fontdata.h" // sulphur-point-regular font

typedef int32_t bool32;

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#if USE_HARDWARE_RENDERING
#	define FGL_IMPLEMENTATION
#	include <final_dynamic_opengl.h>
#	include "shaders.h"

struct VideoShader {
	GLuint programId;
	GLuint uniform_uniProjMat;
	GLuint uniform_uniTextures;
	GLuint uniform_uniTextureScaleY;
	GLuint uniform_uniTextureOffsetY;
};

static char glErrorCodeBuffer[16];
static const char *GetGLErrorString(const GLenum err) {
	switch (err) {
		case GL_INVALID_ENUM:
			return "GL_INVALID_ENUM";
		case GL_INVALID_VALUE:
			return "GL_INVALID_VALUE";
		case GL_INVALID_OPERATION:
			return "GL_INVALID_OPERATION";
		case GL_STACK_OVERFLOW:
			return "GL_STACK_OVERFLOW";
		case GL_STACK_UNDERFLOW:
			return "GL_STACK_UNDERFLOW";
		case GL_OUT_OF_MEMORY:
			return "GL_OUT_OF_MEMORY";
		default:
			if (_itoa_s(err, glErrorCodeBuffer, fplArrayCount(glErrorCodeBuffer), 10) == 0)
				return (const char *)glErrorCodeBuffer;
			else
				return "";
	}
}

static void CheckGLError() {
	GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		const char *msg = GetGLErrorString(err);
		assert(!msg);
	}
}

static GLuint CompileShader(GLuint type, const char *source, const char *name) {
	GLuint result = glCreateShader(type);
	glShaderSource(result, 1, &source, nullptr);
	glCompileShader(result);
	int compileStatus;
	glGetShaderiv(result, GL_COMPILE_STATUS, &compileStatus);
	if (compileStatus == GL_FALSE) {
		int length;
		glGetShaderiv(result, GL_INFO_LOG_LENGTH, &length);
		char *message = (char *)fplStackAllocate(length * sizeof(char));
		glGetShaderInfoLog(result, length, &length, message);
		FPL_LOG_ERROR("App", "Failed to compile %s shader '%s':\n%s\n", (type == GL_VERTEX_SHADER ? "vertex" : "fragment"), name, message);
		glDeleteShader(result);
		return 0;
	}
	return(result);
}

static GLuint CreateShader(const char *vertexShaderSource, const char *fragmentShaderSource, const char *name) {
	GLuint result = glCreateProgram();
	GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexShaderSource, name);
	GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource, name);
	if (vs == 0 || fs == 0) {
		glDeleteProgram(result);
		return 0;
	}
	glAttachShader(result, vs);
	glAttachShader(result, fs);
	glDeleteShader(fs);
	glDeleteShader(vs);
	glLinkProgram(result);

	int linkStatus;
	glGetProgramiv(result, GL_LINK_STATUS, &linkStatus);
	if (GL_LINK_STATUS == GL_FALSE) {
		int length;
		glGetProgramiv(result, GL_INFO_LOG_LENGTH, &length);
		char *message = (char *)fplStackAllocate(length * sizeof(char));
		glGetProgramInfoLog(result, length, &length, message);
		FPL_LOG_ERROR("App", "Failed to link %s shader program:\n%s\n", name, message);
		glDeleteProgram(result);
		return 0;
	}

	glValidateProgram(result);
	return(result);
}

static bool LoadVideoShader(VideoShader &shader, const char *vertexSource, const char *fragSource, const char *name) {
	shader.programId = CreateShader(vertexSource, fragSource, name);
	shader.uniform_uniProjMat = glGetUniformLocation(shader.programId, "uniProjMat");
	shader.uniform_uniTextures = glGetUniformLocation(shader.programId, "uniTextures");
	shader.uniform_uniTextureScaleY = glGetUniformLocation(shader.programId, "uniTextureScaleY");
	shader.uniform_uniTextureOffsetY = glGetUniformLocation(shader.programId, "uniTextureOffsetY");
	return true;
}
#endif

static FFMPEGContext ffmpeg = {};

//
// Stats
//
struct MemoryStats {
	volatile int32_t allocatedPackets;
	volatile int32_t usedPackets;
	volatile int32_t allocatedFrames;
	volatile int32_t usedFrames;
};

static MemoryStats globalMemStats = {};

static void PrintMemStats() {
	int32_t allocatedPackets = fplAtomicLoadS32(&globalMemStats.allocatedPackets);
	int32_t usedPackets = fplAtomicLoadS32(&globalMemStats.usedPackets);
	int32_t allocatedFrames = fplAtomicLoadS32(&globalMemStats.allocatedFrames);
	int32_t usedFrames = fplAtomicLoadS32(&globalMemStats.usedFrames);
	fplDebugFormatOut("Packets: %d / %d, Frames: %d / %d\n", allocatedPackets, usedPackets, allocatedFrames, usedFrames);
}

//
// Constants
//

// Max number of frames in the queues
constexpr uint32_t MAX_VIDEO_FRAME_QUEUE_COUNT = 4;
constexpr uint32_t MAX_AUDIO_FRAME_QUEUE_COUNT = 8;
constexpr uint32_t MAX_FRAME_QUEUE_COUNT = fplMax(MAX_AUDIO_FRAME_QUEUE_COUNT, MAX_VIDEO_FRAME_QUEUE_COUNT);

// Total size of data from all packet queues
constexpr uint64_t MAX_PACKET_QUEUE_SIZE = fplMegaBytes(16);

// Min number of packet frames in a single queue
constexpr uint32_t MIN_PACKET_FRAMES = 25;

// External clock min/max frames
constexpr uint32_t EXTERNAL_CLOCK_MIN_FRAMES = 2;
constexpr uint32_t EXTERNAL_CLOCK_MAX_FRAMES = 10;

// External clock speed adjustment constants for realtime sources based on buffer fullness
constexpr double EXTERNAL_CLOCK_SPEED_MIN = 0.900;
constexpr double EXTERNAL_CLOCK_SPEED_MAX = 1.010;
constexpr double EXTERNAL_CLOCK_SPEED_STEP = 0.001;

// No AV sync correction is done if below the minimum AV sync threshold
constexpr double AV_SYNC_THRESHOLD_MIN = 0.04;
// No AV sync correction is done if above the maximum AV sync threshold
constexpr double AV_SYNC_THRESHOLD_MAX = 0.1;
// No AV correction is done if too big error
constexpr double AV_NOSYNC_THRESHOLD = 10.0;
// If a frame duration is longer than this, it will not be duplicated to compensate AV sync
constexpr double AV_SYNC_FRAMEDUP_THRESHOLD = 0.1;
// Default refresh rate of 1/sec
constexpr double DEFAULT_REFRESH_RATE = 0.01;
// Number of audio samples required to make an average.
constexpr int AV_AUDIO_DIFF_AVG_NB = 20;
// Maximum audio speed change to get correct sync
constexpr int AV_SAMPLE_CORRECTION_PERCENT_MAX = 10;

//
// Packet Queue
//
static AVPacket globalFlushPacket = {};

struct PacketList {
	AVPacket packet;
	PacketList *next;
	int32_t serial;
};

struct PacketQueue {
	fplMutexHandle lock;
	fplSignalHandle addedSignal;
	fplSignalHandle freeSignal;
	PacketList *first;
	PacketList *last;
	uint64_t size;
	uint64_t duration;
	int32_t packetCount;
	int32_t serial;
};

static bool IsFlushPacket(PacketList *packet) {
	assert(packet != nullptr);
	bool result = (packet->packet.data == (uint8_t *)&globalFlushPacket);
	return(result);
}

static PacketList *AllocatePacket(PacketQueue &queue) {
	PacketList *packet = (PacketList *)ffmpeg.av_mallocz(sizeof(PacketList));
	if (packet == nullptr) {
		return nullptr;
	}
	fplAtomicFetchAndAddS32(&globalMemStats.allocatedPackets, 1);
	return(packet);
}

static void DestroyPacket(PacketQueue &queue, PacketList *packet) {
	ffmpeg.av_freep(packet);
	fplAtomicFetchAndAddS32(&globalMemStats.allocatedPackets, -1);
}

static void ReleasePacketData(PacketList *packet) {
	if (!IsFlushPacket(packet)) {
		ffmpeg.av_packet_unref(&packet->packet);
	}
}

static void ReleasePacket(PacketQueue &queue, PacketList *packet) {
	ReleasePacketData(packet);
	DestroyPacket(queue, packet);
	fplSignalSet(&queue.freeSignal);
}

static bool AquirePacket(PacketQueue &queue, PacketList *&packet) {
	bool result = false;
	packet = AllocatePacket(queue);
	if (packet != nullptr) {
		result = true;
	}
	return(result);
}

static void FlushPacketQueue(PacketQueue &queue) {
	fplMutexLock(&queue.lock);
	PacketList *p = queue.first;
	while (p != nullptr) {
		PacketList *n = p->next;
		ReleasePacketData(p);
		DestroyPacket(queue, p);
		p = n;
	}
	queue.first = queue.last = nullptr;
	queue.packetCount = 0;
	queue.size = 0;
	queue.duration = 0;
	fplMutexUnlock(&queue.lock);
}

static void DestroyPacketQueue(PacketQueue &queue) {
	FlushPacketQueue(queue);
	fplSignalDestroy(&queue.freeSignal);
	fplSignalDestroy(&queue.addedSignal);
	fplMutexDestroy(&queue.lock);
}

static bool InitPacketQueue(PacketQueue &queue) {
	if (!fplMutexInit(&queue.lock)) {
		return false;
	}
	if (!fplSignalInit(&queue.addedSignal, fplSignalValue_Unset)) {
		return false;
	}
	if (!fplSignalInit(&queue.freeSignal, fplSignalValue_Unset)) {
		return false;
	}
	return true;
}

static void PushPacket(PacketQueue &queue, PacketList *packet) {
	fplMutexLock(&queue.lock);
	{
		packet->next = nullptr;
		if (IsFlushPacket(packet)) {
			queue.serial++;
		}
		packet->serial = queue.serial;
		if (queue.first == nullptr) {
			queue.first = packet;
		}
		if (queue.last != nullptr) {
			assert(queue.last->next == nullptr);
			queue.last->next = packet;
		}
		queue.last = packet;
		queue.size += packet->packet.size + sizeof(*packet);
		queue.duration += packet->packet.duration;
		fplAtomicFetchAndAddS32(&queue.packetCount, 1);
		fplAtomicFetchAndAddS32(&globalMemStats.usedPackets, 1);
		fplSignalSet(&queue.addedSignal);
	}
	fplMutexUnlock(&queue.lock);
}

static bool PopPacket(PacketQueue &queue, PacketList *&packet) {
	bool result = false;
	fplMutexLock(&queue.lock);
	{
		if (queue.first != nullptr) {
			PacketList *p = queue.first;
			PacketList *n = p->next;
			queue.first = n;
			p->next = nullptr;
			packet = p;
			queue.duration -= packet->packet.duration;
			queue.size -= packet->packet.size + sizeof(*packet);
			if (queue.first == nullptr) {
				queue.last = nullptr;
			}
			fplAtomicFetchAndAddS32(&queue.packetCount, -1);
			fplAtomicFetchAndAddS32(&globalMemStats.usedPackets, -1);
			result = true;
		}
	}
	fplMutexUnlock(&queue.lock);
	return(result);
}

static bool PushNullPacket(PacketQueue &queue, int streamIndex) {
	bool result = false;
	PacketList *packet = nullptr;
	if (AquirePacket(queue, packet)) {
		ffmpeg.av_init_packet(&packet->packet);
		packet->packet.data = nullptr;
		packet->packet.size = 0;
		packet->packet.stream_index = streamIndex;
		PushPacket(queue, packet);
		result = true;
	}
	return(result);
}

static bool PushFlushPacket(PacketQueue &queue) {
	bool result = false;
	PacketList *packet = nullptr;
	if (AquirePacket(queue, packet)) {
		packet->packet = globalFlushPacket;
		PushPacket(queue, packet);
		result = true;
	}
	return(result);
}

static void StartPacketQueue(PacketQueue &queue) {
	fplMutexLock(&queue.lock);
	assert(PushFlushPacket(queue));
	fplMutexUnlock(&queue.lock);
}

//
// Frame Queue
//
struct Frame {
	AVRational sar;
	AVFrame *frame;
	double pts;
	double duration;
	int64_t pos;
	int32_t serial;
	int32_t width;
	int32_t height;
	bool32 isUploaded;
};

static AVFrame *AllocateFrame() {
	AVFrame *result = ffmpeg.av_frame_alloc();
	fplAtomicFetchAndAddS32(&globalMemStats.allocatedFrames, 1);
	return(result);
}

static void FreeFrameData(Frame *frame) {
	ffmpeg.av_frame_unref(frame->frame);
}

static void FreeFrame(Frame *frame) {
	FreeFrameData(frame);
	ffmpeg.av_frame_free(&frame->frame);
}

struct FrameQueue {
	Frame frames[MAX_FRAME_QUEUE_COUNT];
	fplMutexHandle lock;
	fplSignalHandle signal;
	PacketList *pendingPacket;
	volatile uint32_t *stopped;
	int32_t readIndex;
	int32_t writeIndex;
	int32_t count;
	int32_t capacity;
	int32_t keepLast;
	int32_t readIndexShown;
	bool32 isValid;
	bool32 hasPendingPacket;
};

static bool InitFrameQueue(FrameQueue &queue, int32_t capacity, volatile uint32_t *stopped, int32_t keepLast) {
	queue = {};
	queue.capacity = fplMin(capacity, MAX_FRAME_QUEUE_COUNT);
	for (int32_t i = 0; i < queue.capacity; ++i) {
		Frame *frame = queue.frames + i;
		frame->frame = AllocateFrame();
		if (frame->frame == nullptr) {
			return false;
		}
	}

	queue.keepLast = !!keepLast;
	queue.stopped = stopped;

	if (!fplMutexInit(&queue.lock)) {
		return false;
	}
	if (!fplSignalInit(&queue.signal, fplSignalValue_Unset)) {
		return false;
	}

	queue.isValid = true;
	return true;
}

static void DestroyFrameQueue(FrameQueue &queue) {
	fplSignalDestroy(&queue.signal);
	fplMutexDestroy(&queue.lock);
	for (int64_t i = 0; i < queue.capacity; ++i) {
		Frame *frame = queue.frames + i;
		FreeFrame(frame);
	}
}

static Frame *PeekFrameQueue(FrameQueue &queue) {
	return &queue.frames[(queue.readIndex + queue.readIndexShown) % queue.capacity];
}

static Frame *PeekFrameQueueNext(FrameQueue &queue) {
	return &queue.frames[(queue.readIndex + queue.readIndexShown + 1) % queue.capacity];
}

static Frame *PeekFrameQueueLast(FrameQueue &queue) {
	return &queue.frames[queue.readIndex];
}

static bool PeekWritableFromFrameQueue(FrameQueue &queue, Frame *&frame) {
	fplMutexLock(&queue.lock);
	if (queue.count >= queue.capacity || *queue.stopped) {
		fplMutexUnlock(&queue.lock);
		return false;
	}
	fplMutexUnlock(&queue.lock);

	if (*queue.stopped) {
		return false;
	}

	frame = &queue.frames[queue.writeIndex];
	return true;
}

static bool PeekReadableFromFrameQueue(FrameQueue &queue, Frame *&frame) {
	fplMutexLock(&queue.lock);
	if ((queue.count - queue.readIndexShown) <= 0 || *queue.stopped) {
		fplMutexUnlock(&queue.lock);
		return false;
	}
	fplMutexUnlock(&queue.lock);

	if (*queue.stopped) {
		return false;
	}

	frame = &queue.frames[(queue.readIndex + queue.readIndexShown) % queue.capacity];
	return true;
}

static void NextWritable(FrameQueue &queue) {
	queue.writeIndex = (queue.writeIndex + 1) % queue.capacity;

	fplMutexLock(&queue.lock);
	queue.count++;
	fplSignalSet(&queue.signal);
	fplMutexUnlock(&queue.lock);
}

static void NextReadable(FrameQueue &queue) {
	if (queue.keepLast && !queue.readIndexShown) {
		queue.readIndexShown = 1;
		return;
	}

	FreeFrameData(&queue.frames[queue.readIndex]);
	queue.readIndex = (queue.readIndex + 1) % queue.capacity;

	fplMutexLock(&queue.lock);
	queue.count--;
	fplSignalSet(&queue.signal);
	fplMutexUnlock(&queue.lock);
}

static int32_t GetFrameQueueRemainingCount(const FrameQueue &queue) {
	return queue.count - queue.readIndexShown;
}

//
// Media
//
struct MediaStream {
	AVStream *stream;
	AVCodecContext *codecContext;
	AVCodec *codec;
	int32_t streamIndex;
	bool32 isValid;
};

struct ReaderContext {
	PacketQueue packetQueue;
	fplMutexHandle lock;
	fplSignalHandle stopSignal;
	fplSignalHandle resumeSignal;
	fplThreadHandle *thread;
	volatile uint32_t readPacketCount;
	volatile uint32_t stopRequest;
	bool32 isEOF;
};

static bool InitReader(ReaderContext &outReader) {
	outReader = {};
	if (!fplMutexInit(&outReader.lock)) {
		return false;
	}
	if (!fplSignalInit(&outReader.stopSignal, fplSignalValue_Unset)) {
		return false;
	}
	if (!fplSignalInit(&outReader.resumeSignal, fplSignalValue_Unset)) {
		return false;
	}
	if (!InitPacketQueue(outReader.packetQueue)) {
		return false;
	}
	return true;
}

static void DestroyReader(ReaderContext &reader) {
	DestroyPacketQueue(reader.packetQueue);
	fplSignalDestroy(&reader.resumeSignal);
	fplSignalDestroy(&reader.stopSignal);
	fplMutexDestroy(&reader.lock);
}

static void StopReader(ReaderContext &reader) {
	reader.stopRequest = 1;
	fplSignalSet(&reader.stopSignal);
	fplThreadWaitForOne(reader.thread, FPL_TIMEOUT_INFINITE);
	fplThreadTerminate(reader.thread);
	reader.thread = nullptr;
}

static void StartReader(ReaderContext &reader, fpl_run_thread_callback *readerThreadFunc, void *state) {
	reader.stopRequest = 0;
	assert(reader.thread == nullptr);
	reader.thread = fplThreadCreate(readerThreadFunc, state);
}

//
// Decoder
//
struct PlayerState;
struct Decoder {
	PacketQueue packetsQueue;
	FrameQueue frameQueue;
	fplMutexHandle lock;
	fplSignalHandle stopSignal;
	fplSignalHandle resumeSignal;
	fplThreadHandle *thread;
	PlayerState *state;
	ReaderContext *reader;
	MediaStream *stream;
	int64_t start_pts;
	AVRational start_pts_tb;
	int64_t next_pts;
	AVRational next_pts_tb;
	volatile uint32_t stopRequest;
	volatile uint32_t isEOF;
	volatile uint32_t decodedFrameCount;
	int32_t pktSerial;
	int32_t finishedSerial;
};

static bool InitDecoder(Decoder &outDecoder, PlayerState *state, ReaderContext *reader, MediaStream *stream, uint32_t frameCapacity, int32_t keepLast) {
	outDecoder = {};
	outDecoder.stream = stream;
	outDecoder.reader = reader;
	outDecoder.state = state;
	outDecoder.pktSerial = -1;
	outDecoder.start_pts = AV_NOPTS_VALUE;
	if (!fplMutexInit(&outDecoder.lock)) {
		return false;
	}
	if (!fplSignalInit(&outDecoder.stopSignal, fplSignalValue_Unset)) {
		return false;
	}
	if (!fplSignalInit(&outDecoder.resumeSignal, fplSignalValue_Unset)) {
		return false;
	}
	if (!InitPacketQueue(outDecoder.packetsQueue)) {
		return false;
	}
	if (!InitFrameQueue(outDecoder.frameQueue, frameCapacity, &outDecoder.stopRequest, keepLast)) {
		return false;
	}

	return true;
}

static void DestroyDecoder(Decoder &decoder) {
	DestroyFrameQueue(decoder.frameQueue);
	DestroyPacketQueue(decoder.packetsQueue);
	fplSignalDestroy(&decoder.resumeSignal);
	fplSignalDestroy(&decoder.stopSignal);
	fplMutexDestroy(&decoder.lock);
}

static void StartDecoder(Decoder &decoder, fpl_run_thread_callback *decoderThreadFunc) {
	StartPacketQueue(decoder.packetsQueue);
	assert(decoder.thread == nullptr);
	decoder.thread = fplThreadCreate(decoderThreadFunc, &decoder);
}

static void StopDecoder(Decoder &decoder) {
	decoder.stopRequest = 1;
	fplSignalSet(&decoder.stopSignal);
	fplThreadWaitForOne(decoder.thread, FPL_TIMEOUT_INFINITE);
	fplThreadTerminate(decoder.thread);
	decoder.thread = nullptr;
	FlushPacketQueue(decoder.packetsQueue);
}

static void AddPacketToDecoder(Decoder &decoder, PacketList *targetPacket, AVPacket *sourcePacket) {
	targetPacket->packet = *sourcePacket;
	PushPacket(decoder.packetsQueue, targetPacket);
}

//
// Clock
//
struct Clock {
	double pts;
	double ptsDrift;
	double lastUpdated;
	double speed;
	int32_t *queueSerial;
	int32_t serial;
	bool32 isPaused;
};
enum class AVSyncType {
	AudioMaster,
	VideoMaster,
	ExternalClock,
};

static double GetClock(const Clock &clock) {
	if (*clock.queueSerial != clock.serial) {
		return NAN;
	}
	double result;
	if (clock.isPaused) {
		result = clock.pts;
	} else {
		double time = ffmpeg.av_gettime_relative() / (double)AV_TIME_BASE;
		result = clock.ptsDrift + time - (time - clock.lastUpdated) * (1.0 - clock.speed);
	}
	return(result);
}

static void SetClockAt(Clock &clock, const double pts, const int32_t serial, const double time) {
	clock.pts = pts;
	clock.lastUpdated = time;
	clock.ptsDrift = clock.pts - time;
	clock.serial = serial;
}

static void SetClock(Clock &clock, const double pts, const int32_t serial) {
	double time = ffmpeg.av_gettime_relative() / (double)AV_TIME_BASE;
	SetClockAt(clock, pts, serial, time);
}

static void SetClockSpeed(Clock &clock, const double speed) {
	SetClock(clock, GetClock(clock), clock.serial);
	clock.speed = speed;
}

static void InitClock(Clock &clock, int32_t *queueSerial) {
	clock.speed = 1.0;
	clock.isPaused = false;
	clock.queueSerial = queueSerial;
	SetClock(clock, NAN, -1);
}

static void SyncClockToSlave(Clock &c, const Clock &slave) {
	double clock = GetClock(c);
	double slaveClock = GetClock(slave);
	if (!isnan(slaveClock) && (isnan(clock) || fabs(clock - slaveClock) > AV_NOSYNC_THRESHOLD)) {
		SetClock(c, slaveClock, slave.serial);
	}
}

//
// Video
//
struct VideoTexture {
#if USE_HARDWARE_RENDERING
	GLuint id;
	GLuint pboId;
	GLuint target;
	GLint internalFormat;
	GLenum format;
#	if !USE_GL_PBO
	uint8_t *data;
#	endif
#else
	uint32_t id;
#endif
	uint32_t width;
	uint32_t height;
	uint32_t pixelSize;
	int32_t rowSize;
	uint32_t colorBits;
};

static bool InitVideoTexture(VideoTexture &texture, const uint32_t w, const uint32_t h, const uint32_t colorBits) {
	texture.width = w;
	texture.height = h;
	texture.colorBits = colorBits;

	int colorComponents = colorBits / 8;

	texture.pixelSize = colorComponents * sizeof(uint8_t);
	texture.rowSize = w * texture.pixelSize;

#if USE_HARDWARE_RENDERING
	size_t dataSize = texture.rowSize * texture.height;

#	if USE_GL_PBO
	glGenBuffers(1, &texture.pboId);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture.pboId);
	glBufferData(GL_PIXEL_UNPACK_BUFFER, dataSize, 0, GL_STREAM_DRAW);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
#	else
	texture.data = (uint8_t *)fplMemoryAllocate(dataSize);
#	endif

#if USE_GL_RECTANGLE_TEXTURES
	texture.target = GL_TEXTURE_RECTANGLE;
#else
	texture.target = GL_TEXTURE_2D;
#endif

	texture.internalFormat = GL_RGBA8;
	texture.format = GL_RGBA;
	if (colorComponents == 1) {
		texture.internalFormat = GL_R8;
		texture.format = GL_RED;
	}

	glGenTextures(1, &texture.id);
	glBindTexture(texture.target, texture.id);
	glTexImage2D(texture.target, 0, texture.internalFormat, w, h, 0, texture.format, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(texture.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(texture.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(texture.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(texture.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(texture.target, 0);
	CheckGLError();
#else
	texture.id = 1;
	fplResizeVideoBackBuffer(w, h);
#endif

	return true;
}

static uint8_t *LockVideoTexture(VideoTexture &texture) {
	uint8_t *result;

#if USE_HARDWARE_RENDERING
#	if USE_GL_PBO
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture.pboId);
	result = (uint8_t *)glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
	CheckGLError();
#	else
	result = texture.data;
#	endif
#else
	fplVideoBackBuffer *backBuffer = fplGetVideoBackBuffer();
	result = (uint8_t *)backBuffer->pixels;
#endif

	return(result);
}

static void UnlockVideoTexture(VideoTexture &texture) {
#if USE_HARDWARE_RENDERING
#	if USE_GL_PBO
	glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
	glBindTexture(texture.target, texture.id);
	glTexSubImage2D(texture.target, 0, 0, 0, texture.width, texture.height, texture.format, GL_UNSIGNED_BYTE, (void *)0);
	glBindTexture(texture.target, 0);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	CheckGLError();
#	else
	glBindTexture(texture.target, texture.id);
	glTexSubImage2D(texture.target, 0, 0, 0, texture.width, texture.height, texture.format, GL_UNSIGNED_BYTE, texture.data);
	glBindTexture(texture.target, 0);
#	endif
#endif
}

static void DestroyVideoTexture(VideoTexture &texture) {
#if USE_HARDWARE_RENDERING
#	if !USE_GL_PBO
	fplMemoryFree(texture.data);
#	endif
	glDeleteTextures(1, &texture.id);
#	if USE_GL_PBO
	glDeleteBuffers(1, &texture.pboId);
#	endif
#endif

	texture = {};
}

static uint16_t videoQuadIndices[6] = {
	0, 1, 2,
	2, 3, 0,
};

constexpr uint32_t MAX_TARGET_TEXTURE_COUNT = 4;
struct VideoContext {
	MediaStream stream;
	Decoder decoder;
	Clock clock;
	VideoTexture targetTextures[MAX_TARGET_TEXTURE_COUNT];

#if USE_HARDWARE_RENDERING
	VideoShader basicShader;
	VideoShader yuv420pShader;
	GLuint vao;
	GLuint vertexBufferId;
	GLuint indexBufferId;
	VideoShader *activeShader;
#endif

	SwsContext *softwareScaleCtx;
	uint32_t targetTextureCount;
};

static void FlipSourcePicture(uint8_t *srcData[8], int srcLineSize[8], int height) {
	int h0 = srcLineSize[0];
	for (int i = 0; i < 8; ++i) {
		int hi = srcLineSize[i];
		if (hi == 0) {
			break;
		}
		int h;
		if (hi != h0) {
			int div = h0 / hi;
			h = (height / div) - 1;
		} else {
			h = height - 1;
		}
		srcData[i] = srcData[i] + srcLineSize[i] * h;
		srcLineSize[i] = -srcLineSize[i];
	}

}

static void UploadTexture(VideoContext &video, const AVFrame *sourceNativeFrame) {
	AVCodecContext *videoCodecCtx = video.stream.codecContext;
#if USE_HARDWARE_RENDERING && USE_GLSL_IMAGE_FORMAT_DECODING
	switch (sourceNativeFrame->format) {
		case AVPixelFormat::AV_PIX_FMT_YUV420P:
			assert(video.targetTextureCount == 3);
			for (uint32_t textureIndex = 0; textureIndex < video.targetTextureCount; ++textureIndex) {
				VideoTexture &targetTexture = video.targetTextures[textureIndex];
				uint8_t *data = LockVideoTexture(targetTexture);
				assert(data != nullptr);
				uint32_t h = (textureIndex == 0) ? sourceNativeFrame->height : sourceNativeFrame->height / 2;
				fplMemoryCopy(sourceNativeFrame->data[textureIndex], sourceNativeFrame->linesize[textureIndex] * h, data);
				UnlockVideoTexture(targetTexture);
			}
			break;
		default:
			break;
	}
#else
	assert(video.targetTextureCount == 1);
	VideoTexture &targetTexture = video.targetTextures[0];
	assert(targetTexture.width == sourceNativeFrame->width);
	assert(targetTexture.height == sourceNativeFrame->height);

	uint8_t *data = LockVideoTexture(targetTexture);
	assert(data != nullptr);

	int32_t dstLineSize[8] = { targetTexture.rowSize, 0 };
	uint8_t *dstData[8] = { data, nullptr };
	uint8_t *srcData[8];
	int srcLineSize[8];
	for (int i = 0; i < 8; ++i) {
		srcData[i] = sourceNativeFrame->data[i];
		srcLineSize[i] = sourceNativeFrame->linesize[i];
	}

#if USE_FFMPEG_SOFTWARE_CONVERSION
	ffmpeg.sws_scale(video.softwareScaleCtx, (uint8_t const *const *)srcData, srcLineSize, 0, videoCodecCtx->height, dstData, dstLineSize);
#else
	ConversionFlags flags = ConversionFlags::None;
#	if USE_HARDWARE_RENDERING
	flags |= ConversionFlags::DstBGRA;
#	endif
	switch (sourceNativeFrame->format) {
		case AVPixelFormat::AV_PIX_FMT_YUV420P:
			ConvertYUV420PToRGB32(dstData, dstLineSize, targetTexture.width, targetTexture.height, srcData, srcLineSize, flags);
			break;
		default:
			ffmpeg.sws_scale(video.softwareScaleCtx, (uint8_t const *const *)srcData, srcLineSize, 0, videoCodecCtx->height, dstData, dstLineSize);
			break;
	}
#endif
	UnlockVideoTexture(targetTexture);
#endif
}

//
// Audio
//
struct AudioContext {
	MediaStream stream;
	Decoder decoder;
	fplAudioDeviceFormat audioSource;
	fplAudioDeviceFormat audioTarget;
	Clock clock;
	double audioClock;
	int32_t audioClockSerial;
	int32_t audioDiffAvgCount;
	double audioDiffCum;
	double audioDiffAbgCoef;
	double audioDiffThreshold;

	SwrContext *softwareResampleCtx;
	Frame *pendingAudioFrame;

	// @NOTE(final): Buffer holding some amount of samples in the format FPL expects, required for doing conversion using swr_convert().
	uint8_t *conversionAudioBuffer;
	uint32_t maxConversionAudioFrameCount;
	uint32_t maxConversionAudioBufferSize;
	uint32_t conversionAudioFramesRemaining;
	uint32_t conversionAudioFrameIndex;
};

static AVSampleFormat MapAudioFormatType(const fplAudioFormatType format) {
	// @TODO(final): Support planar formats as well
	switch (format) {
		case fplAudioFormatType_U8:
			return AVSampleFormat::AV_SAMPLE_FMT_U8;
		case fplAudioFormatType_S16:
			return AVSampleFormat::AV_SAMPLE_FMT_S16;
		case fplAudioFormatType_S32:
			return AVSampleFormat::AV_SAMPLE_FMT_S32;
		case fplAudioFormatType_F32:
			return AVSampleFormat::AV_SAMPLE_FMT_FLT;
		case fplAudioFormatType_F64:
			return AVSampleFormat::AV_SAMPLE_FMT_DBL;
		default:
			return AVSampleFormat::AV_SAMPLE_FMT_NONE;
	}
}

static fplAudioFormatType MapAVSampleFormat(const AVSampleFormat format) {
	switch (format) {
		case AV_SAMPLE_FMT_U8:
		case AV_SAMPLE_FMT_U8P:
			return fplAudioFormatType_U8;
		case AV_SAMPLE_FMT_S16:
		case AV_SAMPLE_FMT_S16P:
			return fplAudioFormatType_S16;
		case AV_SAMPLE_FMT_S32:
		case AV_SAMPLE_FMT_S32P:
			return fplAudioFormatType_S32;
		case AV_SAMPLE_FMT_S64:
		case AV_SAMPLE_FMT_S64P:
			return fplAudioFormatType_S64;
		case AV_SAMPLE_FMT_FLT:
		case AV_SAMPLE_FMT_FLTP:
			return fplAudioFormatType_F32;
		case AV_SAMPLE_FMT_DBL:
		case AV_SAMPLE_FMT_DBLP:
			return fplAudioFormatType_F64;
		default:
			return fplAudioFormatType_None;
	}
}

static bool IsPlanarAVSampleFormat(const AVSampleFormat format) {
	switch (format) {
		case AV_SAMPLE_FMT_U8P:
		case AV_SAMPLE_FMT_S16P:
		case AV_SAMPLE_FMT_S32P:
		case AV_SAMPLE_FMT_S64P:
		case AV_SAMPLE_FMT_FLTP:
		case AV_SAMPLE_FMT_DBLP:
			return true;
		default:
			return false;
	}
}

//
// Player
//
struct PlayerPosition {
	int64_t value;
	bool32 isValid;
};

struct PlayerSettings {
	PlayerPosition startTime;
	PlayerPosition duration;
	int32_t frameDrop;
	int32_t reorderDecoderPTS;
	bool32 isInfiniteBuffer;
	bool32 isLoop;
	bool32 isVideoDisabled;
	bool32 isAudioDisabled;
};

static void InitPlayerSettings(PlayerSettings &settings) {
	settings.startTime = {};
	settings.duration = {};
	settings.frameDrop = 1;
	settings.isInfiniteBuffer = false;
	settings.isLoop = false;
	settings.reorderDecoderPTS = -1;
}

struct SeekState {
	int64_t pos;
	int64_t rel;
	int32_t seekFlags;
	bool32 isRequired;
};

//
// Font
//
struct FontChar {
	// Cartesian coordinates: TR, TL, BL, BR

	// Just for debug purposes
	uint32_t charCode;
	Vec2f uv[4]; // In range of 0.0 to 1.0
	Vec2f offset[4]; // In range of -1.0 to 1.0
	float advance; // In range of -1.0 to 1.0
};

struct FontInfo {
	FontChar *chars;
	uint8_t *atlasBitmap;
	float ascent;
	float descent;
	float lineGap;
	uint32_t atlasWidth;
	uint32_t atlasHeight;
	uint32_t firstChar;
	uint32_t charCount;
	bool32 isValid;
};

static FontChar GetFontChar(const FontInfo &info, const uint32_t codePoint) {
	uint32_t lastCharPastOne = info.firstChar + info.charCount;
	fplAssert(codePoint >= info.firstChar && codePoint < lastCharPastOne);
	uint32_t charIndex = codePoint - info.firstChar;
	FontChar result = info.chars[charIndex];
	return(result);
}

static void ReleaseFontInfo(FontInfo &font) {
	if (font.chars)
		fplMemoryFree(font.chars);
	if (font.atlasBitmap)
		fplMemoryFree(font.atlasBitmap);
}

static bool LoadFontInfo(const uint8_t *data, const size_t dataSize, const uint32_t atlasWidth, const uint32_t atlasHeight, const uint32_t firstChar, const uint32_t charCount, const float fontSize, FontInfo *outFont) {
	FontInfo font = {};
	font.atlasWidth = atlasWidth;
	font.atlasHeight = atlasHeight;
	font.firstChar = firstChar;
	font.charCount = charCount;

	int fontIndex = 0;
	int fontOffset = stbtt_GetFontOffsetForIndex(data, fontIndex);
	stbtt_fontinfo nativeInfo;
	if (!stbtt_InitFont(&nativeInfo, data, fontOffset)) {
		return(false);
	}

	// TODO: One memory block for both
	font.atlasBitmap = (uint8_t *)fplMemoryAllocate(font.atlasWidth * font.atlasHeight);
	font.chars = (FontChar *)fplMemoryAllocate(font.charCount * sizeof(*font.chars));

	int ascent = 0;
	int descent = 0;
	int lineGap = 0;

	float fontScale = 1.0f / fontSize;
	float pixelScale = stbtt_ScaleForPixelHeight(&nativeInfo, fontSize);
	stbtt_GetFontVMetrics(&nativeInfo, &ascent, &descent, &lineGap);

	font.ascent = (float)ascent * pixelScale * fontScale;
	font.descent = (float)descent * pixelScale * fontScale;
	font.lineGap = (float)lineGap * pixelScale * fontScale;

	stbtt_pack_context context;
	if (!stbtt_PackBegin(&context, font.atlasBitmap, font.atlasWidth, font.atlasHeight, 0, 1, nullptr)) {
		ReleaseFontInfo(font);
		return(false);
	}

	int oversampleX = 2, oversampleY = 2;
	stbtt_PackSetOversampling(&context, oversampleX, oversampleY);

	stbtt_packedchar *packedChars = (stbtt_packedchar *)STBTT_malloc(sizeof(stbtt_packedchar) * font.charCount, nullptr);
	if (!stbtt_PackFontRange(&context, data, 0, fontSize, font.firstChar, font.charCount, packedChars)) {
		STBTT_free(packedChars, nullptr);
		ReleaseFontInfo(font);
		return(false);
	}

	float invAtlasW = 1.0f / (float)font.atlasWidth;
	float invAtlasH = 1.0f / (float)font.atlasHeight;

	float baseline = font.ascent;

	for (uint32_t charIndex = 0; charIndex < font.charCount; ++charIndex) {
		const stbtt_packedchar *b = packedChars + charIndex;

		FontChar *outChar = font.chars + charIndex;

		float s0 = b->x0 * invAtlasW;
		float s1 = b->x1 * invAtlasW;
		float t0 = b->y0 * invAtlasH;
		float t1 = b->y1 * invAtlasH;

		float x0 = b->xoff * pixelScale;
		float x1 = b->xoff2 * pixelScale;

		// Y must be inverted, to flip letter (Cartesian conversion)
		float y0 = b->yoff * -pixelScale;
		float y1 = b->yoff2 * -pixelScale;

		// Y must be inverted, to flip letter (Cartesian conversion)
		outChar->offset[0] = Vec2f(x1, y0); // Top-right
		outChar->offset[1] = Vec2f(x0, y0); // Top-left
		outChar->offset[2] = Vec2f(x0, y1); // Bottom-left
		outChar->offset[3] = Vec2f(x1, y1); // Bottom-right

		outChar->uv[0] = Vec2f(s1, t0);
		outChar->uv[1] = Vec2f(s0, t0);
		outChar->uv[2] = Vec2f(s0, t1);
		outChar->uv[3] = Vec2f(s1, t1);

		outChar->advance = b->xadvance * pixelScale;
	}

	STBTT_free(packedChars, nullptr);

	stbtt_PackEnd(&context);

	*outFont = font;

	return(true);
}

enum class TextRenderMode {
	Baseline = 0,
	Bottom,
};

#if USE_HARDWARE_RENDERING
struct IndexBuffer {
	uint32_t *indices;
	uint32_t lastIndex;
	uint32_t capacity;
	uint32_t count;

	static const GLuint target = GL_ELEMENT_ARRAY_BUFFER;
	GLuint ibo;

	static IndexBuffer Alloc(const uint32_t capacity) {
		IndexBuffer result = {};
		result.capacity = capacity;
		result.indices = (uint32_t *)fplMemoryAllocate(sizeof(*result.indices) * capacity);

		size_t indicesSize = capacity * sizeof(*result.indices);

		glGenBuffers(1, &result.ibo);
		glBindBuffer(target, result.ibo);
		glBufferData(target, indicesSize, nullptr, GL_DYNAMIC_DRAW);
		glBindBuffer(target, 0);

		return(result);
	}

	void Clear() {
		lastIndex = count = 0;
	}

	void Release() {
		if (ibo) {
			glDeleteBuffers(1, &ibo);
			ibo = 0;
		}

		if (indices) {
			fplMemoryFree(indices);
			indices = nullptr;
		}
		lastIndex = capacity = count = 0;
	}

	void Bind() {
		glBindBuffer(target, ibo);
	}

	void Unbind() {
		glBindBuffer(target, 0);
	}
};

struct BufferVertex {
	Vec4f pos;
	Vec4f color;
	Vec2f uv;
};

struct VertexBuffer {
	BufferVertex *verts;
	uint32_t capacity;
	uint32_t count;
	uint32_t stride;

	static const GLuint target = GL_ARRAY_BUFFER;
	GLuint vbo;

	void Clear() {
		count = 0;
	}

	static VertexBuffer Alloc(const uint32_t capacity) {
		VertexBuffer result = {};
		result.capacity = capacity;
		result.verts = (BufferVertex *)fplMemoryAllocate(sizeof(*result.verts) * capacity);
		result.stride = sizeof(*result.verts);

		size_t vertsSize = capacity * sizeof(*result.verts);

		glGenBuffers(1, &result.vbo);
		glBindBuffer(target, result.vbo);
		glBufferData(target, vertsSize, nullptr, GL_DYNAMIC_DRAW);
		glBindBuffer(target, 0);

		return(result);
	}

	void Release() {
		if (vbo) {
			glDeleteBuffers(1, &vbo);
			vbo = 0;
		}

		if (verts) {
			fplMemoryFree(verts);
			verts = nullptr;
		}
		capacity = count = 0;
	}

	void Bind() {
		glBindBuffer(target, vbo);
	}

	void Unbind() {
		glBindBuffer(target, 0);
	}
};

constexpr uint32_t MAX_FONT_BUFFER_VERTEX_COUNT = 32 * 1024;
constexpr uint32_t MAX_FONT_BUFFER_INDEX_COUNT = MAX_FONT_BUFFER_VERTEX_COUNT * 6;
constexpr uint32_t MAX_FONT_BUFFER_ELEMENT_COUNT = MAX_FONT_BUFFER_INDEX_COUNT / 3;

struct FontBuffer {
	VertexBuffer vb;
	IndexBuffer ib;

	GLuint vao;
	GLuint texture;
	GLuint programId;
	GLuint uniform_uniViewProjMat;
	GLuint uniform_uniTexture;
};

static void ReleaseFontBuffer(FontBuffer &buffer) {
	if (buffer.programId) {
		glDeleteProgram(buffer.programId);
		buffer.programId = 0;
	}

	if (buffer.vao) {
		glDeleteVertexArrays(1, &buffer.vao);
		buffer.vao = 0;
	}

	if (buffer.texture) {
		glDeleteTextures(1, &buffer.texture);
		buffer.texture = 0;
	}

	buffer.ib.Release();
	buffer.vb.Release();
}

static FontBuffer AllocFontBuffer(const uint32_t atlasWidth, const uint32_t atlasHeight, const uint8_t *atlasBitmap) {
	FontBuffer result = {};

	result.vb = VertexBuffer::Alloc(MAX_FONT_BUFFER_VERTEX_COUNT);
	result.ib = IndexBuffer::Alloc(MAX_FONT_BUFFER_INDEX_COUNT);

	size_t vertexStride = result.vb.stride;

	glGenVertexArrays(1, &result.vao);
	glBindVertexArray(result.vao);

	result.vb.Bind();
	result.ib.Bind();

	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, (GLsizei)vertexStride, (void *)offsetof(BufferVertex, pos));
	glEnableVertexAttribArray(0);

	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, (GLsizei)vertexStride, (void *)offsetof(BufferVertex, color));
	glEnableVertexAttribArray(1);

	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, (GLsizei)vertexStride, (void *)offsetof(BufferVertex, uv));
	glEnableVertexAttribArray(2);

	glBindVertexArray(0);
	result.ib.Unbind();
	result.vb.Unbind();
	CheckGLError();

	// Texture
	glGenTextures(1, &result.texture);
	glBindTexture(GL_TEXTURE_2D, result.texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlasWidth, atlasHeight, 0, GL_RED, GL_UNSIGNED_BYTE, atlasBitmap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);
	CheckGLError();

	// Shader
	result.programId = CreateShader(FontShaderSource::Vertex, FontShaderSource::Fragment, FontShaderSource::Name);
	result.uniform_uniViewProjMat = glGetUniformLocation(result.programId, "uniViewProjMat");
	result.uniform_uniTexture = glGetUniformLocation(result.programId, "uniTexture");

	return(result);
}

static void ClearFontBuffer(FontBuffer &buffer) {
	buffer.vb.Clear();
	buffer.ib.Clear();
}

static void PushQuadToBuffer(FontBuffer &buffer, const Vec2f &position, const Vec2f &size, const Vec4f &color) {
	uint32_t indexCount = 6;
	uint32_t vertexCount = 4;
	fplAssert((buffer.vb.count + vertexCount) <= buffer.vb.capacity);
	fplAssert((buffer.ib.count + indexCount) <= buffer.ib.capacity);

	uint32_t vertexOffset = buffer.vb.count * sizeof(*buffer.vb.verts);
	uint32_t elementOffset = buffer.ib.count * sizeof(*buffer.ib.indices);
	uint32_t verticesSize = vertexCount * sizeof(*buffer.vb.verts);
	uint32_t indicesSize = indexCount * sizeof(*buffer.ib.indices);

	BufferVertex *verts = buffer.vb.verts;
	uint32_t *indices = buffer.ib.indices;

	uint32_t vertexStart = buffer.vb.count;
	uint32_t indexStart = buffer.ib.count;

	Vec3f p0 = Vec3f(position.x + size.w, position.y + size.h, 0.0f);
	Vec3f p1 = Vec3f(position.x, position.y + size.h, 0.0f);
	Vec3f p2 = Vec3f(position.x, position.y, 0.0f);
	Vec3f p3 = Vec3f(position.x + size.w, position.y, 0.0f);

	Vec2f uv0 = Vec2f(1.0f, 1.0f); // Top-right
	Vec2f uv1 = Vec2f(0.0f, 1.0f); // Top-left
	Vec2f uv2 = Vec2f(0.0f, 0.0f); // Bottom-left
	Vec2f uv3 = Vec2f(1.0f, 0.0f); // Bottom-right

	uint32_t vertexIndex = vertexStart;
	verts[vertexIndex++] = { Vec4f(p0, 1.0f), color, uv0 }; // Top-right
	verts[vertexIndex++] = { Vec4f(p1, 1.0f), color, uv1 }; // Top-left
	verts[vertexIndex++] = { Vec4f(p2, 1.0f), color, uv2 }; // Bottom-left
	verts[vertexIndex++] = { Vec4f(p3, 1.0f), color, uv3 }; // Bottom-right

	uint32_t elementIndex = indexStart;
	indices[elementIndex++] = buffer.ib.lastIndex + 0;
	indices[elementIndex++] = buffer.ib.lastIndex + 1;
	indices[elementIndex++] = buffer.ib.lastIndex + 2;
	indices[elementIndex++] = buffer.ib.lastIndex + 2;
	indices[elementIndex++] = buffer.ib.lastIndex + 3;
	indices[elementIndex++] = buffer.ib.lastIndex + 0;

	buffer.ib.lastIndex += 4;

	buffer.vb.Bind();
	glBufferSubData(GL_ARRAY_BUFFER, vertexOffset, verticesSize, verts + vertexStart);
	buffer.vb.Unbind();
	buffer.vb.count += vertexCount;
	CheckGLError();

	buffer.ib.Bind();
	glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, elementOffset, indicesSize, indices + indexStart);
	buffer.ib.Unbind();
	buffer.ib.count += indexCount;
	CheckGLError();
}

static Vec2f ComputeTextSize(const FontInfo &info, const char *text, const float scale) {
	const char *s = text;
	Vec2f result = Vec2f(0, 0);
	while (*s) {
		FontChar glyph = GetFontChar(info, *s);

		Vec2f p0 = glyph.offset[0] * scale;
		Vec2f p1 = glyph.offset[1] * scale;
		Vec2f p2 = glyph.offset[2] * scale;
		Vec2f p3 = glyph.offset[3] * scale;

		// TODO(final): Compute actual text rectangle
		//Vec2f charSize = 

		result += Vec2f(glyph.advance * scale, 0.0f);
		++s;
	}
}



static void PushTextToBuffer(FontBuffer &buffer, const FontInfo &info, const char *text, const float scale, const Vec2f &position, const Vec4f &color, const TextRenderMode mode) {
	uint32_t textLen = (uint32_t)fplGetStringLength(text);
	if (textLen > 0) {
		uint32_t indexCount = textLen * 6;
		uint32_t vertexCount = textLen * 4;
		fplAssert((buffer.vb.count + vertexCount) <= buffer.vb.capacity);
		fplAssert((buffer.ib.count + indexCount) <= buffer.ib.capacity);

		size_t vertexOffset = buffer.vb.count * sizeof(*buffer.vb.verts);
		size_t elementOffset = buffer.ib.count * sizeof(*buffer.ib.indices);
		size_t verticesSize = vertexCount * sizeof(*buffer.vb.verts);
		size_t indicesSize = indexCount * sizeof(*buffer.ib.indices);

		BufferVertex *verts = buffer.vb.verts;
		uint32_t *indices = buffer.ib.indices;

		uint32_t vertexStart = buffer.vb.count;
		uint32_t indexStart = buffer.ib.count;

		const char *s = text;
		Vec2f offset = position;
		uint32_t vertexIndex = vertexStart;
		uint32_t elementIndex = indexStart;
		while (*s) {
			FontChar glyph = GetFontChar(info, *s);

			Vec3f p0 = Vec3f(offset + glyph.offset[0] * scale, 0.0f);
			Vec3f p1 = Vec3f(offset + glyph.offset[1] * scale, 0.0f);
			Vec3f p2 = Vec3f(offset + glyph.offset[2] * scale, 0.0f);
			Vec3f p3 = Vec3f(offset + glyph.offset[3] * scale, 0.0f);

			verts[vertexIndex++] = { Vec4f(p0, 1.0f), color, glyph.uv[0] }; // Top-right
			verts[vertexIndex++] = { Vec4f(p1, 1.0f), color, glyph.uv[1] }; // Top-left
			verts[vertexIndex++] = { Vec4f(p2, 1.0f), color, glyph.uv[2] }; // Bottom-left
			verts[vertexIndex++] = { Vec4f(p3, 1.0f), color, glyph.uv[3] }; // Bottom-right

			indices[elementIndex++] = buffer.ib.lastIndex + 0;
			indices[elementIndex++] = buffer.ib.lastIndex + 1;
			indices[elementIndex++] = buffer.ib.lastIndex + 2;
			indices[elementIndex++] = buffer.ib.lastIndex + 2;
			indices[elementIndex++] = buffer.ib.lastIndex + 3;
			indices[elementIndex++] = buffer.ib.lastIndex + 0;

			buffer.ib.lastIndex += 4;

			offset += Vec2f(glyph.advance * scale, 0.0f);

			++s;
		}

		buffer.vb.Bind();
		glBufferSubData(GL_ARRAY_BUFFER, vertexOffset, verticesSize, verts + vertexStart);
		buffer.vb.Unbind();
		buffer.vb.count += vertexCount;
		CheckGLError();

		buffer.ib.Bind();
		glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, elementOffset, indicesSize, indices + indexStart);
		buffer.ib.Unbind();
		buffer.ib.count += indexCount;
		CheckGLError();
	}
}
#else
// For now font rendering is disabled in non-hardware rendering mode
struct FontBuffer {
	int empty;
};
static FontBuffer AllocFontBuffer(const uint32_t atlasWidth, const uint32_t atlasHeight, const uint8_t *atlasBitmap) {
	FontBuffer result = fplZeroInit;
	return(result);
}
static void ReleaseFontBuffer(FontBuffer &buffer) {
}
static void ClearFontBuffer(FontBuffer &buffer) {
}
static void PushTextToBuffer(FontBuffer &buffer, const FontInfo &info, const char *text, const float scale, const Vec2f &position, const Vec4f &color, const TextRenderMode mode) {
}
#endif // USE_HARDWARE_RENDERING

//
// Player
//
constexpr uint32_t MAX_STREAM_COUNT = 8;
struct PlayerState {
	ReaderContext reader;
	MediaStream streams[MAX_STREAM_COUNT];
	VideoContext video;
	AudioContext audio;
	PlayerSettings settings;

	FontInfo fontInfo;

	FontBuffer fontBuffer;

	Clock externalClock;
	SeekState seek;
	fplWindowSize viewport;

	AVFormatContext *formatCtx;

	const char *filePathOrUrl;

	double streamLength; // Length of the stream in seconds
	double frameLastPTS;
	double frameLastDelay;
	double frameTimer;
	double maxFrameDuration;
	double pauseClock;

	AVSyncType syncType;
	volatile uint32_t forceRefresh;

	int loop;
	int readPauseReturn;
	int step;
	int frame_drops_early;
	int frame_drops_late;
	int64_t pauseNumFrames;

	bool32 isInfiniteBuffer;
	bool32 isRealTime;
	bool32 isPaused;
	bool32 lastPaused;
	bool32 isFullscreen;
	bool32 seekByBytes;
};

static void ReleasePlayer(PlayerState &state) {
	ReleaseFontBuffer(state.fontBuffer);
	ReleaseFontInfo(state.fontInfo);
}

static bool InitPlayer(PlayerState &state) {
	//
	// OpenGL
	//
#if USE_HARDWARE_RENDERING
#if USE_GL_BLENDING
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#else
	glDisable(GL_BLEND);
#endif

	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
	glEnable(GL_CULL_FACE);
#endif

//
// Font Info
//
	uint32_t firstChar = ' ';
	uint32_t charCount = '~' - firstChar;
	if (!LoadFontInfo(sulphurPointRegularData, sulphurPointRegularDataSize, 1024, 1024, firstChar, charCount, 40.0f, &state.fontInfo)) {
		ReleasePlayer(state);
		return(false);
	}

	// Font Buffer
	state.fontBuffer = AllocFontBuffer(state.fontInfo.atlasWidth, state.fontInfo.atlasHeight, state.fontInfo.atlasBitmap);


	//
	// Settings
	//
	InitPlayerSettings(state.settings);
	state.isInfiniteBuffer = state.settings.isInfiniteBuffer;
	state.loop = state.settings.isLoop ? 1 : 0;

	return(true);
}

//
// Utils
//
static void PutPacketBackToReader(ReaderContext &reader, PacketList *packet) {
	ReleasePacket(reader.packetQueue, packet);
}

static bool StreamHasEnoughPackets(const AVStream *stream, const int streamIndex, const PacketQueue &queue) {
	bool result = (streamIndex < 0) ||
		(stream->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
		((queue.packetCount > MIN_PACKET_FRAMES) && (!queue.duration || (av_q2d(stream->time_base) * queue.duration) > 1.0));
	return (result);
}

static AVSyncType GetMasterSyncType(const PlayerState *state) {
	if (state->syncType == AVSyncType::VideoMaster) {
		if (state->video.stream.isValid) {
			return AVSyncType::VideoMaster;
		} else {
			return AVSyncType::AudioMaster;
		}
	} else if (state->syncType == AVSyncType::AudioMaster) {
		if (state->audio.stream.isValid) {
			return AVSyncType::AudioMaster;
		} else {
			return AVSyncType::ExternalClock;
		}
	} else {
		return AVSyncType::ExternalClock;
	}
}

static double GetMasterFrameRate(const PlayerState *state) {
	if (state->video.stream.isValid && state->video.stream.stream->avg_frame_rate.den != 0)
		return av_q2d(state->video.stream.stream->avg_frame_rate);
	if (state->audio.stream.isValid && state->audio.stream.stream->avg_frame_rate.den != 0)
		return av_q2d(state->audio.stream.stream->avg_frame_rate);
	else
		return 0.0;
}

static const AVStream *GetMasterStream(const PlayerState *state) {
	if (state->video.stream.isValid && state->video.stream.stream->avg_frame_rate.den != 0)
		return state->video.stream.stream;
	if (state->audio.stream.isValid && state->audio.stream.stream->avg_frame_rate.den != 0)
		return state->audio.stream.stream;
	else
		return nullptr;
}

static double GetMasterClock(const PlayerState *state) {
	double result = 0;
	switch (GetMasterSyncType(state)) {
		case AVSyncType::VideoMaster:
			result = GetClock(state->video.clock);
			break;
		case AVSyncType::AudioMaster:
			result = GetClock(state->audio.clock);
			break;
		case AVSyncType::ExternalClock:
			result = GetClock(state->externalClock);
			break;
	}
	return result;
}

static void UpdateExternalClockSpeed(PlayerState *state) {
	if ((state->video.stream.isValid && state->video.decoder.packetsQueue.packetCount <= EXTERNAL_CLOCK_MIN_FRAMES) ||
		(state->audio.stream.isValid && state->audio.decoder.packetsQueue.packetCount <= EXTERNAL_CLOCK_MIN_FRAMES)) {
		SetClockSpeed(state->externalClock, fplMax(EXTERNAL_CLOCK_SPEED_MIN, state->externalClock.speed - EXTERNAL_CLOCK_SPEED_STEP));
	} else if ((!state->video.stream.isValid || (state->video.decoder.packetsQueue.packetCount > EXTERNAL_CLOCK_MAX_FRAMES)) &&
		(!state->audio.stream.isValid || (state->audio.decoder.packetsQueue.packetCount > EXTERNAL_CLOCK_MAX_FRAMES))) {
		SetClockSpeed(state->externalClock, fplMin(EXTERNAL_CLOCK_SPEED_MAX, state->externalClock.speed + EXTERNAL_CLOCK_SPEED_STEP));
	} else {
		double speed = state->externalClock.speed;
		if (speed != 1.0) {
			SetClockSpeed(state->externalClock, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
		}
	}
}

static void AddFrameToDecoder(Decoder &decoder, Frame *frame, AVFrame *srcFrame) {
	ffmpeg.av_frame_move_ref(frame->frame, srcFrame);
	NextWritable(decoder.frameQueue);
}

namespace DecodeResults {
	enum DecodeResultEnum {
		Failed = -99,
		Stopped = -1,
		Success = 0,
		RequireMorePackets,
		EndOfStream,
		Skipped,
	};
};
typedef DecodeResults::DecodeResultEnum DecodeResult;

static DecodeResult DecodeFrame(ReaderContext &reader, Decoder &decoder, AVFrame *frame) {
	assert(decoder.stream != nullptr);
	AVCodecContext *codecCtx = decoder.stream->codecContext;
	int ret = AVERROR(EAGAIN);
	PacketList *pkt;
	for (;;) {
		if (decoder.packetsQueue.serial == decoder.pktSerial) {
			do {
				if (decoder.isEOF) {
					return DecodeResult::Skipped;
				}
				if (decoder.stopRequest) {
					return DecodeResult::Stopped;
				}

				switch (codecCtx->codec_type) {
					case AVMediaType::AVMEDIA_TYPE_VIDEO:
					{
						ret = ffmpeg.avcodec_receive_frame(codecCtx, frame);
						if (ret >= 0) {
							if (decoder.state->settings.reorderDecoderPTS == -1) {
								frame->pts = frame->best_effort_timestamp;
							} else if (!decoder.state->settings.reorderDecoderPTS) {
								frame->pts = frame->pkt_dts;
							}
						}
					} break;

					case AVMediaType::AVMEDIA_TYPE_AUDIO:
					{
						ret = ffmpeg.avcodec_receive_frame(codecCtx, frame);
						if (ret >= 0) {
							AVRational tb = { 1, frame->sample_rate };
							if (frame->pts != AV_NOPTS_VALUE) {
								frame->pts = ffmpeg.av_rescale_q(frame->pts, codecCtx->pkt_timebase, tb);
							} else if (decoder.next_pts != AV_NOPTS_VALUE) {
								frame->pts = ffmpeg.av_rescale_q(decoder.next_pts, decoder.next_pts_tb, tb);
							}
							if (frame->pts != AV_NOPTS_VALUE) {
								decoder.next_pts = frame->pts + frame->nb_samples;
								decoder.next_pts_tb = tb;
							}
						}
					} break;
				}
				if (ret >= 0) {
					return DecodeResult::Success;
				} else if (ret == AVERROR_EOF) {
					decoder.finishedSerial = decoder.pktSerial;
					ffmpeg.avcodec_flush_buffers(codecCtx);
					return DecodeResult::EndOfStream;
				} else if (ret == AVERROR(EAGAIN)) {
					// This will continue sending packets until the frame is complete
					break;
				} else {
					return DecodeResult::Failed;
				}
			} while (ret != AVERROR(EAGAIN));
		}

		do {
			if (decoder.frameQueue.hasPendingPacket) {
				assert(decoder.frameQueue.pendingPacket != nullptr);
				pkt = decoder.frameQueue.pendingPacket;
				decoder.frameQueue.hasPendingPacket = false;
			} else {
				pkt = nullptr;
				if (PopPacket(decoder.packetsQueue, pkt)) {
					decoder.pktSerial = pkt->serial;
				} else {
					// We cannot continue to decode, because the packet queue is empty
					return DecodeResult::RequireMorePackets;
				}
			}
		} while (decoder.packetsQueue.serial != decoder.pktSerial);

		if (pkt != nullptr) {
			if (IsFlushPacket(pkt)) {
				ffmpeg.avcodec_flush_buffers(decoder.stream->codecContext);
				decoder.finishedSerial = 0;
				decoder.next_pts = decoder.start_pts;
				decoder.next_pts_tb = decoder.start_pts_tb;
				PutPacketBackToReader(reader, pkt);
			} else {
				if (ffmpeg.avcodec_send_packet(codecCtx, &pkt->packet) == AVERROR(EAGAIN)) {
					decoder.frameQueue.hasPendingPacket = true;
					decoder.frameQueue.pendingPacket = pkt;
				} else {
					PutPacketBackToReader(reader, pkt);
				}
			}
		}
	}
}

static void QueuePicture(Decoder &decoder, AVFrame *sourceFrame, Frame *targetFrame, const int32_t serial) {
	assert(targetFrame != nullptr);
	assert(targetFrame->frame != nullptr);
	assert(targetFrame->frame->pkt_size <= 0);
	assert(targetFrame->frame->width == 0);

	AVStream *videoStream = decoder.stream->stream;

	AVRational currentTimeBase = videoStream->time_base;
	AVRational currentFrameRate = ffmpeg.av_guess_frame_rate(decoder.state->formatCtx, videoStream, nullptr);

	targetFrame->pos = sourceFrame->pkt_pos;
	targetFrame->pts = (sourceFrame->pts == AV_NOPTS_VALUE) ? NAN : sourceFrame->pts * av_q2d(currentTimeBase);
	targetFrame->duration = (currentFrameRate.num && currentFrameRate.den ? av_q2d({ currentFrameRate.den, currentFrameRate.num }) : 0);
	targetFrame->serial = serial;
	targetFrame->isUploaded = false;
	targetFrame->sar = sourceFrame->sample_aspect_ratio;
	targetFrame->width = sourceFrame->width;
	targetFrame->height = sourceFrame->height;

#if PRINT_PTS
	ConsoleFormatOut("PTS V: %7.2f, Next: %7.2f\n", targetFrame->pts, decoder.next_pts);
#endif

	AddFrameToDecoder(decoder, targetFrame, sourceFrame);
}

static void VideoDecodingThreadProc(const fplThreadHandle *thread, void *userData) {
	Decoder *decoder = (Decoder *)userData;
	assert(decoder != nullptr);

	ReaderContext &reader = *decoder->reader;

	MediaStream *stream = decoder->stream;
	assert(stream != nullptr);
	assert(stream->isValid);
	assert(stream->streamIndex > -1);

	PlayerState *state = decoder->state;

	fplSignalHandle *waitSignals[] = {
		// New packet arrived
		&decoder->packetsQueue.addedSignal,
		// Frame queue changed
		&decoder->frameQueue.signal,
		// Stopped decoding
		&decoder->stopSignal,
		// Resume from sleeping
		&decoder->resumeSignal,
	};

	AVStream *videoStream = decoder->stream->stream;

	AVFrame *sourceFrame = ffmpeg.av_frame_alloc();
	bool hasDecodedFrame = false;
	for (;;) {
		// Wait for any signal (Available packet, Free frame, Stopped, Wake up)
		fplSignalWaitForAny(waitSignals, fplArrayCount(waitSignals), sizeof(fplSignalHandle *), FPL_TIMEOUT_INFINITE);

		// Stop decoder
		if (decoder->stopRequest) {
			break;
		}

		// Wait until the decoder wakes up in the next iteration when the decoder is paused
		if (decoder->isEOF) {
			fplThreadSleep(10);
			continue;
		}

		if (!hasDecodedFrame) {
			// Decode video frame
			DecodeResult decodeResult = DecodeFrame(reader, *decoder, sourceFrame);
			if (decodeResult != DecodeResult::Success) {
				if (decodeResult != DecodeResult::RequireMorePackets) {
					ffmpeg.av_frame_unref(sourceFrame);
				}
				if (decodeResult == DecodeResult::EndOfStream) {
					decoder->isEOF = 1;
					continue;
				} else if (decodeResult <= DecodeResult::Stopped) {
					break;
				}

				// Stream finished and no packets left to decode, then are finished as well
				if (reader.isEOF && (decoder->packetsQueue.packetCount == 0)) {
					decoder->isEOF = 1;
				}
			} else {
#if PRINT_QUEUE_INFOS
				uint32_t decodedVideoFrameIndex = AtomicAddU32(&decoder->decodedFrameCount, 1);
				ConsoleFormatOut("Decoded video frame %lu\n", decodedVideoFrameIndex);
#endif
				hasDecodedFrame = true;

				if (state->settings.frameDrop > 0 || (state->settings.frameDrop && GetMasterSyncType(state) != AVSyncType::VideoMaster)) {
					double dpts = NAN;
					if (sourceFrame->pts != AV_NOPTS_VALUE) {
						dpts = av_q2d(stream->stream->time_base) * sourceFrame->pts;
					}
					if (!isnan(dpts)) {
						double diff = dpts - GetMasterClock(state);
						if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
							diff < 0 &&
							decoder->pktSerial == state->video.clock.serial &&
							decoder->packetsQueue.packetCount) {
							state->frame_drops_early++;
							ffmpeg.av_frame_unref(sourceFrame);
							hasDecodedFrame = false;
#if PRINT_FRAME_DROPS
							FPL_LOG_INFO("App", "Frame drops: %d/%d\n", state->frame_drops_early, state->frame_drops_late);
#endif
						}
					}
				}
			}
		}

		if (hasDecodedFrame) {
			Frame *targetFrame = nullptr;
			if (PeekWritableFromFrameQueue(decoder->frameQueue, targetFrame)) {
				QueuePicture(*decoder, sourceFrame, targetFrame, decoder->pktSerial);
				ffmpeg.av_frame_unref(sourceFrame);
				hasDecodedFrame = false;
			}
		}

	}
	ffmpeg.av_frame_free(&sourceFrame);
}

static void QueueSamples(Decoder &decoder, AVFrame *sourceFrame, Frame *targetFrame, int32_t serial) {
	assert(targetFrame != nullptr);
	assert(targetFrame->frame != nullptr);
	assert(targetFrame->frame->pkt_size <= 0);
	assert(targetFrame->frame->nb_samples == 0);

	AVStream *audioStream = decoder.stream->stream;
	AVRational currentTimeBase = { 1, sourceFrame->sample_rate };

	targetFrame->pos = sourceFrame->pkt_pos;
	targetFrame->pts = (sourceFrame->pts == AV_NOPTS_VALUE) ? NAN : sourceFrame->pts * av_q2d(currentTimeBase);
	targetFrame->duration = av_q2d({ sourceFrame->nb_samples, sourceFrame->sample_rate });
	targetFrame->serial = serial;

#if PRINT_PTS
	ConsoleFormatOut("PTS A: %7.2f, Next: %7.2f\n", targetFrame->pts, decoder.next_pts);
#endif

	AddFrameToDecoder(decoder, targetFrame, sourceFrame);
}

static int SyncronizeAudio(PlayerState *state, const uint32_t sampleCount) {
	int result = sampleCount;
	if (GetMasterSyncType(state) != AVSyncType::AudioMaster) {
		double diff = GetClock(state->audio.clock) - GetMasterClock(state);
		if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
			state->audio.audioDiffCum = diff + state->audio.audioDiffAbgCoef * state->audio.audioDiffCum;
			if (state->audio.audioDiffAvgCount < AV_AUDIO_DIFF_AVG_NB) {
				// Not enough measures to have a correct estimate
				state->audio.audioDiffAvgCount++;
			} else {
				// Estimate the A-V difference 
				double avgDiff = state->audio.audioDiffCum * (1.0 - state->audio.audioDiffAbgCoef);
				if (fabs(avgDiff) >= state->audio.audioDiffThreshold) {
					result = sampleCount + (int)(diff * state->audio.audioSource.sampleRate);
					int min_nb_samples = ((sampleCount * (100 - AV_SAMPLE_CORRECTION_PERCENT_MAX) / 100));
					int max_nb_samples = ((sampleCount * (100 + AV_SAMPLE_CORRECTION_PERCENT_MAX) / 100));
					result = av_clip(result, min_nb_samples, max_nb_samples);
				}
			}
		} else {
			// Too big difference : may be initial PTS errors, so reset A-V filter
			state->audio.audioDiffAvgCount = 0;
			state->audio.audioDiffCum = 0;
		}
	}
	return(result);
}

static void AudioDecodingThreadProc(const fplThreadHandle *thread, void *userData) {
	Decoder *decoder = (Decoder *)userData;
	assert(decoder != nullptr);

	ReaderContext &reader = *decoder->reader;

	PlayerState *state = decoder->state;
	assert(state != nullptr);

	MediaStream *stream = decoder->stream;
	assert(stream != nullptr);
	assert(stream->isValid);
	assert(stream->streamIndex > -1);

	fplSignalHandle *waitSignals[] = {
		// New packet arrived
		&decoder->packetsQueue.addedSignal,
		// Frame queue changed
		&decoder->frameQueue.signal,
		// Stopped decoding
		&decoder->stopSignal,
		// Resume from sleeping
		&decoder->resumeSignal,
	};

	AVFrame *sourceFrame = ffmpeg.av_frame_alloc();
	bool hasDecodedFrame = false;
	for (;;) {
		// Wait for any signal (Available packet, Free frame, Stopped, Wake up)
		fplSignalWaitForAny(waitSignals, fplArrayCount(waitSignals), sizeof(fplSignalHandle *), FPL_TIMEOUT_INFINITE);

		// Stop decoder
		if (decoder->stopRequest) {
			break;
		}

		// Wait until the decoder wakes up in the next iteration when the decoder is paused
		if (decoder->isEOF) {
			continue;
		}

		if (!hasDecodedFrame) {
			// Decode video frame
			DecodeResult decodeResult = DecodeFrame(reader, *decoder, sourceFrame);
			if (decodeResult != DecodeResult::Success) {
				if (decodeResult != DecodeResult::RequireMorePackets) {
					ffmpeg.av_frame_unref(sourceFrame);
				}
				if (decodeResult == DecodeResult::EndOfStream) {
					decoder->isEOF = 1;
					continue;
				} else if (decodeResult <= DecodeResult::Stopped) {
					break;
				}

				// Stream finished and no packets left to decode, then are finished as well
				if (reader.isEOF && (decoder->packetsQueue.packetCount == 0)) {
					decoder->isEOF = 1;
				}
			} else {
#if PRINT_QUEUE_INFOS
				uint32_t decodedAudioFrameIndex = AtomicAddU32(&decoder->decodedFrameCount, 1);
				ConsoleFormatOut("Decoded audio frame %lu\n", decodedAudioFrameIndex);
#endif
				hasDecodedFrame = true;
			}
		}

		if (hasDecodedFrame) {
			Frame *targetFrame = nullptr;
			if (PeekWritableFromFrameQueue(decoder->frameQueue, targetFrame)) {
				QueueSamples(*decoder, sourceFrame, targetFrame, decoder->pktSerial);
				ffmpeg.av_frame_unref(sourceFrame);
				hasDecodedFrame = false;
			}
		}
	}
	ffmpeg.av_frame_free(&sourceFrame);
}

static void WriteSilenceSamples(AudioContext *audio, uint32_t remainingFrameCount, uint32_t outputSampleStride, uint8_t *conversionAudioBuffer) {
	audio->conversionAudioFramesRemaining = remainingFrameCount;
	audio->conversionAudioFrameIndex = 0;
	size_t bytesToClear = remainingFrameCount * outputSampleStride;
	fplMemoryClear(conversionAudioBuffer, bytesToClear);

}

static uint32_t AudioReadCallback(const fplAudioDeviceFormat *nativeFormat, const uint32_t frameCount, void *outputSamples, void *userData) {
	double audioCallbackTime = (double)ffmpeg.av_gettime_relative();

	// FPL Audio Frame = Audio Sample = [Left S16][Right S16]
	// FPL Output frame are interleaved only!
	// FFMPEG Planar audio: (Each channel a plane -> AVFrame->data[channel])
	// FFMPEG Non-planar audio (One plane, Interleaved samples -> AVFrame->extended_data) [Left][Right],[Left][Right],..
	AudioContext *audio = (AudioContext *)userData;
	assert(audio != nullptr);

	Decoder &decoder = audio->decoder;

	PlayerState *state = decoder.state;

	uint32_t result = 0;

	if (audio->stream.isValid) {
		uint8_t *conversionAudioBuffer = audio->conversionAudioBuffer;
		uint32_t maxConversionAudioBufferSize = audio->maxConversionAudioBufferSize;

		uint32_t outputSampleStride = fplGetAudioFrameSizeInBytes(nativeFormat->type, nativeFormat->channels);
		uint32_t maxOutputSampleBufferSize = outputSampleStride * frameCount;

		uint32_t nativeBufferSizeInBytes = fplGetAudioBufferSizeInBytes(nativeFormat->type, nativeFormat->channels, nativeFormat->bufferSizeInFrames);

		fplAudioDeviceFormat *targetFormat = &state->audio.audioTarget;
		uint32_t targetBufferSizeInBytes = fplGetAudioBufferSizeInBytes(targetFormat->type, targetFormat->channels, targetFormat->bufferSizeInFrames);

		uint32_t remainingFrameCount = frameCount;
		while (remainingFrameCount > 0) {
			if (state->isPaused) {
				WriteSilenceSamples(audio, remainingFrameCount, outputSampleStride, conversionAudioBuffer);
			}

			// Consume audio in conversion buffer before we do anything else
			if ((audio->conversionAudioFramesRemaining) > 0) {
				uint32_t maxFramesToRead = audio->conversionAudioFramesRemaining;
				uint32_t framesToRead = fplMin(remainingFrameCount, maxFramesToRead);
				size_t bytesToCopy = framesToRead * outputSampleStride;

				assert(audio->conversionAudioFrameIndex < audio->maxConversionAudioFrameCount);
				size_t sourcePosition = audio->conversionAudioFrameIndex * outputSampleStride;
				assert(sourcePosition < audio->maxConversionAudioBufferSize);

				size_t destPosition = (frameCount - remainingFrameCount) * outputSampleStride;
				assert(destPosition < maxOutputSampleBufferSize);

				fplMemoryCopy(conversionAudioBuffer + sourcePosition, bytesToCopy, (uint8_t *)outputSamples + destPosition);

				remainingFrameCount -= framesToRead;
				audio->conversionAudioFrameIndex += framesToRead;
				audio->conversionAudioFramesRemaining -= framesToRead;
				result += framesToRead;
			}

			// If we consumed all remaining audio frames, then we are done.
			if (remainingFrameCount == 0) {
				// @NOTE(final): Its highly possible that there are frames left in the conversion buffer, so dont clear anything here!
				break;
			}

			// Convert entire pending frame into conversion buffer
			if (audio->pendingAudioFrame != nullptr) {
				assert(audio->conversionAudioFramesRemaining == 0);
				Frame *audioFrame = audio->pendingAudioFrame;
				assert(audioFrame->frame != nullptr);
				audio->pendingAudioFrame = nullptr;

				// Get conversion sample count
				const uint32_t maxConversionSampleCount = audio->maxConversionAudioFrameCount;
				int wantedSampleCount = SyncronizeAudio(state, audioFrame->frame->nb_samples);
				int conversionSampleCount = wantedSampleCount * nativeFormat->sampleRate / audioFrame->frame->sample_rate + 256;

				// @TODO(final): Handle audio format change here!

				//
				// Convert samples
				//
				const uint32_t sourceSampleCount = audioFrame->frame->nb_samples;
				const uint32_t sourceChannels = audioFrame->frame->channels;
				const uint32_t sourceFrameCount = sourceSampleCount;
				uint8_t **sourceSamples = audioFrame->frame->extended_data;
				// @TODO(final): Support for converting planar audio samples
				uint8_t *targetSamples[8] = {};
				targetSamples[0] = audio->conversionAudioBuffer;

				// @NOTE(final): Conversion buffer needs to be big enough to hold the samples for the frame
				assert(conversionSampleCount <= (int)maxConversionSampleCount);
				int samplesPerChannel = ffmpeg.swr_convert(audio->softwareResampleCtx, (uint8_t **)targetSamples, conversionSampleCount, (const uint8_t **)sourceSamples, sourceSampleCount);

				// We are done with this audio frame, release it
				NextReadable(decoder.frameQueue);

				// Update audio clock
				if (!isnan(audioFrame->pts)) {
					state->audio.audioClock = audioFrame->pts + (double)audioFrame->frame->nb_samples / (double)audioFrame->frame->sample_rate;
				} else {
					state->audio.audioClock = NAN;
				}
				state->audio.audioClockSerial = audioFrame->serial;

				if (samplesPerChannel <= 0) {
					break;
				}

				audio->conversionAudioFramesRemaining = samplesPerChannel;
				audio->conversionAudioFrameIndex = 0;
			}

			if ((audio->pendingAudioFrame == nullptr) && (audio->conversionAudioFramesRemaining == 0)) {
				Frame *newAudioFrame;
				if (!state->isPaused && PeekReadableFromFrameQueue(decoder.frameQueue, newAudioFrame)) {
					if (newAudioFrame->serial != decoder.packetsQueue.serial) {
						NextReadable(decoder.frameQueue);
						continue;
					}
					audio->pendingAudioFrame = newAudioFrame;
					audio->conversionAudioFrameIndex = 0;
					audio->conversionAudioFramesRemaining = 0;
					continue;
				} else {
					// No audio frame available, write silence for the remaining frames
					if (remainingFrameCount > 0) {
						WriteSilenceSamples(audio, remainingFrameCount, outputSampleStride, conversionAudioBuffer);
					} else {
						break;
					}
				}
			}
		}

		// Update audio clock
		if (!isnan(audio->audioClock)) {
			uint32_t writtenSize = result * outputSampleStride;
			double pts = audio->audioClock - (double)(nativeFormat->periods * nativeBufferSizeInBytes + writtenSize) / (double)targetBufferSizeInBytes;
			SetClockAt(audio->clock, pts, audio->audioClockSerial, audioCallbackTime / (double)AV_TIME_BASE);
			SyncClockToSlave(state->externalClock, audio->clock);
		}

	}

	return(result);
}

static void StreamTogglePause(PlayerState *state) {
	if (state->isPaused) {
		state->frameTimer += (ffmpeg.av_gettime_relative() / (double)AV_TIME_BASE) - state->video.clock.lastUpdated;
		if (state->readPauseReturn != AVERROR(ENOSYS)) {
			state->video.clock.isPaused = false;
		}
		SetClock(state->video.clock, GetClock(state->video.clock), state->video.clock.serial);
	}
	SetClock(state->externalClock, GetClock(state->externalClock), state->externalClock.serial);
	state->isPaused = state->audio.clock.isPaused = state->video.clock.isPaused = state->externalClock.isPaused = !state->isPaused;

	if (state->isPaused) {
		// Store number of frames and current clock as pause state
		double frameRate = GetMasterFrameRate(state);
		if (isnan(frameRate) || frameRate < 0) frameRate = 0;

		double clockCurrent = fplMax(0.0, GetMasterClock(state));
		if (isnan(clockCurrent) || clockCurrent < 0) clockCurrent = 0;

		state->pauseNumFrames = frameRate != 0 ? (int)(clockCurrent / (1.0f / frameRate)) : 0;
		state->pauseClock = clockCurrent;

		fplAssert(state->pauseNumFrames >= 0);
		fplAssert(!isnan(state->pauseClock));
	}
}

static void SeekStream(PlayerState *state, int64_t pos, int64_t rel) {
	SeekState *seek = &state->seek;
	if (!seek->isRequired) {
		seek->pos = pos;
		seek->rel = rel;
		seek->seekFlags = AVSEEK_FLAG_ANY; // Seek to and frame, not just key frames
		if (state->seekByBytes)
			seek->seekFlags |= AVSEEK_FLAG_BYTE; // Some file formats does not allow to seek by seconds
		seek->isRequired = 1;
		fplSignalSet(&state->reader.resumeSignal);
	}
}

static void ToggleFullscreen(PlayerState *state) {
	if (state->isFullscreen) {
		fplSetWindowFullscreenSize(false, 0, 0, 0);
		state->isFullscreen = false;
	} else {
		state->isFullscreen = fplSetWindowFullscreenSize(true, 0, 0, 0);
	}
}

static void TogglePause(PlayerState *state) {
	StreamTogglePause(state);
	state->step = false;
}

static void StepToNextFrame(PlayerState *state) {
	if (state->isPaused) {
		StreamTogglePause(state);
	}
	state->step = 1;
}

static void PacketReadThreadProc(const fplThreadHandle *thread, void *userData) {
	PlayerState *state = (PlayerState *)userData;
	assert(state != nullptr);

	ReaderContext &reader = state->reader;
	VideoContext &video = state->video;
	AudioContext &audio = state->audio;
	MediaStream *videoStream = video.decoder.stream;
	MediaStream *audioStream = audio.decoder.stream;
	AVFormatContext *formatCtx = state->formatCtx;
	assert(formatCtx != nullptr);

	fplSignalHandle *waitSignals[] = {
		// We got a free packet for use to read into
		&reader.packetQueue.freeSignal,
		// Reader should terminate
		&reader.stopSignal,
		// Reader can continue
		&reader.resumeSignal,
	};

	bool skipWait = true;
	AVPacket srcPacket;
	bool hasPendingPacket = false;
	for (;;) {
		// Wait for any signal or skip wait
		if (!skipWait) {
			fplSignalWaitForAny(waitSignals, fplArrayCount(waitSignals), sizeof(fplSignalHandle *), FPL_TIMEOUT_INFINITE);
		} else {
			skipWait = false;
		}

		// Stop reader
		if (reader.stopRequest) {
			break;
		}

		// Pause
		if (state->isPaused != state->lastPaused) {
			state->lastPaused = state->isPaused;
			if (state->isPaused) {
				state->readPauseReturn = ffmpeg.av_read_pause(formatCtx);
			} else {
				ffmpeg.av_read_play(formatCtx);
			}
		}

		// Seeking
		if (state->seek.isRequired) {
			int64_t seekTarget = state->seek.pos;
			int64_t seekMin = state->seek.rel > 0 ? seekTarget - state->seek.rel + 2 : INT64_MIN;
			int64_t seekMax = state->seek.rel < 0 ? seekTarget - state->seek.rel - 2 : INT64_MAX;
			double seekTargetSeconds = seekTarget / (double)AV_TIME_BASE;
			double seekMinSeconds = seekMin / (double)AV_TIME_BASE;
			double seekMaxSeconds = seekMax / (double)AV_TIME_BASE;
			int seekFlags = state->seek.seekFlags;
			if (state->seek.rel < 0) {
				seekFlags |= AVSEEK_FLAG_BACKWARD;
			}
			fplDebugFormatOut("Seek to: %llu %llu %llu (%f %f %f)\n", seekMin, seekTarget, seekMax, seekMinSeconds, seekTargetSeconds, seekMaxSeconds);
			int seekResult = ffmpeg.avformat_seek_file(formatCtx, -1, seekMin, seekTarget, seekMax, seekFlags);
			if (seekResult < 0) {
				// @TODO(final): Log seek error
			} else {
				if (state->audio.stream.isValid) {
					FlushPacketQueue(state->audio.decoder.packetsQueue);
					PushFlushPacket(state->audio.decoder.packetsQueue);

					state->audio.decoder.isEOF = false;
					fplSignalSet(&state->audio.decoder.resumeSignal);
				}

				if (state->video.stream.isValid) {
					FlushPacketQueue(state->video.decoder.packetsQueue);
					PushFlushPacket(state->video.decoder.packetsQueue);

					state->video.decoder.isEOF = false;
					fplSignalSet(&state->video.decoder.resumeSignal);
				}

				if (state->seek.seekFlags & AVSEEK_FLAG_BYTE) {
					SetClock(state->externalClock, NAN, 0);
				} else {
					SetClock(state->externalClock, seekTarget / (double)AV_TIME_BASE, 0);
				}
			}
			state->seek.isRequired = false;
			reader.isEOF = false;
			if (state->isPaused) {
				StepToNextFrame(state);
			}
		}

		// @TODO(final): Handle attached pictures

		// Limit the queue?
		if ((!state->isInfiniteBuffer &&
			(audio.decoder.packetsQueue.size + video.decoder.packetsQueue.size) > MAX_PACKET_QUEUE_SIZE) ||
			(StreamHasEnoughPackets(audio.stream.stream, audio.stream.streamIndex, audio.decoder.packetsQueue) &&
				StreamHasEnoughPackets(video.stream.stream, video.stream.streamIndex, video.decoder.packetsQueue))) {
			skipWait = true;
			fplThreadSleep(10);
			continue;
		}

		//
		// Seek to the beginning when everything is done
		//
		// @TODO(final): Make this configurable
		bool autoExit = true;
		int64_t startTime = AV_NOPTS_VALUE;

		if (!state->isPaused &&
			(!state->audio.stream.isValid || (state->audio.decoder.finishedSerial == state->audio.decoder.packetsQueue.serial && GetFrameQueueRemainingCount(state->audio.decoder.frameQueue) == 0)) &&
			(!state->video.stream.isValid || (state->video.decoder.finishedSerial == state->video.decoder.packetsQueue.serial && GetFrameQueueRemainingCount(state->video.decoder.frameQueue) == 0))) {
			if ((state->loop == -1) || (state->loop > 0)) {
				if (state->loop > 0) {
					--state->loop;
				}
				SeekStream(state, startTime != AV_NOPTS_VALUE ? startTime : 0, 0);
			} else {
				if (autoExit) {
					break;
				}
			}
		}

		// Read packet
		if (!hasPendingPacket) {
			int res = ffmpeg.av_read_frame(formatCtx, &srcPacket);
			if (res < 0) {
				if ((res == AVERROR_EOF || ffmpeg.avio_feof(formatCtx->pb)) && !reader.isEOF) {
					if (video.stream.isValid) {
						PushNullPacket(video.decoder.packetsQueue, video.stream.streamIndex);
					}
					if (audio.stream.isValid) {
						PushNullPacket(audio.decoder.packetsQueue, audio.stream.streamIndex);
					}
					reader.isEOF = true;
				}
				if (formatCtx->pb != nullptr && formatCtx->pb->error) {
					// @TODO(final): Handle error
					break;
				}

				// Wait for a few milliseconds
				fplThreadSleep(10);
				skipWait = true;
				continue;
			} else {
				hasPendingPacket = true;
				reader.isEOF = false;
			}
		}

		if (hasPendingPacket) {
			// Try to get new packet from the freelist
			PacketList *targetPacket = nullptr;
			if (AquirePacket(reader.packetQueue, targetPacket)) {
				assert(targetPacket != nullptr);

#if PRINT_QUEUE_INFOS
				uint32_t packetIndex = AtomicAddU32(&reader.readPacketCount, 1);
				ConsoleFormatOut("Read packet %lu\n", packetIndex);
#endif

				// Check if packet is in play range, then queue, otherwise discard
				int64_t streamStartTime = formatCtx->streams[srcPacket.stream_index]->start_time;
				int64_t pktTimeStamp = (srcPacket.pts == AV_NOPTS_VALUE) ? srcPacket.dts : srcPacket.pts;
				double timeInSeconds = (double)(pktTimeStamp - (streamStartTime != AV_NOPTS_VALUE ? streamStartTime : 0)) * av_q2d(formatCtx->streams[srcPacket.stream_index]->time_base);
				bool pktInPlayRange = (!state->settings.duration.isValid) ||
					((timeInSeconds / (double)AV_TIME_BASE) <= ((double)state->settings.duration.value / (double)AV_TIME_BASE));

				if ((videoStream != nullptr) && (srcPacket.stream_index == videoStream->streamIndex) && pktInPlayRange) {
					AddPacketToDecoder(video.decoder, targetPacket, &srcPacket);
#if PRINT_QUEUE_INFOS
					ConsoleFormatOut("Queued video packet %lu\n", packetIndex);
#endif
				} else if ((audioStream != nullptr) && (srcPacket.stream_index == audioStream->streamIndex) && pktInPlayRange) {
					AddPacketToDecoder(audio.decoder, targetPacket, &srcPacket);
#if PRINT_QUEUE_INFOS
					ConsoleFormatOut("Queued audio packet %lu\n", packetIndex);
#endif
				} else {
#if PRINT_QUEUE_INFOS
					ConsoleFormatOut("Dropped packet %lu\n", packetIndex);
#endif
					ffmpeg.av_packet_unref(&srcPacket);
				}
				hasPendingPacket = false;
			}
			skipWait = true;
		}
	}

	FPL_LOG_INFO("App", "Reader thread stopped.\n");
}

static bool OpenStreamComponent(const char *mediaFilePath, const int32_t streamIndex, AVStream *stream, MediaStream &outStream) {
	// Get codec name
	char codecName[5] = {};
	fplMemoryCopy(&stream->codecpar->codec_tag, 4, codecName);

	// Determine codec type name
	const char *typeName;
	switch (stream->codecpar->codec_type) {
		case AVMEDIA_TYPE_VIDEO:
			typeName = "Video";
			break;
		case AVMEDIA_TYPE_AUDIO:
			typeName = "Audio";
			break;
		default:
			typeName = "";
			assert(!"Unsupported stream type!");
	}

	// Create codec context
	outStream.codecContext = ffmpeg.avcodec_alloc_context3(nullptr);
	if (ffmpeg.avcodec_parameters_to_context(outStream.codecContext, stream->codecpar) < 0) {
		FPL_LOG_ERROR("App" ,"Failed getting %s codec context from codec '%s' in media file '%s'!\n", typeName, codecName, mediaFilePath);
		return false;
	}

	// @NOTE(finaL): Set packet time base to stream time base
	outStream.codecContext->pkt_timebase = stream->time_base;

	// Find decoder
	// @TODO(final): We could force the codec here if we want (avcodec_find_decoder_by_name).
	outStream.codec = ffmpeg.avcodec_find_decoder(stream->codecpar->codec_id);
	if (outStream.codec == nullptr) {
		FPL_LOG_ERROR("App", "Unsupported %s codec '%s' in media file '%s' found!\n", typeName, codecName, mediaFilePath);
		return false;
	}

	// Open codec
	if (ffmpeg.avcodec_open2(outStream.codecContext, outStream.codec, nullptr) < 0) {
		FPL_LOG_ERROR("App", "Failed opening %s codec '%s' from media file '%s'!\n", typeName, codecName, mediaFilePath);
		return false;
	}

	// @TODO(final): Why do we need to set the discard flag to default here?
	stream->discard = AVDISCARD_DEFAULT;

	outStream.isValid = true;
	outStream.stream = stream;
	outStream.streamIndex = streamIndex;

	return true;
}

static bool IsRealTime(AVFormatContext *s) {
	if (!strcmp(s->iformat->name, "rtp") ||
		!strcmp(s->iformat->name, "rtsp") ||
		!strcmp(s->iformat->name, "sdp")) {
		return true;
	}
	if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4))) {
		return true;
	}
	return false;
}

struct DisplayRect {
	int left;
	int top;
	int right;
	int bottom;
};

static DisplayRect CalculateDisplayRect(const int screenLeft, const int screenTop, const int screenWidth, const int screenHeight, const int pictureWidth, const int pictureHeight, const AVRational pictureSAR) {
	double aspect_ratio;
	if (pictureSAR.num == 0.0) {
		aspect_ratio = 0.0;
	} else {
		aspect_ratio = av_q2d(pictureSAR);
	}
	if (aspect_ratio <= 0.0) {
		aspect_ratio = 1.0;
	}
	aspect_ratio *= (float)pictureWidth / (float)pictureHeight;

	int height = screenHeight;
	int width = lrint(height * aspect_ratio) & ~1;
	if (width > screenWidth) {
		width = screenWidth;
		height = lrint(width / aspect_ratio) & ~1;
	}
	int x = (screenWidth - width) / 2;
	int y = (screenHeight - height) / 2;
	DisplayRect result;
	result.left = screenLeft + x;
	result.top = screenTop + y;
	result.right = result.left + FFMAX(width, 1);
	result.bottom = result.top + FFMAX(height, 1);
	return(result);
}

static void RenderOSD(PlayerState *state, const Mat4f &proj, const float w, const float h) {
	ClearFontBuffer(state->fontBuffer);

	char osdTextBuffer[256];

	float osdFontSize = (float)h / 30.0f;
	float fontHeight = osdFontSize * (state->fontInfo.ascent + state->fontInfo.descent);
	float fontBaseline = osdFontSize * state->fontInfo.ascent;
	float fontLineHeight = fontHeight * 1.25f;
	Vec2f osdPos = Vec2f(0.0f, h - fontBaseline);

	const char *stateMsg = "Playing";
	double frameRate = fplMax(0.0, GetMasterFrameRate(state));
	double clockLength = fplMax(0.0, state->streamLength);
	int64_t numFrames;
	double clockCurrent;
	if (state->isPaused) {
		stateMsg = "Paused";
		numFrames = state->pauseNumFrames;
		clockCurrent = state->pauseClock;
	} else {
		clockCurrent = GetMasterClock(state);
		if (isnan(clockCurrent) || clockCurrent < 0) clockCurrent = 0;
		numFrames = frameRate != 0 ? (int64_t)(clockCurrent / (1.0f / frameRate)) : 0;
		fplAssert(numFrames >= 0);
		fplAssert(!isnan(clockCurrent));
	}

	const char *filename = fplExtractFileName(state->filePathOrUrl);

	// [State: Filename]
	fplFormatString(osdTextBuffer, fplArrayCount(osdTextBuffer), "%s: %s", stateMsg, filename);
	PushTextToBuffer(state->fontBuffer, state->fontInfo, osdTextBuffer, osdFontSize, osdPos, Vec4f(1, 1, 1, 1), TextRenderMode::Baseline);
	osdPos += Vec2f(0, -osdFontSize);

	// Round to milliseconds, we dont care about nanoseconds
	double clockCurrentSecondsRoundAsMillis = round(clockCurrent * 1000.0) / 1000.0;

	{
		int currentMillis = (int)(clockCurrentSecondsRoundAsMillis * 1000.0) % 1000;
		int currentSeconds = (int)clockCurrentSecondsRoundAsMillis % 60;
		int currentMinutes = (int)(clockCurrentSecondsRoundAsMillis / 60.0) % 60;
		int currentHours = (int)(clockCurrentSecondsRoundAsMillis / 60.0 / 60.0);

		int totalMillis = (int)(clockLength * 1000.0) % 1000;
		int totalSeconds = (int)clockLength % 60;
		int totalMinutes = (int)(clockLength / 60.0) % 60;
		int totalHours = (int)(clockLength / 60.0 / 60.0);

		fplFormatString(osdTextBuffer, fplArrayCount(osdTextBuffer), "Time: %02d:%02d:%02d:%03d", currentHours, currentMinutes, currentSeconds, currentMillis);
		PushTextToBuffer(state->fontBuffer, state->fontInfo, osdTextBuffer, osdFontSize, osdPos, Vec4f(1, 1, 1, 1), TextRenderMode::Baseline);
		osdPos += Vec2f(0, -osdFontSize);

		fplFormatString(osdTextBuffer, fplArrayCount(osdTextBuffer), "Frames: %d", numFrames);
		PushTextToBuffer(state->fontBuffer, state->fontInfo, osdTextBuffer, osdFontSize, osdPos, Vec4f(1, 1, 1, 1), TextRenderMode::Baseline);
		osdPos += Vec2f(0, -osdFontSize);

		fplFormatString(osdTextBuffer, fplArrayCount(osdTextBuffer), "Length: %02d:%02d:%02d:%03d", totalHours, totalMinutes, totalSeconds, totalMillis);
		PushTextToBuffer(state->fontBuffer, state->fontInfo, osdTextBuffer, osdFontSize, osdPos, Vec4f(1, 1, 1, 1), TextRenderMode::Baseline);
	}

#if USE_HARDWARE_RENDERING
	glBindVertexArray(state->fontBuffer.vao);
	CheckGLError();

	glActiveTexture(GL_TEXTURE0 + 0);
	glBindTexture(GL_TEXTURE_2D, state->fontBuffer.texture);
	CheckGLError();

	// Enable shader
	glUseProgram(state->fontBuffer.programId);
	CheckGLError();

	glUniformMatrix4fv(state->fontBuffer.uniform_uniViewProjMat, 1, GL_FALSE, proj.m);
	glUniform1i(state->fontBuffer.uniform_uniTexture, 0);
	CheckGLError();

	// Draw text elements
	glDrawElements(GL_TRIANGLES, state->fontBuffer.ib.count, GL_UNSIGNED_INT, nullptr);
	CheckGLError();

	// Disable shader
	glUseProgram(0);

	glActiveTexture(GL_TEXTURE0 + 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	glBindVertexArray(0);
#endif // USE_HARDWARE_RENDERING
}

static void RenderVideoFrame(PlayerState *state) {
	assert(state != nullptr);
	int readIndex = state->video.decoder.frameQueue.readIndex;
	Frame *vp = PeekFrameQueueLast(state->video.decoder.frameQueue);
	VideoContext &video = state->video;
	bool wasUploaded = false;
	if (!vp->isUploaded) {
		UploadTexture(video, vp->frame);
		vp->isUploaded = true;
		wasUploaded = true;
	}

	// Calculate display rect (Top-Down)
	int w = state->viewport.width;
	int h = state->viewport.height;
	DisplayRect rect = CalculateDisplayRect(0, 0, w, h, vp->width, vp->height, vp->sar);

#if USE_HARDWARE_RENDERING
	Mat4f proj = Mat4f::CreateOrthoRH(0.0f, (float)w, 0.0f, (float)h, 0.0f, 1.0f);

	glViewport(0, 0, w, h);
	glClear(GL_COLOR_BUFFER_BIT);

	float uMin = 0.0f;
	float vMin = 0.0f;
#if USE_GL_RECTANGLE_TEXTURES
	float uMax = (float)vp->width;
	float vMax = (float)vp->height;
#else
	float uMax = 1.0f;
	float vMax = 1.0f;
#endif

	float left = (float)rect.left;
	float right = (float)rect.right;
	float top = (float)rect.bottom;
	float bottom = (float)rect.top;

	float vertexData[4 * 4] = {
		// Top right
		right, top, uMax, vMax,
		// Top left
		left, top, uMin, vMax,
		// Bottom left
		left, bottom, uMin, vMin,
		// Bottom right
		right, bottom, uMax, vMin,
	};

#if USE_GL_BLENDING
	// Disable blending
	glDisable(GL_BLEND);
#endif

	// Update vertex array buffer with new rectangle
	glBindBuffer(GL_ARRAY_BUFFER, state->video.vertexBufferId);
	glBufferData(GL_ARRAY_BUFFER, fplArrayCount(vertexData) * sizeof(float), vertexData, GL_STREAM_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	CheckGLError();

	// Enable vertex array buffer
	glBindVertexArray(state->video.vao);
	CheckGLError();

	// Enable textures
	int textureIndices[MAX_TARGET_TEXTURE_COUNT] = {};
	for (uint32_t textureIndex = 0; textureIndex < video.targetTextureCount; ++textureIndex) {
		const VideoTexture &targetTexture = video.targetTextures[textureIndex];
		glActiveTexture(GL_TEXTURE0 + textureIndex);
		glBindTexture(targetTexture.target, targetTexture.id);
		textureIndices[textureIndex] = textureIndex;
	}
	CheckGLError();

	// Enable shader
	const VideoShader *shader = state->video.activeShader;
	glUseProgram(shader->programId);
	glUniformMatrix4fv(shader->uniform_uniProjMat, 1, GL_FALSE, proj.m);
	glUniform1iv(shader->uniform_uniTextures, (GLsizei)MAX_TARGET_TEXTURE_COUNT, textureIndices);
	glUniform1f(shader->uniform_uniTextureOffsetY, vMax);
	glUniform1f(shader->uniform_uniTextureScaleY, -1.0f);
	CheckGLError();

	// Draw quad
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
	CheckGLError();

	// Disable shader
	glUseProgram(0);

	// Disable textures
	for (int textureIndex = (int)video.targetTextureCount - 1; textureIndex >= 0; textureIndex--) {
		const VideoTexture &targetTexture = video.targetTextures[textureIndex];
		glActiveTexture(GL_TEXTURE0 + textureIndex);
		glBindTexture(targetTexture.target, 0);
	}

	glBindVertexArray(0);

#if USE_GL_BLENDING
	// Re-Enable blending
	glEnable(GL_BLEND);
#endif

	// TODO(final): OSD Support for software rendering requires bitmap blitting!
	RenderOSD(state, proj, (float)w, (float)h);

#else
	fplVideoBackBuffer *backBuffer = fplGetVideoBackBuffer();

	// TODO(final): Detect if we need to flip the frame
#if USE_FLIP_V_PICTURE_IN_SOFTWARE
	backBuffer->outputRect = fplCreateVideoRectFromLTRB(rect.left, rect.bottom, rect.right, rect.top);
#else
	backBuffer->outputRect = fplCreateVideoRectFromLTRB(rect.left, rect.top, rect.right, rect.bottom);
#endif
	backBuffer->useOutputRect = true;
#endif

	fplVideoFlip();

#if PRINT_FRAME_UPLOAD_INFOS
	ConsoleFormatOut("Displayed frame: %d(%s)\n", readIndex, (wasUploaded ? " (New)" : ""));
#endif
}

static void UpdateVideoClock(PlayerState *state, const double pts, const int32_t serial) {
	SetClock(state->video.clock, pts, serial);
	SyncClockToSlave(state->externalClock, state->video.clock);
}

static double GetFrameDuration(const PlayerState *state, const Frame *cur, const Frame *next) {
	if (cur->serial == next->serial) {
		double duration = next->pts - cur->pts;
		if (isnan(duration) || duration <= 0 || duration > state->maxFrameDuration)
			return cur->duration;
		else
			return duration;
	} else {
		return 0.0;
	}
}

static double ComputeVideoDelay(const PlayerState *state, const double delay) {
	double result = delay;
	double diff = 0.0;
	double videoClock = 0.0;
	double masterClock = 0.0;
	double syncThreshold = 0.0;
	if (GetMasterSyncType(state) != AVSyncType::VideoMaster) {
		videoClock = GetClock(state->video.clock);
		masterClock = GetMasterClock(state);
		diff = videoClock - masterClock;
		syncThreshold = fplMax(AV_SYNC_THRESHOLD_MIN, fplMin(AV_SYNC_THRESHOLD_MAX, delay));
		if (!isnan(diff) && fabs(diff) < state->maxFrameDuration) {
			if (diff <= -syncThreshold) {
				result = FFMAX(0, delay + diff);
			} else if (diff >= syncThreshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
				result = delay + diff;
			} else if (diff >= syncThreshold) {
				result = 2 * delay;
			}
		}
	}

#if PRINT_VIDEO_DELAY
	ConsoleFormatOut("video: delay=%0.3f A-V=%f\n", delay, -diff);
#endif

	return(result);
}

static void VideoRefresh(PlayerState *state, double &remainingTime, int &displayCount) {
	if (!state->isPaused && GetMasterSyncType(state) == AVSyncType::ExternalClock && state->isRealTime) {
		UpdateExternalClockSpeed(state);
	}
	if (state->video.stream.isValid) {
	retry:
		if (GetFrameQueueRemainingCount(state->video.decoder.frameQueue) > 0) {
			// Dequeue the current and the last picture
			Frame *lastvp = PeekFrameQueueLast(state->video.decoder.frameQueue);
			Frame *vp = PeekFrameQueue(state->video.decoder.frameQueue);

			// Serials from frame and packet queue must match
			if (vp->serial != state->video.decoder.packetsQueue.serial) {
				NextReadable(state->video.decoder.frameQueue);
				goto retry;
			}

			// Reset frame timer when serial from current and last frame are different
			if (lastvp->serial != vp->serial) {
				state->frameTimer = ffmpeg.av_gettime_relative() / (double)AV_TIME_BASE;
			}

			// Just display the last shown frame
			if (state->isPaused) {
				goto display;
			}

			// Compute duration and delay
			double lastDuration = GetFrameDuration(state, lastvp, vp);
			double delay = ComputeVideoDelay(state, lastDuration);

			// Compute remaining time if have time left to display the frame
			double time = ffmpeg.av_gettime_relative() / (double)AV_TIME_BASE;
			if (time < state->frameTimer + delay) {
				remainingTime = fplMin(state->frameTimer + delay - time, remainingTime);
				goto display;
			}

			// Accumulate frame timer by the computed delay
			state->frameTimer += delay;

			// Reset frame timer when frametimer is out-of-sync
			if (delay > 0 && time - state->frameTimer > AV_SYNC_THRESHOLD_MAX) {
				state->frameTimer = time;
			}

			// @TODO(final): Why do we need to lock the frame queue here?
			fplMutexLock(&state->video.decoder.frameQueue.lock);
			if (!isnan(vp->pts)) {
				UpdateVideoClock(state, vp->pts, vp->serial);
			}
			fplMutexUnlock(&state->video.decoder.frameQueue.lock);

			// When we got more than one frame we may drop this frame entirely
			if (GetFrameQueueRemainingCount(state->video.decoder.frameQueue) > 1) {
				Frame *nextvp = PeekFrameQueueNext(state->video.decoder.frameQueue);
				double duration = GetFrameDuration(state, vp, nextvp);
				if (!state->step && (state->settings.frameDrop > 0 || (state->settings.frameDrop && GetMasterSyncType(state) != AVSyncType::VideoMaster)) && time > state->frameTimer + duration) {
					state->frame_drops_late++;
					NextReadable(state->video.decoder.frameQueue);

#if PRINT_FRAME_DROPS
					FPL_LOG_INFO("App", "Frame drops: %d/%d\n", state->frame_drops_early, state->frame_drops_late);
#endif
					goto retry;
				}
			}

			// Get to next readable frame in the queue and force the refresh of the frame
			NextReadable(state->video.decoder.frameQueue);
			state->forceRefresh = 1;

			if (state->step && !state->isPaused) {
				StreamTogglePause(state);
			}
		}

	display:
		if (!state->settings.isVideoDisabled && state->forceRefresh && state->video.decoder.frameQueue.readIndexShown) {
			RenderVideoFrame(state);
			displayCount++;
		} else {
			if (state->video.decoder.frameQueue.count < state->video.decoder.frameQueue.capacity) {
				// @TODO(final): This is not great, but a fix to not wait forever in the video decoding thread
				fplSignalSet(&state->video.decoder.frameQueue.signal);
			}
		}
	}
	state->forceRefresh = 0;

#if PRINT_CLOCKS
	double masterClock = GetMasterClock(state);
	double audioClock = GetClock(state->audio.clock);
	double videoClock = GetClock(state->video.clock);
	double extClock = GetClock(state->externalClock);
	ConsoleFormatOut("M: %7.2f, A: %7.2f, V: %7.2f, E: %7.2f\n", masterClock, audioClock, videoClock, extClock);
#endif
}

static int DecodeInterruptCallback(void *opaque) {
	PlayerState *state = (PlayerState *)opaque;
	int result = state->reader.stopRequest;
	return(result);
}

static void ReleaseVideoContext(VideoContext &video) {
#if USE_HARDWARE_RENDERING
	glDeleteProgram(video.basicShader.programId);
	video.basicShader.programId = 0;
	glDeleteBuffers(1, &video.indexBufferId);
	video.indexBufferId = 0;
	glDeleteBuffers(1, &video.vertexBufferId);
	video.vertexBufferId = 0;
#endif

	for (uint32_t textureIndex = 0; textureIndex < video.targetTextureCount; ++textureIndex) {
		if (video.targetTextures[textureIndex].id) {
			DestroyVideoTexture(video.targetTextures[textureIndex]);
		}
	}
	video.targetTextureCount = 0;

	if (video.softwareScaleCtx != nullptr) {
		ffmpeg.sws_freeContext(video.softwareScaleCtx);
	}
	if (video.stream.codecContext != nullptr) {
		ffmpeg.avcodec_free_context(&video.stream.codecContext);
	}
}

static bool InitializeVideo(PlayerState &state, const char *mediaFilePath) {
	VideoContext &video = state.video;
	AVCodecContext *videoCodexCtx = video.stream.codecContext;

	// Init video decoder
	if (!InitDecoder(video.decoder, &state, &state.reader, &video.stream, MAX_VIDEO_FRAME_QUEUE_COUNT, 1)) {
		FPL_LOG_ERROR("App", "Failed initialize video decoder for media file '%s'!\n", mediaFilePath);
		return false;
	}

	AVPixelFormat targetPixelFormat;
#if USE_HARDWARE_RENDERING
	targetPixelFormat = AVPixelFormat::AV_PIX_FMT_RGBA;
#else
	targetPixelFormat = AVPixelFormat::AV_PIX_FMT_BGRA;
#endif

	// Get software context
	video.softwareScaleCtx = ffmpeg.sws_getContext(
		videoCodexCtx->width,
		videoCodexCtx->height,
		videoCodexCtx->pix_fmt,
		videoCodexCtx->width,
		videoCodexCtx->height,
		targetPixelFormat,
		SWS_BILINEAR,
		nullptr,
		nullptr,
		nullptr
	);
	if (video.softwareScaleCtx == nullptr) {
		FPL_LOG_ERROR("App", "Failed getting software scale context with size (%d x %d) for file '%s'!\n", videoCodexCtx->width, videoCodexCtx->height, mediaFilePath);
		return false;
	}

#if USE_HARDWARE_RENDERING && USE_GLSL_IMAGE_FORMAT_DECODING
	switch (videoCodexCtx->pix_fmt) {
		case AVPixelFormat::AV_PIX_FMT_YUV420P:
		{
			state.video.activeShader = &state.video.yuv420pShader;
			state.video.targetTextureCount = 3;
			if (!InitVideoTexture(state.video.targetTextures[0], videoCodexCtx->width, videoCodexCtx->height, 8)) {
				return false;
			}
			if (!InitVideoTexture(state.video.targetTextures[1], videoCodexCtx->width / 2, videoCodexCtx->height / 2, 8)) {
				return false;
			}
			if (!InitVideoTexture(state.video.targetTextures[2], videoCodexCtx->width / 2, videoCodexCtx->height / 2, 8)) {
				return false;
			}
		} break;
		default:
		{
			state.video.activeShader = &state.video.basicShader;
			state.video.targetTextureCount = 1;
			if (!InitVideoTexture(state.video.targetTextures[0], videoCodexCtx->width, videoCodexCtx->height, 32)) {
				return false;
			}
		} break;
	}
#else
#	if USE_HARDWARE_RENDERING
	state.video.activeShader = &state.video.basicShader;
#	endif
	state.video.targetTextureCount = 1;
	if (!InitVideoTexture(state.video.targetTextures[0], videoCodexCtx->width, videoCodexCtx->height, 32)) {
		return false;
	}
#endif

#if USE_HARDWARE_RENDERING
	// Allocate VAO
	glGenVertexArrays(1, &state.video.vao);
	glBindVertexArray(state.video.vao);
	CheckGLError();

	// Allocate 4 vertices
	uint32_t verticesSize = 4 * 4 * sizeof(float);
	glGenBuffers(1, &state.video.vertexBufferId);
	glBindBuffer(GL_ARRAY_BUFFER, state.video.vertexBufferId);
	glBufferData(GL_ARRAY_BUFFER, verticesSize, nullptr, GL_STREAM_DRAW);
	CheckGLError();

	uint32_t indicesSize = fplArrayCount(videoQuadIndices) * sizeof(*videoQuadIndices);
	glGenBuffers(1, &state.video.indexBufferId);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, state.video.indexBufferId);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, indicesSize, videoQuadIndices, GL_STATIC_DRAW);
	CheckGLError();

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (GLvoid *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (GLvoid *)(sizeof(float) * 2));
	CheckGLError();

	glBindVertexArray(0);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	if (!LoadVideoShader(state.video.basicShader, BasicShaderSource::Vertex, BasicShaderSource::Fragment, BasicShaderSource::Name)) {
		return false;
	}
	if (!LoadVideoShader(state.video.yuv420pShader, YUV420PShaderSource::Vertex, YUV420PShaderSource::Fragment, YUV420PShaderSource::Name)) {
		return false;
	}

	CheckGLError();
#endif

	state.frameTimer = 0.0;
	state.frameLastPTS = 0.0;
	state.frameLastDelay = 40e-3;

	return true;
}

static void ReleaseAudio(AudioContext &audio) {
	if (audio.conversionAudioBuffer != nullptr) {
		fplMemoryAlignedFree(audio.conversionAudioBuffer);
		audio.conversionAudioBuffer = nullptr;
	}
	if (audio.softwareResampleCtx != nullptr) {
		ffmpeg.swr_free(&audio.softwareResampleCtx);
	}
	if (audio.stream.codecContext != nullptr) {
		ffmpeg.avcodec_free_context(&audio.stream.codecContext);
	}
}

static bool InitializeAudio(PlayerState &state, const char *mediaFilePath, const fplAudioDeviceFormat &nativeAudioFormat) {
	AudioContext &audio = state.audio;
	AVCodecContext *audioCodexCtx = audio.stream.codecContext;

	// Init audio decoder
	if (!InitDecoder(audio.decoder, &state, &state.reader, &audio.stream, MAX_AUDIO_FRAME_QUEUE_COUNT, 1)) {
		FPL_LOG_ERROR("App", "Failed initialize audio decoder for media file '%s'!\n", mediaFilePath);
		return false;
	}

	if ((state.formatCtx->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !state.formatCtx->iformat->read_seek) {
		audio.decoder.start_pts = audio.stream.stream->start_time;
		audio.decoder.start_pts_tb = audio.stream.stream->time_base;
	}

	uint32_t nativeBufferSizeInBytes = fplGetAudioBufferSizeInBytes(nativeAudioFormat.type, nativeAudioFormat.channels, nativeAudioFormat.bufferSizeInFrames);

	AVSampleFormat targetSampleFormat = MapAudioFormatType(nativeAudioFormat.type);
	// @TODO(final): Map target audio channels to channel layout
	int targetChannelCount = nativeAudioFormat.channels;
	uint64_t targetChannelLayout = AV_CH_LAYOUT_STEREO;
	assert(targetChannelCount == 2);
	int targetSampleRate = nativeAudioFormat.sampleRate;
	audio.audioTarget = {};
	audio.audioTarget.periods = nativeAudioFormat.periods;
	audio.audioTarget.channels = targetChannelCount;
	audio.audioTarget.sampleRate = targetSampleRate;
	audio.audioTarget.type = nativeAudioFormat.type;
	audio.audioTarget.bufferSizeInFrames = ffmpeg.av_samples_get_buffer_size(nullptr, audio.audioTarget.channels, 1, targetSampleFormat, 1);
	uint32_t targetBufferSizeInBytes = fplGetAudioBufferSizeInBytes(audio.audioTarget.type, audio.audioTarget.channels, audio.audioTarget.bufferSizeInFrames);

	AVSampleFormat inputSampleFormat = audioCodexCtx->sample_fmt;
	int inputChannelCount = audioCodexCtx->channels;
	// @TODO(final): Map input audio channels to channel layout
	uint64_t inputChannelLayout = AV_CH_LAYOUT_STEREO;
	int inputSampleRate = audioCodexCtx->sample_rate;
	assert(inputChannelCount == 2);
	audio.audioSource = {};
	audio.audioSource.channels = inputChannelCount;
	audio.audioSource.sampleRate = inputSampleRate;
	audio.audioSource.type = MapAVSampleFormat(inputSampleFormat);
	audio.audioSource.periods = nativeAudioFormat.periods;
	audio.audioSource.bufferSizeInFrames = ffmpeg.av_samples_get_buffer_size(nullptr, inputChannelCount, 1, inputSampleFormat, 1);
	uint32_t sourceBufferSizeInBytes = fplGetAudioBufferSizeInBytes(audio.audioSource.type, audio.audioSource.channels, audio.audioSource.bufferSizeInFrames);

	// Compute AVSync audio threshold
	audio.audioDiffAbgCoef = exp(log(0.01) / AV_AUDIO_DIFF_AVG_NB);
	audio.audioDiffAvgCount = 0;
	audio.audioDiffThreshold = nativeBufferSizeInBytes / (double)targetBufferSizeInBytes;

	// Create software resample context and initialize
	audio.softwareResampleCtx = ffmpeg.swr_alloc_set_opts(nullptr,
		targetChannelLayout,
		targetSampleFormat,
		targetSampleRate,
		inputChannelLayout,
		inputSampleFormat,
		inputSampleRate,
		0,
		nullptr);
	ffmpeg.swr_init(audio.softwareResampleCtx);

	// Allocate conversion buffer in native format, this must be big enough to hold one AVFrame worth of data.
	int lineSize;
	audio.maxConversionAudioBufferSize = ffmpeg.av_samples_get_buffer_size(&lineSize, targetChannelCount, targetSampleRate, targetSampleFormat, 1);
	audio.maxConversionAudioFrameCount = audio.maxConversionAudioBufferSize / fplGetAudioSampleSizeInBytes(nativeAudioFormat.type) / targetChannelCount;
	audio.conversionAudioBuffer = (uint8_t *)fplMemoryAlignedAllocate(audio.maxConversionAudioBufferSize, 16);
	audio.conversionAudioFrameIndex = 0;
	audio.conversionAudioFramesRemaining = 0;

	return true;
}

static void ReleaseMedia(PlayerState &state) {
	DestroyDecoder(state.audio.decoder);
	ReleaseAudio(state.audio);
	DestroyDecoder(state.video.decoder);
	ReleaseVideoContext(state.video);
	DestroyReader(state.reader);
	if (state.formatCtx != nullptr) {
		ffmpeg.avformat_close_input(&state.formatCtx);
	}
}

static bool LoadMedia(PlayerState &state, const char *mediaFilePath, const fplAudioDeviceFormat &nativeAudioFormat) {
	// @TODO(final): Custom IO!

	// Open media file
	if (ffmpeg.avformat_open_input(&state.formatCtx, mediaFilePath, nullptr, nullptr) != 0) {
		FPL_LOG_ERROR("App", "Failed opening media file '%s'!\n", mediaFilePath);
		goto release;
	}

	state.streamLength = state.formatCtx->duration / (double)AV_TIME_BASE;

	state.formatCtx->interrupt_callback.callback = DecodeInterruptCallback;
	state.formatCtx->interrupt_callback.opaque = &state;

	// Retrieve stream information
	if (ffmpeg.avformat_find_stream_info(state.formatCtx, nullptr) < 0) {
		FPL_LOG_ERROR("App", "Failed getting stream informations for media file '%s'!\n", mediaFilePath);
		goto release;
	}

	// Dump information about file into standard error
	ffmpeg.av_dump_format(state.formatCtx, 0, mediaFilePath, 0);

	// Dont limit the queues when we are playing realtime based media, like internet streams, etc.
	state.isRealTime = IsRealTime(state.formatCtx);
	if (!state.isInfiniteBuffer && state.isRealTime) {
		state.isInfiniteBuffer = true;
	}

	// Find the first streams
	state.video.stream.streamIndex = -1;
	state.audio.stream.streamIndex = -1;
	for (uint32_t streamIndex = 0; streamIndex < state.formatCtx->nb_streams; streamIndex++) {
		AVStream *stream = state.formatCtx->streams[streamIndex];
		switch (stream->codecpar->codec_type) {
			case AVMEDIA_TYPE_VIDEO:
			{
				if (state.video.stream.streamIndex == -1 && !state.settings.isVideoDisabled) {
					OpenStreamComponent(mediaFilePath, streamIndex, stream, state.video.stream);
				}
			} break;
			case AVMEDIA_TYPE_AUDIO:
			{
				if (state.audio.stream.streamIndex == -1 && !state.settings.isAudioDisabled) {
					OpenStreamComponent(mediaFilePath, streamIndex, stream, state.audio.stream);
				}
			} break;
		}
	}

	// No streams found
	if ((!state.video.stream.isValid) && (!state.audio.stream.isValid)) {
		FPL_LOG_ERROR("App", "No video or audio stream in media file '%s' found!\n", mediaFilePath);
		goto release;
	}

	// We need to initialize the reader first before we allocate stream specific stuff
	if (!InitReader(state.reader)) {
		FPL_LOG_ERROR("App", "Failed initializing reader file '%s'!\n", mediaFilePath);
		goto release;
	}

	// Allocate audio related resources
	if (state.audio.stream.isValid) {
		if (!InitializeAudio(state, mediaFilePath, nativeAudioFormat)) {
			goto release;
		}
	}

	// Allocate video related resources
	if (state.video.stream.isValid) {
		if (!InitializeVideo(state, mediaFilePath)) {
			goto release;
		}
	}

	// Init timings
	state.maxFrameDuration = (state.formatCtx->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
	InitClock(state.video.clock, &state.video.decoder.packetsQueue.serial);
	InitClock(state.audio.clock, &state.audio.decoder.packetsQueue.serial);
	InitClock(state.externalClock, &state.externalClock.serial);
	state.audio.audioClockSerial = -1;

	// Seek init
	state.seekByBytes = !!(state.formatCtx->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", state.formatCtx->iformat->name);

	// Save file path
	state.filePathOrUrl = mediaFilePath;

	return true;

release:
	ReleaseMedia(state);
	return false;
}

static void SeekRelative(PlayerState *state, double incr) {
	// TODO(tspaete): Make this operation thread-safe
	const AVStream *stream = GetMasterStream(state);
	if (stream != nullptr) {
		double pos = GetMasterClock(state);
		if (isnan(pos)) {
			pos = (double)state->seek.pos / AV_TIME_BASE;
		}
		pos += incr;
		double start = state->formatCtx->start_time / (double)AV_TIME_BASE;
		if ((state->formatCtx->start_time != AV_NOPTS_VALUE) && (pos < start)) {
			pos = start;
		}
		SeekStream(state, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE));
	}
}

int main(int argc, char **argv) {
	int result = 0;

	if (argc < 2) {
		FPL_LOG_CRITICAL("App", "Media file argument missing!");
		return -1;
	}

	fplArchType arch = fplGetProcessorArchitecture();
	if (!(arch == fplArchType_x64 || arch == fplArchType_x86_64)) {
		FPL_LOG_CRITICAL("App", "x64 architecture is required to run this demo!");
		return -1;
	}

	const char *mediaFilePath = argv[1];

	fplSettings settings = fplMakeDefaultSettings();

	fplCopyString("FPL FFmpeg Demo", settings.window.title, fplArrayCount(settings.window.title));
#if USE_HARDWARE_RENDERING
	settings.video.driver = fplVideoDriverType_OpenGL;
	settings.video.graphics.opengl.compabilityFlags = fplOpenGLCompabilityFlags_Core;
	settings.video.graphics.opengl.majorVersion = 3;
	settings.video.graphics.opengl.minorVersion = 3;
#else
	settings.video.driver = fplVideoDriverType_Software;
#endif
	settings.video.isAutoSize = false;
	settings.video.isVSync = false;

	if (!fplPlatformInit(fplInitFlags_All, &settings)) {
		return -1;
	}

#if USE_HARDWARE_RENDERING
	if (!fglLoadOpenGL(true)) {
		fplPlatformRelease();
		return -1;
	}
#endif

	PlayerState state = {};

	// Init
	if (!InitPlayer(state)) {
		goto release;
	}

	// Get native audio format
	fplAudioDeviceFormat nativeAudioFormat;
	if (!fplGetAudioHardwareFormat(&nativeAudioFormat)) {
		goto release;
	}

	// Load ffmpeg libraries
	if (!LoadFFMPEG(ffmpeg)) {
		goto release;
	}

	// Init flush packet
	ffmpeg.av_init_packet(&globalFlushPacket);
	globalFlushPacket.data = (uint8_t *)&globalFlushPacket;

	// Load media
	if (!LoadMedia(state, mediaFilePath, nativeAudioFormat)) {
		goto release;
	}

	// Start decoder and reader
	if (state.video.stream.isValid) {
		StartDecoder(state.video.decoder, VideoDecodingThreadProc);
	}
	if (state.audio.stream.isValid) {
		StartDecoder(state.audio.decoder, AudioDecodingThreadProc);
	}
	StartReader(state.reader, PacketReadThreadProc, &state);

	// Start playing audio
	if (state.audio.stream.isValid) {
		fplSetAudioClientReadCallback(AudioReadCallback, &state.audio);
		fplPlayAudio();
	}

	//
	// App loop
	//
	fplGetWindowSize(&state.viewport);
	double lastTime = fplGetTimeInSecondsHP();
	double remainingTime = 0.0;
	double lastRefreshTime = fplGetTimeInSecondsHP();
	int refreshCount = 0;
	while (fplWindowUpdate()) {
		//
		// Handle events
		//

		// TODO: Make constant!
		const double SeekStep = 5.0f;

		fplEvent ev = {};
		while (fplPollEvent(&ev)) {
			switch (ev.type) {
				case fplEventType_Keyboard:
				{
					if (ev.keyboard.type == fplKeyboardEventType_Button) {
						if (ev.keyboard.buttonState == fplButtonState_Release) {
							switch (ev.keyboard.mappedKey) {
								case fplKey_Space:
								{
									TogglePause(&state);
								} break;

								case fplKey_F:
								{
									ToggleFullscreen(&state);
								} break;

								case fplKey_Left:
								case fplKey_Right:
								{
									// TODO(final): Make seeking thread-safe!
									double seekRelative = (ev.keyboard.mappedKey == fplKey_Left) ? -SeekStep : SeekStep;
									SeekRelative(&state, seekRelative);
								} break;
							}
						}
					}
				} break;
				case fplEventType_Window:
				{
					if (ev.window.type == fplWindowEventType_Resized) {
						state.viewport.width = ev.window.size.width;
						state.viewport.height = ev.window.size.height;
						state.forceRefresh = 1;
					}
				} break;
			}
		}

		//
		// Refresh video
		//
		if (remainingTime > 0.0) {
			uint32_t msToSleep = (uint32_t)(remainingTime * 1000.0);
			fplThreadSleep(msToSleep);
		}
		remainingTime = DEFAULT_REFRESH_RATE;
		if (!state.isPaused || state.forceRefresh) {
			VideoRefresh(&state, remainingTime, refreshCount);
#if PRINT_VIDEO_REFRESH
			fplDebugFormatOut("Video refresh: %d\n", ++refreshCount);
#endif
		} else {
			RenderVideoFrame(&state);
		}

		// Update time
		double now = fplGetTimeInSecondsHP();
		double refreshDelta = now - lastRefreshTime;
		if (refreshDelta >= 1.0) {
			lastRefreshTime = now;
#if PRINT_FPS
			fplDebugFormatOut("FPS: %d\n", refreshCount);
#endif
			refreshCount = 0;
		}
		double delta = now - lastTime;
		lastTime = now;
#if PRINT_MEMORY_STATS
		PrintMemStats();
#endif
	}


release:
	// Stop audio
	if (state.audio.stream.isValid) {
		fplStopAudio();
	}

	// Stop reader and decoders
	StopReader(state.reader);
	if (state.video.stream.isValid) {
		StopDecoder(state.video.decoder);
	}
	if (state.audio.stream.isValid) {
		StopDecoder(state.audio.decoder);
	}

	ReleaseMedia(state);
	ReleaseFFMPEG(ffmpeg);
	ReleasePlayer(state);

	// Release platform
#if USE_HARDWARE_RENDERING
	fglUnloadOpenGL();
#endif
	fplPlatformRelease();

	return(0);
}