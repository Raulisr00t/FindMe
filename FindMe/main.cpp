#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

typedef LONG NTSTATUS;

#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)

struct NT_IO_STATUS_BLOCK
{
    union
    {
        NTSTATUS Status;
        PVOID    Pointer;
    };

    ULONG_PTR Information;
};

struct FILE_DIR_INFORMATION
{
    ULONG         NextEntryOffset;
    ULONG         FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG         FileAttributes;
    ULONG         FileNameLength;
    WCHAR         FileName[1];
};

typedef NTSTATUS(NTAPI* FN_NtQueryDirectoryFile)(
    HANDLE, HANDLE, PVOID, PVOID,
    NT_IO_STATUS_BLOCK*,
    PVOID, ULONG, ULONG,
    BOOLEAN, PVOID, BOOLEAN
);

static FN_NtQueryDirectoryFile pNtQueryDirectoryFile;

struct PathRef
{
    wchar_t* data;
    uint32_t length;
};


class PathArena
{
private:

    struct Block
    {
        size_t   used;
        size_t   capacity;
        Block*   next;
        wchar_t  data[];
    };

    Block*     head = nullptr;
    std::mutex mutex;

    static constexpr size_t BLOCK_CHARS = 65536;

    wchar_t* Alloc(size_t chars)
    {
        const size_t needed = chars + 1;

        if (!head || head->used + needed > head->capacity)
        {
            const size_t cap =
                (needed > BLOCK_CHARS) ? needed : BLOCK_CHARS;

            auto* block = static_cast<Block*>(
                malloc(sizeof(Block) + cap * sizeof(wchar_t))
            );

            block->used     = 0;
            block->capacity = cap;
            block->next     = head;

            head = block;
        }

        wchar_t* ptr = head->data + head->used;

        head->used += needed;

        return ptr;
    }

public:

    PathArena() = default;

    ~PathArena()
    {
        while (head)
        {
            Block* next = head->next;

            free(head);

            head = next;
        }
    }

    PathArena(const PathArena&) = delete;
    PathArena& operator=(const PathArena&) = delete;


    PathRef Store(const wchar_t* str, size_t len)
    {
        std::lock_guard<std::mutex> lock(mutex);

        wchar_t* dest = Alloc(len);

        wmemcpy(dest, str, len);
        dest[len] = L'\0';

        return {
            dest,
            static_cast<uint32_t>(len)
        };
    }


    PathRef BuildChild(
        const wchar_t* dir,  size_t dirLen,
        const wchar_t* name, size_t nameLen)
    {
        const size_t total = dirLen + 1 + nameLen;

        std::lock_guard<std::mutex> lock(mutex);

        wchar_t* dest = Alloc(total);

        wmemcpy(dest, dir, dirLen);
        dest[dirLen] = L'\\';
        wmemcpy(dest + dirLen + 1, name, nameLen);
        dest[total] = L'\0';

        return {
            dest,
            static_cast<uint32_t>(total)
        };
    }
};


class DirectoryQueue
{
private:

    std::deque<PathRef> queue;

    std::mutex          mutex;
    std::condition_variable cv;

    uint64_t pending  = 0;
    bool     shutdown = false;

public:

    void Push(PathRef path)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);

            queue.push_back(path);
            ++pending;
        }

        cv.notify_one();
    }


    bool Pop(PathRef& path)
    {
        std::unique_lock<std::mutex> lock(mutex);

        cv.wait(lock, [&]
            {
                return shutdown || !queue.empty();
            });

        if (shutdown && queue.empty())
            return false;

        path = queue.front();
        queue.pop_front();

        return true;
    }


    void Complete()
    {
        std::lock_guard<std::mutex> lock(mutex);

        --pending;

        if (pending == 0)
        {
            shutdown = true;
            cv.notify_all();
        }
    }


    void WaitForCompletion()
    {
        std::unique_lock<std::mutex> lock(mutex);

        cv.wait(lock, [&]
            {
                return pending == 0;
            });
    }
};

class FileSearcher
{
private:

    const wchar_t* target;
    size_t         targetLength;

    DirectoryQueue directories;
    PathArena      arena;

    std::atomic<uint64_t> matches{ 0 };
    std::atomic<uint64_t> directoriesScanned{ 0 };
    std::atomic<uint64_t> filesScanned{ 0 };

    std::mutex outputMutex;

private:

    static bool IsDotEntry(
        const wchar_t* name,
        ULONG          nameBytes)
    {
        const ULONG chars = nameBytes / sizeof(wchar_t);

        if (chars == 1 && name[0] == L'.')
            return true;

        return
            chars == 2 &&
            name[0] == L'.' &&
            name[1] == L'.';
    }


    void PrintMatch(
        const PathRef& directory,
        const wchar_t* filename,
        ULONG          filenameChars)
    {
        std::lock_guard<std::mutex> lock(outputMutex);

        wprintf(
            L"[+] Found: %ls\\%.*ls\n",
            directory.data,
            static_cast<int>(filenameChars),
            filename
        );
    }


