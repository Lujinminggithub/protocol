/*
 * event_buffer.cpp - 环形缓冲区实现
 */

#include "event_buffer.h"
#include <cstring>

/*++
 * RingBuffer::Enqueue
 *
 * 向环形缓冲区写入数据
 *
 * --*/
bool RingBuffer::Enqueue(const void* data, size_t size) {
    std::lock_guard<std::mutex> lock(mtx);

    if (size > capacity) return false;

    size_t available = capacity - writePos;
    size_t firstPart = std::min(size, available);

    std::memcpy(&buffer[writePos], data, firstPart);
    writePos += firstPart;

    // 处理环绕
    if (writePos >= capacity) {
        size_t remaining = size - firstPart;
        std::memcpy(buffer.data(), static_cast<const uint8_t*>(data) + firstPart, remaining);
        writePos = remaining;
    }

    return true;
}

/*++
 * RingBuffer::Dequeue
 *
 * 从环形缓冲区读取数据
 *
 * --*/
bool RingBuffer::Dequeue(void* out, size_t size) {
    std::lock_guard<std::mutex> lock(mtx);

    size_t available = capacity - readPos;
    size_t firstPart = std::min(size, available);

    std::memcpy(out, &buffer[readPos], firstPart);
    readPos += firstPart;

    if (readPos >= capacity) {
        size_t remaining = size - firstPart;
        std::memcpy(static_cast<uint8_t*>(out) + firstPart, buffer.data(), remaining);
        readPos = remaining;
    }

    return true;
}

size_t RingBuffer::GetUsed() {
    std::lock_guard<std::mutex> lock(mtx);
    if (writePos >= readPos) return writePos - readPos;
    return capacity - readPos + writePos;
}

size_t RingBuffer::GetAvailable() {
    return capacity - GetUsed();
}

bool RingBuffer::IsFull() {
    return GetAvailable() == 0;
}

/*++
 * BufferEnqueue
 *
 * N-API 导出: 向缓冲区写入数据
 *
 * --*/
Napi::Value BufferEnqueue(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // TODO: 使用 Napi::Buffer 获取数据
    return Napi::Boolean::New(env, false);
}

/*++
 * BufferDequeue
 *
 * N-API 导出: 从缓冲区读取数据
 *
 * --*/
Napi::Value BufferDequeue(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    // TODO: 返回 Napi::Buffer
    return env.Undefined();
}

/*++
 * InitEventBufferAddon
 *
 * 初始化环形缓冲区模块
 *
 * --*/
Napi::Object InitEventBufferAddon(Napi::Env env, Napi::Object exports) {
    exports.Set("enqueue", Napi::Function::New(env, BufferEnqueue));
    exports.Set("dequeue", Napi::Function::New(env, BufferDequeue));
    return exports;
}
