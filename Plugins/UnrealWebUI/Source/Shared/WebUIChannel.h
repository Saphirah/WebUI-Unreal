#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace WebUIIPC {
// Dedicated blocking I/O threads. Send never waits for the peer or for a reply.
// The random, local-only pipe name is supplied by the parent process.
class Channel {
public:
    using Receiver = std::function<void(std::string)>;
    std::atomic<bool> connected{false};
    bool Send(std::string message) {
        if (message.size() > MaxPacket || stopping) return false;
        { std::lock_guard<std::mutex> lock(mutex);
          if (stopping || queuedBytes + message.size() > MaxQueue) return false;
          queuedBytes += message.size(); queue.push_back(std::move(message)); }
        wake.notify_all(); return true;
    }
    void Start(std::wstring name, bool server, Receiver receiver) {
        reader = std::thread([this, name, server, receiver] {
            HANDLE handle = INVALID_HANDLE_VALUE;
            if (server) {
                handle = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                    1, 65536, 65536, 0, nullptr);
                pipe = handle;
                if(handle==INVALID_HANDLE_VALUE)return;
                OVERLAPPED operation{};operation.hEvent=CreateEventW(nullptr,true,false,nullptr);
                bool ok=ConnectNamedPipe(handle,&operation)!=0;
                if(!ok){const DWORD error=GetLastError();if(error==ERROR_PIPE_CONNECTED)ok=true;
                    else if(error==ERROR_IO_PENDING){DWORD count=0;ok=Complete(handle,operation,count);}}
                CloseHandle(operation.hEvent);if(!ok)return;
            } else {
                for (int i=0; i<200 && !stopping; ++i) {
                    handle = CreateFileW(name.c_str(), GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                    if (handle != INVALID_HANDLE_VALUE) break;
                    Sleep(25);
                }
                pipe = handle;
                if (handle == INVALID_HANDLE_VALUE) return;
            }
            {std::lock_guard<std::mutex> lock(mutex);connected=true;}wake.notify_all();
            while (!stopping) {
                uint32_t length = 0;
                if (!ReadAll(handle, &length, sizeof(length)) || length > MaxPacket) break;
                std::string message(length, '\0');
                if (!ReadAll(handle, message.data(), length)) break;
                receiver(std::move(message));
            }
            {std::lock_guard<std::mutex> lock(mutex);connected=false;stopping=true;}wake.notify_all();
        });
        writer = std::thread([this] {
            while (!stopping) {
                std::string message;
                { std::unique_lock<std::mutex> lock(mutex);
                  wake.wait(lock, [this] { return stopping || (connected && !queue.empty()); });
                  if (stopping) break;
                  message = std::move(queue.front()); queue.pop_front(); queuedBytes -= message.size(); }
                const uint32_t length = static_cast<uint32_t>(message.size());
                const HANDLE handle = pipe;
                if (!WriteAll(handle, &length, sizeof(length)) || !WriteAll(handle, message.data(), length)) {
                    {std::lock_guard<std::mutex> lock(mutex);stopping=true;connected=false;}wake.notify_all();break;
                }
            }
        });
    }
    void Stop() {
        {std::lock_guard<std::mutex> lock(mutex);stopping=true;}wake.notify_all();
        const HANDLE active=pipe;if(active!=INVALID_HANDLE_VALUE)CancelIoEx(active,nullptr);
        if (reader.joinable()) CancelSynchronousIo(reader.native_handle());
        if (writer.joinable()) CancelSynchronousIo(writer.native_handle());
        if (reader.joinable()) reader.join();
        if (writer.joinable()) writer.join();
        const HANDLE handle = pipe.exchange(INVALID_HANDLE_VALUE);
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        connected = false;
    }
    ~Channel() { Stop(); }
private:
    static constexpr uint32_t MaxPacket = 16 * 1024 * 1024;
    static constexpr size_t MaxQueue = 64 * 1024 * 1024;
    std::atomic<HANDLE> pipe{INVALID_HANDLE_VALUE};
    std::atomic<bool> stopping{false};
    std::thread reader, writer;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> queue;
    size_t queuedBytes = 0;
    bool Complete(HANDLE h,OVERLAPPED& operation,DWORD& count){
        DWORD result;
        do{result=WaitForSingleObject(operation.hEvent,100);}while(result==WAIT_TIMEOUT&&!stopping);
        if(result!=WAIT_OBJECT_0)CancelIoEx(h,&operation);
        return GetOverlappedResult(h,&operation,&count,true)!=0;
    }
    bool Transfer(HANDLE h,void* buffer,uint32_t length,bool write){
        auto* data=static_cast<char*>(buffer);
        OVERLAPPED operation{};operation.hEvent=CreateEventW(nullptr,true,false,nullptr);
        bool ok=true;
        while(length&&!stopping){
            ResetEvent(operation.hEvent);DWORD count=0;
            ok=(write?WriteFile(h,data,length,&count,&operation):ReadFile(h,data,length,&count,&operation))!=0;
            if(!ok&&GetLastError()==ERROR_IO_PENDING)ok=Complete(h,operation,count);
            if(!ok||!count){ok=false;break;}data+=count;length-=count;
        }
        CloseHandle(operation.hEvent);return ok&&length==0;
    }
    bool ReadAll(HANDLE h,void* buffer,uint32_t length){return Transfer(h,buffer,length,false);}
    bool WriteAll(HANDLE h,const void* buffer,uint32_t length){return Transfer(h,const_cast<void*>(buffer),length,true);}
};
}