    void ScanDirectory(const PathRef& directory)
    {
        ++directoriesScanned;

        HANDLE hDir = CreateFileW(
            directory.data,
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr
        );

        if (hDir == INVALID_HANDLE_VALUE)
        {
            directories.Complete();
            return;
        }

        alignas(8) BYTE buffer[65536];
        NT_IO_STATUS_BLOCK iosb{};
        BOOLEAN restart = TRUE;

        for (;;)
        {
            const NTSTATUS status = pNtQueryDirectoryFile(
                hDir,
                nullptr,
                nullptr,
                nullptr,
                &iosb,
                buffer,
                sizeof(buffer),
                1,       // FileDirectoryInformation
                FALSE,   // ReturnSingleEntry
                nullptr, // FileName filter
                restart
            );

            restart = FALSE;

            if (!NT_SUCCESS(status))
                break;

            auto* entry =
                reinterpret_cast<FILE_DIR_INFORMATION*>(buffer);

            for (;;)
            {
                const wchar_t* name      = entry->FileName;
                const ULONG    nameBytes  = entry->FileNameLength;
                const ULONG    nameChars  = nameBytes / sizeof(wchar_t);
                const DWORD    attributes = entry->FileAttributes;

                if (!IsDotEntry(name, nameBytes))
                {
                    if (attributes & FILE_ATTRIBUTE_DIRECTORY)
                    {
                        if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                        {
                            directories.Push(
                                arena.BuildChild(
                                    directory.data, directory.length,
                                    name, nameChars
                                )
                            );
                        }
                    }
                    else
                    {
                        ++filesScanned;

                        if (nameChars == targetLength &&
                            _wcsnicmp(name, target, targetLength) == 0)
                        {
                            ++matches;

                            PrintMatch(
                                directory,
                                name,
                                nameChars
                            );
                        }
                    }
                }

                if (entry->NextEntryOffset == 0)
                    break;

                entry = reinterpret_cast<FILE_DIR_INFORMATION*>(
                    reinterpret_cast<BYTE*>(entry) +
                    entry->NextEntryOffset
                );
            }
        }

        CloseHandle(hDir);

        directories.Complete();
    }


    void Worker()
    {
        PathRef directory{};

        while (directories.Pop(directory))
        {
            ScanDirectory(directory);
        }
    }


public:

    explicit FileSearcher(const wchar_t* filename)
        : target(filename),
          targetLength(wcslen(filename))
    {
    }


    uint64_t Search(
        const wchar_t* root,
        unsigned int   workerCount)
    {
        if (workerCount == 0)
            workerCount = 1;

        size_t rootLen = wcslen(root);

        if (rootLen > 3 && root[rootLen - 1] == L'\\')
            --rootLen;

        directories.Push(
            arena.Store(root, rootLen)
        );

        std::vector<std::thread> workers;

        workers.reserve(workerCount);

        for (unsigned int i = 0; i < workerCount; ++i)
        {
            workers.emplace_back(
                &FileSearcher::Worker,
                this
            );
        }

        directories.WaitForCompletion();

        for (auto& worker : workers)
        {
            if (worker.joinable())
                worker.join();
        }

        return matches.load(
            std::memory_order_relaxed
        );
    }


    uint64_t GetMatches() const
    {
        return matches.load(
            std::memory_order_relaxed
        );
    }


    uint64_t GetDirectoriesScanned() const
    {
        return directoriesScanned.load(
            std::memory_order_relaxed
        );
    }


    uint64_t GetFilesScanned() const
    {
        return filesScanned.load(
            std::memory_order_relaxed
        );
    }
};

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 3)
    {
        wprintf(
            L"Usage: %ls <Directory> <FileName>\n\n"
            L"Example:\n"
            L"  %ls C:\\ whoami.exe\n",
            argv[0],
            argv[0]
        );

        return 1;
    }

    // Resolve NtQueryDirectoryFile from ntdll
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");

    if (!ntdll)
    {
        wprintf(L"[-] Failed to get ntdll.dll\n");
        return 1;
    }

    pNtQueryDirectoryFile = reinterpret_cast<FN_NtQueryDirectoryFile>(
        GetProcAddress(ntdll, "NtQueryDirectoryFile")
    );

    if (!pNtQueryDirectoryFile)
    {
        wprintf(L"[-] NtQueryDirectoryFile not found\n");
        return 1;
    }

    const wchar_t* root     = argv[1];
    const wchar_t* filename = argv[2];

    unsigned int workers =
        std::thread::hardware_concurrency();

    if (workers == 0)
        workers = 4;

    if (workers > 16)
        workers = 16;

    wprintf(
        L"[*] Searching : %ls\n"
        L"[*] Filename  : %ls\n"
        L"[*] Workers   : %u\n\n",
        root,
        filename,
        workers
    );

    // High-resolution timing
    LARGE_INTEGER freq, start, end;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    FileSearcher searcher(filename);

    const uint64_t matchCount =
        searcher.Search(
            root,
            workers
        );

    QueryPerformanceCounter(&end);

    const double elapsed =
        static_cast<double>(end.QuadPart - start.QuadPart) /
        static_cast<double>(freq.QuadPart);

    wprintf(
        L"\n"
        L"[*] Search complete.\n"
        L"[*] Directories scanned : %llu\n"
        L"[*] Files scanned       : %llu\n"
        L"[*] Matches             : %llu\n"
        L"[*] Elapsed time        : %.3f s\n",
        searcher.GetDirectoriesScanned(),
        searcher.GetFilesScanned(),
        matchCount,
        elapsed
    );

    return matchCount > 0 ? 0 : 1;
}