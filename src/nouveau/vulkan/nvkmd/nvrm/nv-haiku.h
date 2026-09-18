#pragma once

#include <OS.h>
#include <drivers/Drivers.h>


#define NVIDIA_CONTROL_DEVICE_NAME "nvidiactl"
#define NVIDIA_DEVICE_NAME "graphics/nvidia"


enum {
	NV_HAIKU_BASE = B_DEVICE_OP_CODES_END + 1,
};

enum {
	NV_HAIKU_GET_COOKIE = 0, // kernel only
	NV_HAIKU_MAP,
	NV_HAIKU_READ_REGISTER, // development aid
	NV_HAIKU_READ_VRAM, // development aid
	NV_HAIKU_WAIT_FOR_RESUME,
	NV_HAIKU_PUBLISH_SCANOUT,
	NV_HAIKU_GET_SCANOUT,
};

// NV_HAIKU_PUBLISH_SCANOUT / NV_HAIKU_GET_SCANOUT: the accelerant says where
// the screen's frame buffer lives, so that a program drawing with the GPU can
// put a finished frame there itself instead of sending it through the host.
// The memory is shared by its owner, and the caller duplicates the handle into
// its own resman client.
typedef struct {
	uint32 client;			// resman client that owns the memory
	uint32 memory;			// the memory object
	uint32 width;
	uint32 height;
	uint32 bytes_per_row;
	uint32 color_space;		// Haiku color_space of the frame buffer
	uint64 size;
} nv_haiku_scanout_info;

// NV_HAIKU_WAIT_FOR_RESUME: waits until the resume generation differs from
// the passed one (or the timeout expires) and returns the current one. Display
// state is lost when resuming from suspend, so clients restore their modes
// when the generation changes.
typedef struct {
	uint32 generation;
	uint32 reserved;
	int64 timeout;
} nv_haiku_resume_params;

typedef struct {
	uint64 offset;
	uint32 values[16];
} nv_haiku_vram_params;

typedef struct {
	uint32 offset;
	uint32 value;
} nv_haiku_register_params;


typedef struct {
	char name[B_OS_NAME_LENGTH];
	void *address;
	uint32 addressSpec;
	uint32 protection;
} nv_haiku_map_params;
