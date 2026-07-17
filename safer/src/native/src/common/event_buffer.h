/*
 * event_buffer.h - Ring buffer declarations
 */

#pragma once

#include <napi.h>
#include <vector>
#include <mutex>

struct RingBuffer {
    std::vector<uint8_t> buffer;
    size_t writePos;
    size_t readPos;
    size_t capacity;
    std::mutex mtx;

    RingBuffer(size_t size = 65536)
        : buffer(size, 0), writePos(0), readPos(0), capacity(size) {}

    bool Enqueue(const void* data, size_t size);
    bool Dequeue(void* out, size_t size);
    size_t GetUsed();
    size_t GetAvailable();
    bool IsFull();
};

Napi::Object InitEventBufferAddon(Napi::Env env, Napi::Object exports);
Napi::Value BufferEnqueue(const Napi::CallbackInfo& info);
Napi::Value BufferDequeue(const Napi::CallbackInfo& info);
